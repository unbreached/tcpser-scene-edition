/* tcpser console client (5250-style). No dependencies.
 *   GET  /api/status            full snapshot
 *   GET  /api/events            SSE "status" events (same JSON)
 *   POST /api/line/<n>/hangup   hang up the caller on line n   (X-Requested-With: tcpser)
 *   POST /api/unblock           clear timed blocks
 * Screens: F2 status, F3 callers, F4 traffic, F6 security (also keys 1-4).
 * Everything caller-derived is rendered with textContent, never innerHTML. */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const pad = (s, n, right) => { s = String(s); if (s.length >= n) return s.slice(0, n); return right ? s.padStart(n) : s.padEnd(n); };
  const fmtBytes = (n) => n < 1024 ? n + "B" : n < 1048576 ? (n / 1024).toFixed(1) + "K" : (n / 1048576).toFixed(1) + "M";
  const fmtDur = (s) => { s = Math.max(0, s | 0); const h = (s / 3600) | 0, m = ((s % 3600) / 60) | 0, x = s % 60;
    return h ? `${h}:${String(m).padStart(2, "0")}:${String(x).padStart(2, "0")}` : `${String(m).padStart(2, "0")}:${String(x).padStart(2, "0")}`; };
  const fmtUp = (s) => { const d = (s / 86400) | 0, h = ((s % 86400) / 3600) | 0, m = ((s % 3600) / 60) | 0; return (d ? d + "D " : "") + String(h).padStart(2, "0") + ":" + String(m).padStart(2, "0"); };
  const timeOf = (iso) => { const d = new Date(iso); return isNaN(d) ? "--:--:--" : d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false }); };
  const dateOf = (iso) => { const d = new Date(iso); return isNaN(d) ? "" : d.toLocaleDateString(); };
  const BLOCKS = " ▁▂▃▄▅▆▇█";
  const PAGES = ["status", "callers", "traffic", "security"];
  const PAGE_KEYS = { F2: "status", F3: "callers", F4: "traffic", F6: "security", "1": "status", "2": "callers", "3": "traffic", "4": "security" };

  let mode = "petscii", paused = false, lastData = null, frozen = null, pendingAsk = null, es = null, page = "status";

  // ---- per-browser preferences (phosphor, screen) -------------------------
  const PHOS = ["green", "amber", "white"];
  function pref(k, v) { try { if (v === undefined) return localStorage.getItem("tcpser." + k); localStorage.setItem("tcpser." + k, v); } catch (e) { return null; } return null; }
  function setPhosphor(p, save) { document.documentElement.setAttribute("data-phosphor", p); if (save) pref("phosphor", p); }
  { const p = pref("phosphor"); if (PHOS.includes(p)) setPhosphor(p, false); }

  // ---- screens -----------------------------------------------------------
  function showPage(p, save) {
    if (!PAGES.includes(p)) return;
    page = p;
    document.querySelectorAll(".page").forEach((el) => { el.hidden = el.dataset.page !== p; });
    document.querySelectorAll("#tabs button").forEach((b) => b.classList.toggle("on", b.dataset.page === p));
    if (save) pref("page", p);
    if (lastData) render(lastData);
  }
  document.querySelectorAll("#tabs button").forEach((b) => { b.onclick = () => { showPage(b.dataset.page, true); msg(""); }; });

  // ---- message line ------------------------------------------------------
  function msg(text, cls) { const m = $("msg"); m.textContent = text || ""; m.className = "msgline" + (cls ? " " + cls : ""); }
  function ask(text, onYes) { pendingAsk = onYes; msg(text + "  (Y/N)", "ask"); }
  function answer(yes) { const fn = pendingAsk; pendingAsk = null; msg(""); if (yes && fn) fn(); }

  // ---- PETSCII rendering -------------------------------------------------
  const CTL = { 0x05: "WHT", 0x0d: "⏎", 0x0a: "␊", 0x0e: "LC", 0x8e: "UC", 0x11: "↓", 0x91: "↑", 0x1d: "→", 0x9d: "←", 0x12: "RVS", 0x92: "/RVS",
    0x13: "HOME", 0x93: "CLR", 0x14: "DEL", 0x94: "INS", 0x1c: "RED", 0x1e: "GRN", 0x1f: "BLU", 0x81: "ORG", 0x90: "BLK", 0x95: "BRN", 0x96: "LRD",
    0x97: "GR1", 0x98: "GR2", 0x99: "LGN", 0x9a: "LBL", 0x9b: "GR3", 0x9c: "PUR", 0x9e: "YEL", 0x9f: "CYN", 0x07: "BEL", 0x08: "BS", 0x09: "TAB", 0x1b: "ESC" };
  const GFX = { 0xa0: " ", 0xa6: "▒", 0xa7: "▕", 0xa9: "◤", 0xac: "▗", 0xad: "└", 0xae: "┐", 0xaf: "▂", 0xb0: "┌", 0xb1: "┴", 0xb2: "┬", 0xb3: "┤", 0xb4: "▎",
    0xb5: "▍", 0xb6: "▌", 0xb7: "▔", 0xb8: "▁", 0xb9: "▃", 0xba: "✓", 0xbb: "▖", 0xbc: "▝", 0xbd: "┘", 0xbe: "▘", 0xbf: "▚", 0xc0: "─", 0xdb: "┼", 0xdd: "│",
    0xde: "π", 0xdf: "◥", 0xa1: "▌", 0xa2: "▄", 0xa3: "▔", 0xa4: "▁", 0xa5: "▏", 0xa8: "▇", 0xaa: "▉", 0xab: "├" };
  const b64 = (s) => { const bin = atob(s); const a = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i); return a; };
  function renderBytes(bytes, into) {
    into.textContent = "";
    if (mode === "hex") { into.textContent = Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join(" "); return; }
    let run = ""; const flush = () => { if (run) { into.appendChild(document.createTextNode(run)); run = ""; } };
    for (const b of bytes) {
      if (mode === "ascii") { const c = b & 0x7f; run += (c >= 0x20 && c < 0x7f) ? String.fromCharCode(c) : "."; continue; }
      if (CTL[b] !== undefined) { flush(); const s = document.createElement("span"); s.className = "ctl"; s.textContent = "⟨" + CTL[b] + "⟩"; into.appendChild(s); continue; }
      if (b >= 0x20 && b <= 0x5a) run += String.fromCharCode(b);
      else if (b >= 0x61 && b <= 0x7a) run += String.fromCharCode(b - 0x20);
      else if (b >= 0xc1 && b <= 0xda) run += String.fromCharCode(b - 0x80);
      else if (GFX[b]) run += GFX[b];
      else if (b >= 0x60 && b <= 0x7f) run += GFX[b + 0x40] || "▩";
      else run += "·";
    }
    flush();
  }

  // ---- charts: block glyph bars on the character grid --------------------
  function bars(el, values, opts) {
    const max = Math.max(1, ...values);
    el.textContent = "";
    values.forEach((v, i) => {
      const lvl = v === 0 ? 0 : Math.max(1, Math.round((v / max) * 8));
      const s = document.createElement("span");
      s.textContent = BLOCKS[lvl];
      if (i === values.length - 1) s.className = "now";
      s.title = (opts.label ? opts.label(i) + ": " : "") + (opts.fmt ? opts.fmt(v) : v);
      el.appendChild(s);
    });
  }
  function renderCharts(d) {
    const s = d.series; if (!s) return;
    const calls = s.hour_calls, fails = s.hour_fail;
    const hourOf = (i) => String(((s.hour_now - (23 - i)) % 24 + 24) % 24).padStart(2, "0");
    bars($("ch-calls"), calls, { label: (i) => "hour " + hourOf(i), fmt: (v) => v + " call" + (v === 1 ? "" : "s") });
    bars($("ch-fail"), fails, { label: (i) => "hour " + hourOf(i), fmt: (v) => v + " fail" + (v === 1 ? "" : "s") });
    let axs = ""; for (let i = 0; i < 24; i++) { if (i % 6 === 0) { axs += hourOf(i); i++; } else axs += " "; }
    $("ax-hours").textContent = axs.slice(0, 24) + "  hour of day";
    const sumC = calls.reduce((a, b) => a + b, 0), sumF = fails.reduce((a, b) => a + b, 0), peak = Math.max(0, ...calls);
    $("calls-sum").textContent = `${sumC} TOTAL · PEAK ${peak}/H`;
    $("fail-sum").textContent = `${sumF} TOTAL`;
    // narrow screens get the last 30 minutes so the chart stays on the grid
    const narrow = window.innerWidth < 700, win = narrow ? 30 : 60;
    const mIn = s.min_in.slice(-win), mOut = s.min_out.slice(-win);
    bars($("ch-in"), mIn, { label: (i) => (win - 1 - i) + " min ago", fmt: fmtBytes });
    bars($("ch-out"), mOut, { label: (i) => (win - 1 - i) + " min ago", fmt: fmtBytes });
    $("ax-min").textContent = narrow ? "-30m      -20       -10       now" : "-60m      -50       -40       -30       -20       -10       now";
    const tIn = s.min_in.reduce((a, b) => a + b, 0), tOut = s.min_out.reduce((a, b) => a + b, 0);
    $("traf-sum").textContent = `${win} MIN  ↓${fmtBytes(tIn)} ↑${fmtBytes(tOut)}`;
    $("act-sub").textContent = "AS OF " + timeOf(d.now) + " " + dateOf(d.now);
  }

  // ---- records ------------------------------------------------------------
  function span(cls, text) { const s = document.createElement("span"); if (cls) s.className = cls; s.textContent = text; return s; }
  function renderLines(lines) {
    const host = $("lines"); host.textContent = "";
    lines.forEach((l) => {
      const pre = document.createElement("pre"); pre.className = "rec";
      pre.append(pad(l.id, 2, true) + " " + pad(l.device, 17) + " " + pad(l.speed, 5, true) + " ");
      pre.append(span(l.state === "connected" ? "st-on" : "st-off", pad(l.state === "connected" ? "CARRIER" : "IDLE", 8)));
      pre.append(" " + pad(l.state === "connected" ? l.peer : "", 16) + " " + pad(l.state === "connected" ? fmtDur(l.session_s) : "", 5, true) + " ");
      pre.append(pad(l.state === "connected" ? fmtBytes(l.bytes_in) : "", 6, true) + " " + pad(l.state === "connected" ? fmtBytes(l.bytes_out) : "", 7, true) + "  ");
      pre.append(span(l.dcd ? "led" : "led-off", l.dcd ? "●" : "○"), "   ", span(l.dtr ? "led" : "led-off", l.dtr ? "●" : "○"));
      if (l.state === "connected") {
        pre.append("  ");
        const b = document.createElement("button"); b.type = "button"; b.className = "act"; b.textContent = "F8=HANG UP";
        b.onclick = () => ask(`HANG UP LINE ${l.id} (${l.peer})?`, () => act(`/api/line/${l.id}/hangup`, `LINE ${l.id} HUNG UP`));
        pre.append(b);
      }
      host.appendChild(pre);
    });
    if (!lines.length) host.append(span("empty", "NO LINES CONFIGURED"));
  }
  function renderCallers(hist) {
    const host = $("callers"); host.textContent = ""; $("callers-empty").hidden = hist.length > 0;
    hist.forEach((h) => {
      const pre = document.createElement("pre"); pre.className = "rec";
      pre.append(pad(timeOf(h.time), 8) + " " + pad(dateOf(h.time), 10) + " " + pad(h.ip, 16) + " ");
      pre.append(span("res-" + h.result, pad(h.result.replace("_", " "), 13)));
      pre.append(" " + pad(h.duration ? fmtDur(h.duration) : "", 5, true) + " " + pad(h.bytes_in ? fmtBytes(h.bytes_in) : "", 6, true) + " " + pad(h.bytes_out ? fmtBytes(h.bytes_out) : "", 7, true));
      host.appendChild(pre);
    });
  }
  function renderTraffic(d) {
    const feed = $("feed"); const list = paused && frozen ? frozen : d.traffic;
    $("traffic-note").hidden = !!d.traffic_content; $("feed-empty").hidden = list.length > 0;
    const atBottom = feed.scrollHeight - feed.scrollTop - feed.clientHeight < 8;
    feed.textContent = "";
    list.forEach((t) => {
      const li = document.createElement("li");
      li.append(span("dir " + t.dir, t.dir === "in" ? "CALLER→BBS" : "BBS→CALLER"), span("n num", t.bytes + "B"));
      const x = span("t", ""); if (t.raw) renderBytes(b64(t.raw), x); li.append(x); feed.appendChild(li);
    });
    if (atBottom) feed.scrollTop = feed.scrollHeight;
  }
  function renderBlocklist(bl) {
    const ul = $("blocklist"); ul.textContent = ""; $("block-empty").hidden = bl.length > 0; $("block-count").textContent = bl.length ? String(bl.length) : "";
    $("fkeys").querySelector('[data-f="F9"]').disabled = !bl.some((b) => b.kind === "timed");
    bl.forEach((b) => { const li = document.createElement("li"); li.append(span("ip", b.ip), span("kind " + b.kind, b.kind === "permanent" ? "PERMANENT" : "TIMED, UNTIL " + timeOf(b.expires))); ul.appendChild(li); });
    $("c-blocked").textContent = bl.length;
  }
  function renderEvents(ev) {
    [["events", "events-empty", ev], ["events-short", "events-short-empty", ev.slice(0, 5)]].forEach(([id, emptyId, list]) => {
      const ul = $(id); ul.textContent = ""; $(emptyId).hidden = list.length > 0;
      list.forEach((e) => { const li = document.createElement("li"); li.append(span("time num", timeOf(e.time)), span("", e.text.toUpperCase())); ul.appendChild(li); });
    });
  }
  function render(d) {
    lastData = d;
    $("bbs-name").textContent = d.name || "TCPSER CONSOLE"; document.title = (d.name || "tcpser") + " console";
    $("version").textContent = d.version; $("uptime").textContent = fmtUp(d.uptime_s);
    $("c-conn").textContent = d.stats.total_connections; $("c-pass").textContent = d.stats.gate_pass; $("c-fail").textContent = d.stats.gate_fail;
    $("c-drop").textContent = d.stats.dropped; $("c-in").textContent = fmtBytes(d.stats.bytes_in); $("c-out").textContent = fmtBytes(d.stats.bytes_out);
    const gateText = d.gate.mode.toUpperCase() + (d.gate.mode !== "off" ? ` ${d.gate.timeout}S / BLOCK AFTER ${d.gate.fails}` : "");
    $("gate").textContent = gateText; $("g-mode").textContent = d.gate.mode.toUpperCase(); $("g-timeout").textContent = d.gate.mode !== "off" ? d.gate.timeout + " S" : "-"; $("g-fails").textContent = d.gate.mode !== "off" ? d.gate.fails + " FAILURES" : "-";
    $("listen").textContent = ":" + d.listen_port;
    $("clock").textContent = dateOf(d.now) + " " + timeOf(d.now);
    // render only what is on screen (plus the cheap bits), the rest on page switch
    if (page === "status") { renderLines(d.lines); renderCharts(d); }
    if (page === "callers") renderCallers(d.history);
    if (page === "traffic") renderTraffic(d);
    renderBlocklist(d.blocklist); renderEvents(d.events);
    $("fkeys").querySelector('[data-f="F8"]').disabled = !d.lines.some((l) => l.state === "connected");
  }

  // ---- transport ---------------------------------------------------------
  function setLink(text, cls) { const e = $("live-text"); e.textContent = text; e.className = cls || ""; }
  function locked(is) { $("locked").hidden = !is; if (is) setLink("SIGN-ON REQUIRED", "warn"); }
  async function refresh() {
    if (window.__TCPSER_SNAPSHOT__) { if (lastData) render(lastData); return; }
    try { const r = await fetch("/api/status"); if (r.status === 401) { locked(true); return; } if (r.ok) { render(await r.json()); locked(false); } } catch (e) { /* down */ }
  }
  { const p = pref("page"); showPage(PAGES.includes(p) ? p : "status", false); }
  if (window.__TCPSER_SNAPSHOT__) { render(window.__TCPSER_SNAPSHOT__); setLink("SNAPSHOT", "warn"); msg("STATIC SNAPSHOT. ACTIONS ARE INERT. F2/F3/F4/F6 OR 1-4 SWITCH SCREENS."); }
  else {
    const connect = () => {
      if (es) es.close();
      es = new EventSource("/api/events");
      es.addEventListener("status", (ev) => { setLink("LIVE", "ok"); locked(false); try { render(JSON.parse(ev.data)); } catch (e) { /* torn frame */ } });
      es.onopen = () => setLink("LIVE", "ok");
      es.onerror = () => { setLink("RECONNECTING", "warn"); fetch("/api/status").then((r) => { if (r.status === 401) { locked(true); es.close(); } }).catch(() => {}); };
    };
    refresh(); connect();
  }
  async function act(path, okText) {
    if (window.__TCPSER_SNAPSHOT__) { msg("SNAPSHOT: ACTION NOT SENT", "err"); return; }
    try {
      const r = await fetch(path, { method: "POST", headers: { "X-Requested-With": "tcpser" } });
      if (!r.ok) throw new Error(r.status + " " + r.statusText);
      msg(okText); refresh();
    } catch (e) { msg("ACTION FAILED: " + e.message, "err"); }
  }

  // ---- function keys -----------------------------------------------------
  function fkey(k) {
    if (PAGE_KEYS[k]) { showPage(PAGE_KEYS[k], true); msg(""); return; }
    if (k === "F5") { msg("REFRESHED"); refresh(); return; }
    if (k === "F7") { const cur = document.documentElement.getAttribute("data-phosphor") || "green"; const next = PHOS[(PHOS.indexOf(cur) + 1) % PHOS.length]; setPhosphor(next, true); msg("PHOSPHOR: " + next.toUpperCase()); return; }
    if (k === "F8") { const l = lastData && lastData.lines.find((x) => x.state === "connected"); if (!l) { msg("NO CALLER ONLINE", "err"); return; } ask(`HANG UP LINE ${l.id} (${l.peer})?`, () => act(`/api/line/${l.id}/hangup`, `LINE ${l.id} HUNG UP`)); return; }
    if (k === "F9") { if (!lastData || !lastData.blocklist.some((b) => b.kind === "timed")) { msg("NO TIMED BLOCKS", "err"); return; } ask("CLEAR ALL TIMED BLOCKS?", () => act("/api/unblock", "TIMED BLOCKS CLEARED")); return; }
    if (k === "F10") { if (!window.__TCPSER_SNAPSHOT__) location.href = "/metrics"; else msg("SNAPSHOT: NO METRICS ENDPOINT", "err"); return; }
    if (k === "F12") { ask("EXIT THE CONSOLE?", () => { $("crt").textContent = ""; document.body.style.background = "#000"; }); }
  }
  document.querySelectorAll(".fkeys button").forEach((b) => { b.onclick = () => fkey(b.dataset.f); const t = b.textContent.split("="); b.textContent = ""; const k = document.createElement("b"); k.textContent = t[0] + "="; b.append(k, t[1] || ""); });
  document.addEventListener("keydown", (e) => {
    if (e.target && /^(INPUT|TEXTAREA)$/.test(e.target.tagName)) return;
    if (pendingAsk !== null) { if (e.key === "y" || e.key === "Y") { e.preventDefault(); answer(true); } else if (e.key === "n" || e.key === "N" || e.key === "Escape") { e.preventDefault(); answer(false); } return; }
    if (PAGE_KEYS[e.key] || /^F(5|7|8|9|10|12)$/.test(e.key)) { e.preventDefault(); fkey(e.key); }
  });
  document.querySelectorAll(".opt[data-mode]").forEach((b) => b.onclick = () => { mode = b.dataset.mode; document.querySelectorAll(".opt[data-mode]").forEach((x) => x.classList.toggle("on", x === b)); if (lastData) renderTraffic(lastData); });
  $("pause").onclick = () => { paused = !paused; frozen = paused && lastData ? lastData.traffic : null; $("pause").setAttribute("aria-pressed", String(paused)); $("pause").textContent = paused ? "RELEASE" : "HOLD"; msg(paused ? "TRAFFIC FEED ON HOLD" : ""); if (lastData) renderTraffic(lastData); };
  window.addEventListener("resize", () => { if (lastData && page === "status") renderCharts(lastData); });
})();
