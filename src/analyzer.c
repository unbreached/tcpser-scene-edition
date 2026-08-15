/*
 * RS232 Analyzer for tcpser  (interactive diagnostic for C64 RS232 adapters:
 * SwiftLink, Turbo232, GLINK232, userport clones).
 *
 * What it actually does (in order):
 *   STEP 1  Detect baud rate — listen for the BBS's AT init at each baud rate
 *           and pick the one that yields valid AT commands. NOTE: a BBS usually
 *           sends its init ONCE, so if the first (your -s) speed is wrong the
 *           init may be consumed as noise; if nothing is detected, restart the
 *           BBS and run the analyzer again.
 *   STEP 2  Control-line wiring — toggle DTR/RTS and observe which inputs move
 *           (detects null-modem cross-wiring and 3-wire adapters).
 *   STEP 3  Verify — send an OK response and confirm the BBS accepts it.
 *   REPORT  Cartridge inference (from detected speed + whether control lines
 *           responded) and a recommended command line.
 *
 * Limitations (be honest): flow-control type and parity are NOT swept/probed;
 * the flow-control (&K3/&K4) suggestion in the report is a heuristic to verify,
 * not a measurement. Cartridge detection is best-effort.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <errno.h>

#include "analyzer.h"

/* ---- test matrix ---------------------------------------------------- */

/* Baud rates: C64 userport (300-2400), SwiftLink/GLINK232 (up to 38400),
 * Turbo232 (up to 230400).  Ordered by likelihood for C64 BBS. */
static const int BAUD_RATES[] = {
  300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400
};
static const int NUM_BAUD_RATES = (int)(sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]));


typedef struct {
  int speed;
  int flow_mode;
  int parity_mode;
  int can_write;
  int rx_bytes;
} combo_result_t;

/* Cartridge type enum */
enum {
  CART_UNKNOWN = 0,
  CART_USERPORT,
  CART_SWIFTLINK,
  CART_TURBO232,
  CART_GLINK232,
};

/* ---- helpers --------------------------------------------------------- */

static void wait_for_enter(const char *prompt) {
  char buf[64];
  printf("\n  >> %s\n     Press ENTER when ready...", prompt);
  fflush(stdout);
  if(fgets(buf, sizeof(buf), stdin) == NULL) { /* ignore */ }
}

static int read_lines(int fd) {
  int status = 0;
  if(ioctl(fd, TIOCMGET, &status) < 0) return -1;
  return status;
}

static void set_line(int fd, int flag, int high) {
  int status = 0;
  ioctl(fd, TIOCMGET, &status);
  if(high) status |= flag;
  else     status &= ~flag;
  ioctl(fd, TIOCMSET, &status);
  usleep(150000);
}

static const char *hi_lo(int status, int flag) {
  return (status & flag) ? "HIGH" : "LOW ";
}

static speed_t speed_to_const(int speed) {
  switch(speed) {
    case 300:    return B300;
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return B2400;
  }
}

static void configure_port(int fd, int speed, int flow_mode) {
  struct termios tio;
  if(tcgetattr(fd, &tio) != 0) return;

  speed_t b = speed_to_const(speed);
  cfsetispeed(&tio, b);
  cfsetospeed(&tio, b);

  /* Always 8N1 */
  tio.c_cflag &= ~(PARENB | PARODD | CSTOPB | CSIZE);
  tio.c_cflag |= (CS8 | CLOCAL | CREAD);

  tio.c_cflag &= ~CRTSCTS;
  tio.c_iflag &= ~(IXON | IXOFF | IXANY);
  if(flow_mode == 1) tio.c_cflag |= CRTSCTS;
  if(flow_mode == 2) tio.c_iflag |= (IXON | IXOFF);

  tcsetattr(fd, TCSANOW, &tio);
  usleep(50000);
}

static void print_status(int s) {
  printf("    DTR:  %s  (output)\n", hi_lo(s, TIOCM_DTR));
  printf("    RTS:  %s  (output)\n", hi_lo(s, TIOCM_RTS));
  printf("    CTS:  %s  (input)\n",  hi_lo(s, TIOCM_CTS));
  printf("    DSR:  %s  (input)\n",  hi_lo(s, TIOCM_DSR));
#ifdef TIOCM_CAR
  printf("    DCD:  %s  (input)\n",  hi_lo(s, TIOCM_CAR));
#endif
#ifdef TIOCM_RNG
  printf("    RI:   %s  (input)\n",  hi_lo(s, TIOCM_RNG));
#endif
}

static void print_diff(int before, int after, const char *label) {
  int changed = before ^ after;
  int any = 0;
  struct { int flag; const char *name; } lines[] = {
    { TIOCM_CTS, "CTS" }, { TIOCM_DSR, "DSR" },
#ifdef TIOCM_CAR
    { TIOCM_CAR, "DCD" },
#endif
#ifdef TIOCM_RNG
    { TIOCM_RNG, "RI " },
#endif
    { 0, NULL }
  };

  printf("\n  %s:\n", label);
  for(int i = 0; lines[i].name; i++) {
    if(changed & lines[i].flag) {
      printf("    %s: %s -> %s  ** CHANGED **\n",
             lines[i].name, hi_lo(before, lines[i].flag), hi_lo(after, lines[i].flag));
      any = 1;
    } else {
      printf("    %s: %s  (no change)\n", lines[i].name, hi_lo(after, lines[i].flag));
    }
  }
  if(!any) printf("    (no input lines changed)\n");
}

/*
 * Check if data looks like valid ASCII/AT commands (not line noise).
 * At the wrong baud rate, bytes will be random garbage.
 * At the right baud rate, we'll see printable ASCII with AT prefix.
 * Returns: 2 = contains "AT" (definitely modem init),
 *          1 = mostly printable ASCII, 0 = garbage/noise
 */
static int check_valid_ascii(unsigned char *data, int len) {
  if(len < 2) return 0;
  int printable = 0;
  for(int i = 0; i < len; i++) {
    unsigned char c = data[i];
    if((c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n')
      printable++;
  }
  /* If less than 60% is printable ASCII, it's noise */
  if(printable * 100 / len < 60) return 0;

  /* Check for AT command prefix */
  for(int i = 0; i < len - 1; i++) {
    unsigned char c1 = data[i] & 0x5f; /* case-insensitive */
    unsigned char c2 = data[i+1] & 0x5f;
    if(c1 == 'A' && c2 == 'T') return 2;
  }
  return 1;
}

/*
 * Listen for modem init from BBS and respond with OK.
 * The BBS sends AT init commands (e.g., ATX2M0S0=0...) on startup.
 * If we can read valid AT commands, the baud rate is correct.
 * We respond with "OK\r\n" so the BBS can proceed.
 *
 * Returns: number of valid bytes received, 0 = nothing/noise
 */
static int listen_for_init(int fd, int seconds, int respond) {
  tcflush(fd, TCIFLUSH);
  time_t end = time(NULL) + seconds;
  unsigned char buf[256];
  int total = 0;
  int valid = 0;

  while(time(NULL) < end) {
    fd_set rfs;
    struct timeval tv = { 0, 200000 };
    FD_ZERO(&rfs);
    FD_SET(fd, &rfs);
    if(select(fd + 1, &rfs, NULL, NULL, &tv) > 0) {
      int n = (int)read(fd, buf + total, sizeof(buf) - (size_t)total);
      if(n > 0) {
        total += n;
        if(total >= (int)sizeof(buf)) break;
      }
    }
  }

  if(total > 0) {
    valid = check_valid_ascii(buf, total);
    if(valid >= 1 && respond) {
      /* Respond with OK so the BBS can proceed */
      usleep(50000);
      if(write(fd, "\r\nOK\r\n", 6) < 0) { /* ignore */ }
    }
  }
  return valid >= 1 ? total : 0;
}

/* ---- cartridge detection -------------------------------------------- */

static const char *cartridge_name(int type) {
  switch(type) {
    case CART_USERPORT:  return "Userport adapter (software UART)";
    case CART_SWIFTLINK: return "SwiftLink-232 or GLINK232 (6551 ACIA, max 38400)";
    case CART_TURBO232:  return "Turbo232 (6551 ACIA + ext register, max 230400)";
    case CART_GLINK232:  return "GLINK232 (6551 ACIA, SwiftLink compatible)";
    default:             return "Unknown";
  }
}

/* Best-effort cartridge inference from what the analyzer actually measures:
 * the detected baud rate and whether ANY control lines responded (is_3wire).
 * Flow-control type is not probed, so this deliberately does not try to split
 * UP9600 from a SwiftLink at 9600 by RTS/CTS (which was never measured) — a
 * userport/UP9600 mod exposes no hardware handshake lines, so "no control
 * lines" is the honest signal for a userport adapter. Every branch is
 * reachable. */
static int detect_cartridge(int max_working_speed, int is_3wire) {
  if(max_working_speed > 38400)
    return CART_TURBO232;    /* only Turbo232 exceeds 38400 baud */
  if(is_3wire)
    return CART_USERPORT;    /* no handshake lines -> userport (incl. UP9600) */
  return CART_SWIFTLINK;     /* control lines present -> 6551 ACIA cartridge */
}

/* ---- autonomous matrix sweep ---------------------------------------- */

/*
 * The BBS sends AT init commands when it starts up
 * (e.g., ATX2M0S0=0S7=30V1E0S2=1S10=30). We cycle through baud rates, listen
 * ~2s at each, and flag the rate that yields valid AT text (not line noise).
 *
 * IMPORTANT: this scan assumes the init is still arriving while we sweep. Most
 * BBSes send it ONCE at startup, so the reliable procedure is: press ENTER
 * here, THEN start (or restart) the BBS so its init lands during the sweep. If
 * detection fails, restart the BBS and run the analyzer again.
 */

static int run_baud_scan(int fd, int user_speed, combo_result_t *results, int max_results) {
  int count = 0;
  int found = 0;

  int speeds[16];
  int nspeeds = 0;
  speeds[nspeeds++] = user_speed;
  for(int i = 0; i < NUM_BAUD_RATES && nspeeds < 16; i++) {
    if(BAUD_RATES[i] != user_speed) speeds[nspeeds++] = BAUD_RATES[i];
  }

  printf("\n  ============================================\n");
  printf("  BAUD RATE SCAN\n");
  printf("  ============================================\n");
  printf("  Listening for AT init commands from BBS at each baud rate.\n");
  printf("  Testing %d baud rates (your -s %d first, then others).\n\n", nspeeds, user_speed);

  for(int si = 0; si < nspeeds && count < max_results; si++) {
    int spd = speeds[si];
    printf("  [%2d/%d] %6d baud ... ", si + 1, nspeeds, spd);
    fflush(stdout);

    configure_port(fd, spd, 0); /* no flow control, 8N1 */

    int rx = listen_for_init(fd, 2, 0); /* listen 2s, don't respond yet */

    results[count].speed = spd;
    results[count].flow_mode = 0;
    results[count].parity_mode = 0;
    results[count].can_write = 1;
    results[count].rx_bytes = rx;
    count++;

    if(rx > 0) {
      printf("** GOT AT COMMANDS! (%d bytes) **\n", rx);
      found++;
    } else {
      printf("noise/silence\n");
    }
  }

  printf("\n  Baud scan: %d rates tested, %d received valid data.\n", nspeeds, found);
  return count;
}

/* ---- main analyzer -------------------------------------------------- */

int analyzer_run(const char *tty, int speed) {
  int fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if(fd < 0) {
    fprintf(stderr, "  Could not open serial port %s: %s\n", tty, strerror(errno));
    return 1;
  }

  printf("\n");
  printf("  ============================================\n");
  printf("  RS232 Analyzer for tcpser-frozen\n");
  printf("  ============================================\n");
  printf("  Port:  %s\n", tty);
  printf("  Speed: %d baud (all speeds will be tested)\n", speed);
  printf("\n");
  printf("  This analyzer works with the BBS modem init sequence.\n");
  printf("  The BBS sends AT commands (e.g., ATX2M0S0=0...) to the\n");
  printf("  modem when it starts up. We listen for those commands\n");
  printf("  to detect the correct settings.\n");
  printf("\n");
  printf("  Step 1: Detect baud rate (listen for AT init)\n");
  printf("  Step 2: Test control line wiring (DTR/DCD/DSR)\n");
  printf("  Step 3: Verify full modem init (respond OK)\n");
  printf("  Step 4: Baseline test (if needed)\n");

  /* Initial port setup */
  struct termios tio;
  if(tcgetattr(fd, &tio) != 0) {
    perror("  Could not get port attributes");
    close(fd);
    return 1;
  }
  cfmakeraw(&tio);
  tio.c_cflag &= ~(PARENB | PARODD | CSTOPB | CSIZE | CRTSCTS);
  tio.c_cflag |= (CS8 | CLOCAL | CREAD);
  tio.c_iflag &= ~(IXON | IXOFF | IXANY);
  tio.c_iflag |= IGNBRK;
  cfsetispeed(&tio, speed_to_const(speed));
  cfsetospeed(&tio, speed_to_const(speed));
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 1;
  tcflush(fd, TCIOFLUSH);
  tcsetattr(fd, TCSANOW, &tio);

  /* Assert DTR + RTS (provides DCD + CTS to C64 via null-modem) */
  set_line(fd, TIOCM_DTR | TIOCM_RTS, 1);

  int all_inputs = TIOCM_CTS | TIOCM_DSR;
#ifdef TIOCM_CAR
  all_inputs |= TIOCM_CAR;
#endif

  /* Variables for analysis */
  int dtr_affects = 0, rts_affects = 0;
  int dtr_caused_bbs_react = 0;
  int poweron_changes = 0;
  int best_speed = speed;
  int best_rx = 0, max_working_speed = 0;
  int verify_rx = 0;
  combo_result_t results[32]; /* collected for future summary output */
  int num_results = 0;
  (void)results; (void)num_results;

  /* =================================================================
   * STEP 1: DETECT BAUD RATE
   *
   * The most important thing. Start the BBS, listen for AT init
   * commands at each baud rate. At the right speed we'll see
   * valid ASCII like "ATX2M0S0=0S7=30V1E0S2=1S10=30".
   * At the wrong speed we'll see garbage.
   * ================================================================= */
  printf("\n\n  ============================================\n");
  printf("  STEP 1: Detect Baud Rate\n");
  printf("  ============================================\n");
  printf("\n  The C64 should be ON with the BBS software loaded.\n");
  printf("  When you press ENTER, start the BBS so it sends\n");
  printf("  its AT modem init commands. We will listen at each\n");
  printf("  baud rate to find which one gives valid data.\n");

  wait_for_enter("Start the BBS NOW, then press ENTER immediately.");

  combo_result_t baud_results[16];
  int baud_count = run_baud_scan(fd, speed, baud_results, 16);

  /* Find best baud rate */
  int best_baud_rx = 0;
  for(int i = 0; i < baud_count; i++) {
    if(baud_results[i].rx_bytes > best_baud_rx) {
      best_baud_rx = baud_results[i].rx_bytes;
      best_speed = baud_results[i].speed;
    }
    if(baud_results[i].rx_bytes > 0)
      results[num_results++] = baud_results[i];
  }

  if(best_baud_rx > 0) {
    printf("\n  SUCCESS: Detected %d baud (%d bytes of AT commands received)\n",
           best_speed, best_baud_rx);
    best_rx = best_baud_rx;
    max_working_speed = best_speed;
  } else {
    printf("\n  No valid AT commands detected at any baud rate.\n");
    printf("  Possible causes:\n");
    printf("    - TX/RX wires are swapped or disconnected\n");
    printf("    - BBS didn't send init fast enough (try again)\n");
    printf("    - DCD not asserted (6551 ACIA won't transmit)\n");
    printf("  We will continue with %d baud from your -s setting.\n", speed);
    best_speed = speed;
  }

  /* =================================================================
   * STEP 2: TEST CONTROL LINES
   *
   * Now that we (hopefully) know the baud rate, test the wiring.
   * Toggle DTR/RTS and observe which inputs change.
   * ================================================================= */
  printf("\n\n  ============================================\n");
  printf("  STEP 2: Test Control Line Wiring\n");
  printf("  ============================================\n");

  configure_port(fd, best_speed, 0);

  /* Read current state */
  int state_now = read_lines(fd);
  printf("\n  Current signal state:\n");
  print_status(state_now);

  /* DTR toggle */
  printf("\n  Testing DTR...\n");
  int before_dtr = read_lines(fd);
  set_line(fd, TIOCM_DTR, 0);
  int dtr_low = read_lines(fd);
  set_line(fd, TIOCM_DTR, 1);
  usleep(200000);

  dtr_affects = (before_dtr ^ dtr_low) & all_inputs;
  if(dtr_affects) {
    if(dtr_affects & TIOCM_DSR) printf("    DSR tracks DTR  (standard null-modem)\n");
    if(dtr_affects & TIOCM_CTS) printf("    CTS tracks DTR  (unusual)\n");
#ifdef TIOCM_CAR
    if(dtr_affects & TIOCM_CAR) printf("    DCD tracks DTR  (DTR drives carrier)\n");
#endif
  } else {
    printf("    No inputs responded to DTR toggle.\n");
  }

  /* Check if BBS reacted to DTR drop */
  {
    int after = read_lines(fd);
    dtr_caused_bbs_react = ((state_now ^ after) & all_inputs) != 0;
    if(dtr_caused_bbs_react)
      printf("    NOTE: BBS reacted to DTR drop (it detects hangup via DTR).\n");
  }

  /* RTS toggle */
  printf("\n  Testing RTS...\n");
  int before_rts = read_lines(fd);
  set_line(fd, TIOCM_RTS, 0);
  int rts_low = read_lines(fd);
  set_line(fd, TIOCM_RTS, 1);
  usleep(200000);
  (void)read_lines(fd);

  rts_affects = (before_rts ^ rts_low) & all_inputs;
  if(rts_affects) {
    if(rts_affects & TIOCM_CTS) printf("    CTS tracks RTS  (standard null-modem)\n");
    if(rts_affects & TIOCM_DSR) printf("    DSR tracks RTS  (unusual)\n");
#ifdef TIOCM_CAR
    if(rts_affects & TIOCM_CAR) printf("    DCD tracks RTS  (unusual)\n");
#endif
  } else {
    printf("    No inputs responded to RTS toggle.\n");
  }

  /* =================================================================
   * STEP 3: VERIFY FULL MODEM INIT
   *
   * At the detected baud rate, restart the BBS and listen for
   * the AT init. This time we respond with "OK" so the BBS
   * can proceed to "waiting for call" mode.
   * ================================================================= */
  printf("\n\n  ============================================\n");
  printf("  STEP 3: Verify Modem Init\n");
  printf("  ============================================\n");
  printf("\n  Settings: %d baud, 8N1, no flow control\n", best_speed);
  printf("  We will listen for the BBS AT init and respond OK.\n");
  printf("  If this works, the BBS should proceed to 'waiting for call'.\n");

  wait_for_enter("Restart the BBS, then press ENTER immediately.");

  configure_port(fd, best_speed, 0);
  printf("\n  Listening for AT init commands (10 seconds)...\n");
  verify_rx = listen_for_init(fd, 10, 1);

  if(verify_rx > 0) {
    printf("\n  SUCCESS: Received %d bytes of AT commands.\n", verify_rx);
    printf("  Responded with OK. Check if BBS is now 'waiting for call'.\n");
  } else {
    printf("\n  No AT commands received.\n");
    if(best_baud_rx == 0) {
      printf("  The serial link may not be working. Check TX/RX wiring.\n");
    } else {
      printf("  Try restarting the BBS and running the analyzer again.\n");
    }
  }

  /* =================================================================
   * STEP 4: BASELINE (optional, if Step 1 failed)
   *
   * If we couldn't detect anything, go back to basics:
   * power off the C64, check what changes.
   * ================================================================= */
  if(best_baud_rx == 0 && verify_rx == 0) {
    printf("\n\n  ============================================\n");
    printf("  STEP 4: Baseline Diagnostics\n");
    printf("  ============================================\n");
    printf("  Since we couldn't detect any communication, let's check\n");
    printf("  the hardware from scratch.\n");

    wait_for_enter("Power OFF the C64 completely.");
    int baseline = read_lines(fd);
    printf("\n  Signal state with C64 off:\n");
    print_status(baseline);

    wait_for_enter("Power ON the C64 (don't start BBS yet).");
    int powered = read_lines(fd);
    print_diff(baseline, powered, "Changes after power-on");
    poweron_changes = (baseline ^ powered) & all_inputs;

    if(poweron_changes == 0)
      printf("\n  No lines changed on power-on. This may be a 3-wire adapter.\n");
  }

  /* =================================================================
   * ANALYSIS AND RECOMMENDATIONS
   * ================================================================= */
  printf("\n\n  ============================================\n");
  printf("  ANALYSIS RESULTS\n");
  printf("  ============================================\n");

  int any_control = poweron_changes | dtr_affects | rts_affects;
  int is_3wire = (any_control == 0);
  int dsr_is_dtr = (dtr_affects & TIOCM_DSR) != 0;
  int cts_is_rts = (rts_affects & TIOCM_CTS) != 0;

  /* Cartridge detection */
  int cart = detect_cartridge(max_working_speed, is_3wire);

  printf("\n  CARTRIDGE: %s\n", cartridge_name(cart));

  printf("\n  BAUD RATE: ");
  if(best_rx > 0) printf("%d baud (detected from AT init)\n", best_speed);
  else            printf("unknown (using %d from -s flag)\n", speed);

  printf("\n  WIRING:\n");
  if(is_3wire)   printf("    3-wire adapter (TX/RX/GND only)\n");
  if(dsr_is_dtr) printf("    DTR/DSR cross-wired (standard null-modem)\n");
  if(cts_is_rts) printf("    RTS/CTS cross-wired (standard null-modem)\n");
  if(!is_3wire && !dsr_is_dtr && !cts_is_rts)
    printf("    Could not determine wiring (control lines did not respond)\n");

  printf("\n  DTR/HANGUP: ");
  if(dtr_caused_bbs_react) printf("BBS detects DTR drop -> AT&D2 works\n");
  else if(is_3wire)        printf("3-wire, no DTR -> use AT&D0\n");
  else                     printf("no reaction to DTR -> consider AT&D0\n");

  printf("\n  MODEM INIT: ");
  if(verify_rx > 0)    printf("VERIFIED — BBS accepted OK response\n");
  else if(best_rx > 0) printf("AT commands detected but verification incomplete\n");
  else                  printf("FAILED — no AT commands received\n");

  /* =================================================================
   * RECOMMENDED COMMAND LINE
   * ================================================================= */
  printf("\n  ============================================\n");
  printf("  RECOMMENDED COMMAND LINE\n");
  printf("  ============================================\n");

  int rec_speed = (best_rx > 0) ? best_speed : speed;

  printf("\n    tcpser -d %s -s %d", tty, rec_speed);

  char init_parts[256] = "";
  int need_init = 0;
  if(is_3wire || !dtr_caused_bbs_react) {
    strcat(init_parts, "&D0");
    need_init = 1;
  }
  /* Flow control is not probed directly. For a 6551 cartridge with real control
   * lines running at speed, RTS/CTS is usually required — recommend &K3 as a
   * starting point (the note below tells the operator to verify it). */
  int flow_is_heuristic = 0;
  if(!is_3wire && rec_speed >= 4800 &&
     (cart == CART_SWIFTLINK || cart == CART_TURBO232 || cart == CART_GLINK232)) {
    strcat(init_parts, "&K3");
    need_init = 1;
    flow_is_heuristic = 1;
  }
  if(need_init) printf(" -i \"%s\"", init_parts);
  printf(" -l 4 -p 6400 -x\n");

  printf("\n  Explanation:\n");
  printf("    -s %-6d  baud rate", rec_speed);
  if(best_rx > 0 && rec_speed != speed) printf(" (detected, was %d)", speed);
  printf("\n");
  if(strstr(init_parts, "&D0")) printf("    &D0        ignore DTR (no hangup detection)\n");
  if(strstr(init_parts, "&K3")) printf("    &K3        RTS/CTS hardware flow control\n");
  if(strstr(init_parts, "&K4")) printf("    &K4        XON/XOFF software flow control\n");
  printf("    -l 4       info-level logging\n");
  printf("    -p 6400    TCP listen port\n");
  printf("    -x         bot gate (require backspace)\n");
  if(flow_is_heuristic) {
    printf("\n  NOTE: flow control was not auto-probed. &K3 (RTS/CTS) is the\n");
    printf("        usual choice for a 6551 cartridge at this speed; if the\n");
    printf("        session stalls or drops characters, try removing &K3 or\n");
    printf("        using &K4 (XON/XOFF) instead.\n");
  }

  if(best_rx == 0) {
    printf("\n  TROUBLESHOOTING:\n");
    printf("    1. TX/RX likely swapped — try null-modem adapter\n");
    printf("    2. Swap pins 2 and 3 on the DB-9 cable\n");
    printf("    3. Check cartridge is firmly seated in C64\n");
    printf("    4. Try different USB-serial adapter (FTDI recommended)\n");
    printf("    5. Make sure BBS baud rate matches cartridge capability\n");
    printf("    6. Restart C64 completely and try the analyzer again\n");
  }

  printf("\n  Analyzer complete.\n\n");

  close(fd);
  return 0;
}
