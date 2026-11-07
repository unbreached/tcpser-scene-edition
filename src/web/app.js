/* tcpser web dashboard client. No dependencies. Talks to:
 *   GET  /api/status            full snapshot (JSON)
 *   GET  /api/events            Server-Sent Events: "status" events with the same JSON
 *   POST /api/line/<n>/hangup   hang up the caller on line n
 *   POST /api/unblock           clear timed blocks
 * Auth is a cookie set by visiting /?token=...; the page itself never sees the token.
 * Everything caller-derived is rendered with textContent (never innerHTML). */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const fmtBytes = (n) => n < 1024 ? n + " B" : n < 1048576 ? (n / 1024).toFixed(1) + " KB" : (n / 1048576).toFixed(1) + " MB";
  const fmtDur = (s) => { s = Math.max(0, s | 0); const h = (s / 3600) | 0, m = ((s % 3600) / 60) | 0, x = s % 60;
    return h ? `${h}h ${String(m).padStart(2, "0")}m` : m ? `${m}m ${String(x).padStart(2, "0")}s` : `${x}s`; };
  const fmtUp = (s) => { const d = (s / 86400) | 0, h = ((s % 86400) / 3600) | 0, m = ((s % 3600) / 60) | 0;
    return (d ? d + "d " : "") + String(h).padStart(2, "0") + ":" + String(m).padStart(2, "0"); };
  const timeOf = (iso) => { const d = new Date(iso); return isNaN(d) ? iso : d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" }); };
  const dateOf = (iso) => { const d = new Date(iso); return isNaN(d) ? "" : d.toLocaleDateString(); };
  const today = () => new Date().toLocaleDateString();

  let mode = "petscii";
  let paused = false;
  let lastData = null;
  let frozenTraffic = null;

  // ---- PETSCII rendering -------------------------------------------------
  const CTL = {
    0x05: "WHT", 0x0d: "⏎", 0x0a: "␊", 0x0e: "LC", 0x8e: "UC", 0x11: "↓", 0x91: "↑", 0x1d: "→", 0x9d: "←",
    0x12: "RVS", 0x92: "/RVS", 0x13: "HOME", 0x93: "CLR", 0x14: "DEL", 0x94: "INS", 0x1c: "RED", 0x1e: "GRN",
    0x1f: "BLU", 0x81: "ORG", 0x90: "BLK", 0x95: "BRN", 0x96: "LRD", 0x97: "GR1", 0x98: "GR2", 0x99: "LGN",
    0x9a: "LBL", 0x9b: "GR3", 0x9c: "PUR", 0x9e: "YEL", 0x9f: "CYN", 0x07: "BEL", 0x08: "BS", 0x09: "TAB", 0x1b: "ESC",
  };
  // Shifted/graphics PETSCII -> rough Unicode shapes (good enough to recognise a banner)
  const GFX = { 0xa0: " ", 0xa6: "▒", 0xa7: "▕", 0xa9: "◤", 0xac: "▗", 0xad: "└", 0xae: "┐", 0xaf: "▂", 0xb0: "┌", 0xb1: "┴",
    0xb2: "┬", 0xb3: "┤", 0xb4: "▎", 0xb5: "▍", 0xb6: "▌", 0xb7: "▔", 0xb8: "▁", 0xb9: "▃", 0xba: "✓", 0xbb: "▖", 0xbc: "▝",
    0xbd: "┘", 0xbe: "▘", 0xbf: "▚", 0xc0: "─", 0xdb: "┼", 0xdd: "│", 0xde: "π", 0xdf: "◥", 0xa1: "▌", 0xa2: "▄", 0xa3: "▔",
    0xa4: "▁", 0xa5: "▏", 0xa8: "▇", 0xaa: "▉", 0xab: "├" };
  function b64(s) { const bin = atob(s); const a = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i); return a; }
  function renderBytes(bytes, into) {
    into.textContent = "";
    if (mode === "hex") { into.textContent = Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join(" "); return; }
    let run = "";
    const flush = () => { if (run) { into.appendChild(document.createTextNode(run)); run = ""; } };
    for (const b of bytes) {
      if (mode === "ascii") { const c = b & 0x7f; run += (c >= 0x20 && c < 0x7f) ? String.fromCharCode(c) : "."; continue; }
      if (CTL[b] !== undefined) { flush(); const s = document.createElement("span"); s.className = "ctl"; s.textContent = "⟨" + CTL[b] + "⟩"; into.appendChild(s); continue; }
      if (b >= 0x20 && b < 0x41) run += String.fromCharCode(b);                 // digits, punctuation
      else if (b >= 0x41 && b <= 0x5a) run += String.fromCharCode(b);           // A-Z (set 1 shows uppercase)
      else if (b >= 0x61 && b <= 0x7a) run += String.fromCharCode(b - 0x20);    // PETSCII lowercase range -> uppercase glyphs in set 1
      else if (b >= 0xc1 && b <= 0xda) run += String.fromCharCode(b - 0x80);    // shifted letters
      else if (GFX[b]) run += GFX[b];
      else if (b >= 0x60 && b <= 0x7f) run += GFX[b + 0x40] || "▩";
      else run += "·";
    }
    flush();
  }

  // ---- rendering ---------------------------------------------------------
  function renderLines(lines) {
    const host = $("lines"); host.textContent = "";
    lines.forEach((l) => {
      const row = document.createElement("div"); row.className = "line " + (l.state === "connected" ? "connected" : "idle");
      const id = document.createElement("div"); id.className = "id"; id.textContent = "LINE " + l.id;
      const main = document.createElement("div"); main.className = "main";
      const dev = document.createElement("span"); dev.className = "dev"; dev.textContent = `${l.device} @ ${l.speed}`;
      const st = document.createElement("span"); st.className = "state";
      st.textContent = l.state === "connected" ? "● CARRIER" : "○ awaiting carrier";
      main.append(dev, st);
      if (l.state === "connected") {
        const peer = document.createElement("span"); peer.className = "peer"; peer.textContent = l.peer || "?";
        const meta = document.createElement("span"); meta.className = "meta num";
        meta.textContent = `${fmtDur(l.session_s)}  ↓${fmtBytes(l.bytes_in)} ↑${fmtBytes(l.bytes_out)}`;
        main.append(peer, meta);
      }
      const side = document.createElement("div"); side.className = "side";
      const leds = document.createElement("div"); leds.className = "leds";
      [["DCD", l.dcd], ["DTR", l.dtr]].forEach(([k, on]) => { const s = document.createElement("span"); s.className = on ? "on" : ""; s.textContent = (on ? "●" : "○") + " " + k; leds.appendChild(s); });
      side.appendChild(leds);
      if (l.state === "connected") {
        const b = document.createElement("button"); b.className = "btn danger"; b.type = "button"; b.textContent = "Hang up";
        b.onclick = () => { if (confirm(`Hang up the caller on line ${l.id} (${l.peer})?`)) act(`/api/line/${l.id}/hangup`); };
        side.appendChild(b);
      }
      row.append(id, main, side); host.appendChild(row);
    });
  }
  function renderCallers(hist) {
    const tb = $("callers").querySelector("tbody"); tb.textContent = "";
    $("callers-empty").hidden = hist.length > 0;
    hist.forEach((h) => {
      const tr = document.createElement("tr");
      const t = document.createElement("td"); t.className = "num";
      t.textContent = timeOf(h.time); if (dateOf(h.time) && dateOf(h.time) !== today()) { const d = document.createElement("span"); d.className = "date"; d.textContent = " " + dateOf(h.time); t.appendChild(d); }
      const ip = document.createElement("td"); ip.textContent = h.ip;
      const r = document.createElement("td"); r.className = "res " + h.result; r.textContent = h.result.replace("_", " ");
      const du = document.createElement("td"); du.className = "r num"; du.textContent = h.duration ? fmtDur(h.duration) : "";
      const bi = document.createElement("td"); bi.className = "r num"; bi.textContent = h.bytes_in ? fmtBytes(h.bytes_in) : "";
      const bo = document.createElement("td"); bo.className = "r num"; bo.textContent = h.bytes_out ? fmtBytes(h.bytes_out) : "";
      tr.append(t, ip, r, du, bi, bo); tb.appendChild(tr);
    });
  }
  function renderTraffic(data) {
    const feed = $("feed"); const list = paused && frozenTraffic ? frozenTraffic : data.traffic;
    $("traffic-note").hidden = !!data.traffic_content;
    $("feed-empty").hidden = list.length > 0;
    const atBottom = feed.scrollHeight - feed.scrollTop - feed.clientHeight < 8;
    feed.textContent = "";
    list.forEach((t) => {
      const li = document.createElement("li");
      const d = document.createElement("span"); d.className = "dir " + t.dir; d.textContent = t.dir === "in" ? "CALLER→BBS" : "BBS→CALLER";
      const n = document.createElement("span"); n.className = "n num"; n.textContent = t.bytes + "B";
      const x = document.createElement("span"); x.className = "t petscii";
      if (t.raw) renderBytes(b64(t.raw), x); else x.textContent = "";
      li.append(d, n, x); feed.appendChild(li);
    });
    if (atBottom) feed.scrollTop = feed.scrollHeight;
  }
  function renderBlocklist(bl) {
    const ul = $("blocklist"); ul.textContent = ""; $("block-empty").hidden = bl.length > 0; $("block-count").textContent = bl.length ? "(" + bl.length + ")" : "";
    $("unblock").disabled = !bl.some((b) => b.kind === "timed");
    bl.forEach((b) => {
      const li = document.createElement("li");
      const ip = document.createElement("span"); ip.className = "ip"; ip.textContent = b.ip;
      const k = document.createElement("span"); k.className = "kind " + b.kind; k.textContent = b.kind === "permanent" ? "permanent" : "timed, until " + timeOf(b.expires);
      li.append(ip, k); ul.appendChild(li);
    });
  }
  function renderEvents(ev) {
    const ul = $("events"); ul.textContent = ""; $("events-empty").hidden = ev.length > 0;
    ev.forEach((e) => { const li = document.createElement("li"); const t = document.createElement("span"); t.className = "time num"; t.textContent = timeOf(e.time);
      const x = document.createElement("span"); x.textContent = e.text; li.append(t, x); ul.appendChild(li); });
  }
  function render(data) {
    lastData = data;
    $("bbs-name").textContent = data.name || "TCPSER";
    document.title = (data.name || "tcpser") + " · dashboard";
    $("version").textContent = data.version;
    $("uptime").textContent = fmtUp(data.uptime_s);
    $("c-conn").textContent = data.stats.total_connections;
    $("c-pass").textContent = data.stats.gate_pass;
    $("c-fail").textContent = data.stats.gate_fail;
    $("c-drop").textContent = data.stats.dropped;
    $("c-in").textContent = fmtBytes(data.stats.bytes_in);
    $("c-out").textContent = fmtBytes(data.stats.bytes_out);
    $("gate").textContent = "gate " + data.gate.mode + (data.gate.mode !== "off" ? ` (${data.gate.timeout}s, block after ${data.gate.fails})` : "");
    $("listen").textContent = ":" + data.listen_port;
    $("clock").textContent = new Date(data.now).toLocaleString();
    renderLines(data.lines); renderCallers(data.history); renderTraffic(data); renderBlocklist(data.blocklist); renderEvents(data.events);
  }

  // ---- transport ---------------------------------------------------------
  function setLive(on, text) { $("live-dot").className = "dot " + (on ? "on" : "off"); $("live-text").textContent = text; }
  // Static snapshot mode (docs/screenshots): a page with window.__TCPSER_SNAPSHOT__
  // set renders that data and never opens a connection.
  if (window.__TCPSER_SNAPSHOT__) { render(window.__TCPSER_SNAPSHOT__); setLive(true, "SNAPSHOT"); return; }
  async function act(path) {
    try {
      const r = await fetch(path, { method: "POST", headers: { "X-Requested-With": "tcpser" } });
      if (!r.ok) throw new Error(r.status + " " + r.statusText);
      const d = await fetch("/api/status"); if (d.ok) render(await d.json());
    } catch (e) { alert("Action failed: " + e.message); }
  }
  function locked(is) { $("locked").hidden = !is; if (is) setLive(false, "locked"); }
  let es = null;
  function connect() {
    if (es) es.close();
    es = new EventSource("/api/events");
    es.addEventListener("status", (ev) => { setLive(true, "LIVE"); locked(false); try { render(JSON.parse(ev.data)); } catch (e) { /* ignore a torn frame */ } });
    es.onopen = () => setLive(true, "LIVE");
    es.onerror = () => {
      setLive(false, "reconnecting…");
      // A 401 closes the stream immediately: probe once to tell "locked" from "down".
      fetch("/api/status").then((r) => { if (r.status === 401) { locked(true); es.close(); } }).catch(() => {});
    };
  }
  fetch("/api/status").then((r) => { if (r.status === 401) { locked(true); return null; } return r.json(); }).then((d) => { if (d) render(d); }).catch(() => {});
  connect();

  // ---- controls ----------------------------------------------------------
  document.querySelectorAll(".seg[data-mode]").forEach((b) => b.onclick = () => {
    mode = b.dataset.mode; document.querySelectorAll(".seg[data-mode]").forEach((x) => x.classList.toggle("on", x === b));
    if (lastData) renderTraffic(lastData);
  });
  $("pause").onclick = () => { paused = !paused; frozenTraffic = paused && lastData ? lastData.traffic : null; $("pause").setAttribute("aria-pressed", String(paused)); $("pause").textContent = paused ? "▶ resume" : "⏸ pause"; if (lastData) renderTraffic(lastData); };
  $("unblock").onclick = () => { if (confirm("Clear all timed blocks?")) act("/api/unblock"); };
})();
