#include <sys/file.h>
#include <unistd.h>
#include <termios.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include "dce.h"
#include "debug.h"

#include "serial.h"

/* How long a serial write may wait for the kernel TX queue to drain before we
 * give up on the remainder. The port is O_NONBLOCK; at 2400 baud a full 4 KB
 * queue takes ~17 s to drain, so with hardware flow control asserted by a busy
 * C64 a few seconds of waiting is normal. Bounded so a dead C64 (CTS never
 * raised) cannot wedge the bridge/ip threads forever. */
#ifndef SER_WRITE_TIMEOUT_MS
#define SER_WRITE_TIMEOUT_MS 5000
#endif

// Write loop that handles EINTR and short writes, and WAITS (poll POLLOUT)
// when the non-blocking port's TX queue is full instead of silently dropping
// the remainder — previously every caller ignored the short count, so a
// caller pasting 10 KB at 2400 baud lost most of it.
static int write_all(int fd, const unsigned char *data, int len) {
  int total = 0;
  int waited_ms = 0;
  while (total < len) {
    ssize_t rc = write(fd, data + total, (size_t)(len - total));
    if (rc > 0) {
      total += (int)rc;
      waited_ms = 0;
      continue;
    }
    if (rc == 0) break;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (waited_ms >= SER_WRITE_TIMEOUT_MS) {
        LOG(LOG_WARN, "Serial TX stalled for %d ms (flow control / port not draining): dropped %d of %d bytes",
            waited_ms, len - total, len);
        return total;
      }
      struct pollfd p; p.fd = fd; p.events = POLLOUT; p.revents = 0;
      int pr = poll(&p, 1, 250);
      if (pr < 0 && errno != EINTR) return total;
      if (pr > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
      waited_ms += 250;
      continue;
    }
    return -1;
  }
  return total;
}

int ser_get_bps_const(int speed) {
  int bps_rate = 0;

  LOG_ENTER();
  
  LOG(LOG_DEBUG,"Checking speed: %d",speed);

  switch (speed) {
    case 230400:
      bps_rate = B230400;
      break;
    case 115200:
      bps_rate = B115200;
      break;
    case 57600:
      bps_rate = B57600;
      break;
    case 38400:
      bps_rate = B38400;
      break;
    case 19200:
      bps_rate = B19200;
      break;
    case 9600:
      bps_rate = B9600;
      break;
    case 4800:
      bps_rate = B4800;
      break;
    case 2400:
      bps_rate = B2400;
      break;
    case 1200:
      bps_rate = B1200;
      break;
    case 600:
      bps_rate = B600;
      break;
    case 300:
      bps_rate = B300;
      break;
    case 150:
      bps_rate = B150;
      break;
    case 134:
      bps_rate = B134;
      break;
    case 110:
      bps_rate = B110;
      break;
    case 75:
      bps_rate = B75;
      break;
    case 50:
      bps_rate = B50;
      break;
    case 0:
      bps_rate = B0;
      break;
    default:
      ELOG(LOG_FATAL, "Unknown baud rate"); 
      bps_rate = -1;
  }
  LOG_EXIT();
  return bps_rate;

}

int ser_init_conn(char *tty, int speed) {
  int fd = -1;
  struct termios tio;
  int bps_rate = 0;

  LOG_ENTER();

  bps_rate = ser_get_bps_const(speed);

  if(bps_rate > -1) {
    /* open the device to be non-blocking (read will return immediatly) */
    LOG(LOG_INFO, "Opening serial device %s at speed %d", tty, speed);

    fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd <0) {
      ELOG(LOG_FATAL, "TTY %s could not be opened", tty); 
    } else {
      LOG(LOG_INFO, "Opened serial device %s at speed %d as fd %d", tty, speed, fd);

      // Start from the kernel's current settings to avoid undefined flags.
      // This improves compatibility with a wide range of USB/RS232 adapters.
      if (0 != tcgetattr(fd, &tio)) {
        ELOG(LOG_FATAL, "Could not get serial port attributes");
        close(fd);
        LOG_EXIT();
        return -1;
      }

      cfmakeraw(&tio);

      // Default to 8N1, local mode.
      tio.c_cflag &= ~(PARENB | PARODD | CSTOPB | CSIZE);
      tio.c_cflag |= (CS8 | CLOCAL | CREAD);

      // IMPORTANT: Do not force hardware flow control by default.
      // Many Commodore/userport adapters are effectively 3-wire and will stall
      // if CRTSCTS is enabled.
      tio.c_cflag &= ~CRTSCTS;

      // Ignore break on input.
      tio.c_iflag |= IGNBRK;
      cfsetispeed(&tio, bps_rate);
      cfsetospeed(&tio, bps_rate);

      tio.c_cc[VMIN] = 1;
      tio.c_cc[VTIME] = 0;

      tcflush(fd, TCIFLUSH);
      if (0 != tcsetattr(fd, TCSANOW, &tio)) {
        ELOG(LOG_FATAL, "Could not set serial port attributes");
        close(fd);
        LOG_EXIT();
        return -1;
      }
      LOG(LOG_INFO, "serial device configured");
    }
  }

  LOG_EXIT();
  return fd;
}

int ser_set_flow_control(int fd, int status) {
  struct termios tio;
  if(0 != tcgetattr(fd, &tio)) {
    ELOG(LOG_FATAL, "Could not get serial port attributes");
    return -1;
  }
  // turn all off.
  tio.c_cflag &= ~CRTSCTS;
  tio.c_iflag &= ~(IXON | IXOFF | IXANY);

  // Apply requested settings
  if (status & CRTSCTS) tio.c_cflag |= CRTSCTS;
  if (status & (IXON | IXOFF)) tio.c_iflag |= (IXON | IXOFF);
  if(0 != tcsetattr(fd, TCSANOW, &tio)) {
    ELOG(LOG_FATAL,"Could not set serial port attributes");
    return -1;
  }
  return 0;
}


/* Change the baud rate of an already-open serial port at runtime.
 * TCSADRAIN lets any bytes already queued for TX (e.g. the "POKE 838,x" we just
 * wrote) flush at the OLD speed before the port switches, so the C64 receives
 * them intact. Returns 0 on success, -1 on unsupported speed or ioctl failure. */
int ser_set_speed(int fd, int speed) {
  struct termios tio;
  int bps_rate = ser_get_bps_const(speed);

  LOG_ENTER();
  if(bps_rate < 0) {
    LOG(LOG_ERROR, "Refusing to set unsupported baud rate %d", speed);
    LOG_EXIT();
    return -1;
  }
  if(0 != tcgetattr(fd, &tio)) {
    ELOG(LOG_ERROR, "Could not get serial attributes for speed change");
    LOG_EXIT();
    return -1;
  }
  cfsetispeed(&tio, bps_rate);
  cfsetospeed(&tio, bps_rate);
  if(0 != tcsetattr(fd, TCSADRAIN, &tio)) {
    ELOG(LOG_ERROR, "Could not change serial speed to %d", speed);
    LOG_EXIT();
    return -1;
  }
  LOG(LOG_INFO, "Serial port speed changed to %d", speed);
  LOG_EXIT();
  return 0;
}

int ser_get_control_lines(int fd) {
  int status;

  if (ioctl(fd, TIOCMGET, &status) < 0) {
    if (errno == ENOTTY) {
      /* A pty or a device with no modem-control lines: treat as "link up,
       * DTR asserted" instead of failing (lets tcpser run against a pty). */
      return DCE_CL_LE | DCE_CL_DTR;
    }
    /* EIO/ENXIO/EBADF: the adapter is gone or being reopened. The caller
     * (ctrl_thread) reports this once and waits; polled 10x/s so no ELOG here. */
    return -1;
  }

  int out = DCE_CL_LE;

  // Readable input lines
  if (status & TIOCM_CTS) out |= DCE_CL_CTS;
  if (status & TIOCM_CAR) out |= DCE_CL_DCD;
  if (status & TIOCM_DSR) out |= DCE_CL_DSR;

  // CRITICAL FIX:
  // Treat DSR as remote DTR (Commodore + USB adapter reality)
  if (status & TIOCM_DSR)
      out |= DCE_CL_DTR;

  return out;
}


int ser_set_control_lines(int fd, int state) {
  int status;

  if(0 > ioctl(fd, TIOCMGET, &status)) {
    if(errno == ENOTTY) return 0;   /* pty: nothing to drive */
    ELOG(LOG_WARN, "Could not obtain serial port status");
    return -1;
  }

  // Backwards-compatible behavior:
  // tcpser historically used DTR as a controllable output to signal carrier.
  // Many adapters don't let us drive DCD/DSR directly.
  status &= ~(TIOCM_DTR);
  status |= ((state & DCE_CL_DCD) ? TIOCM_DTR : 0);
  if(0 > ioctl(fd, TIOCMSET, &status)) {
    ELOG(LOG_WARN, "Could not set serial port status");
  }
  return 0;
}

int ser_write(int fd, unsigned char* data, int len) {
  log_trace(TRACE_MODEM_OUT, data, len);
  return write_all(fd, data, len);
}

int ser_read(int fd, unsigned char* data, int len) {
  int res;

  do {
    res = (int)read(fd, data, (size_t)len);
  } while (res < 0 && errno == EINTR);

  if(res == 0) {
    /* With CLOCAL set and VMIN=1, a non-blocking tty never returns 0 for "no
     * data" (that is EAGAIN). 0 is EOF: the USB adapter was unplugged (or the
     * pty master closed). Report it as an error so bridge_task reconnects
     * instead of spinning on a permanently-readable dead descriptor. */
    errno = EIO;
    return -1;
  }
  if(res > 0)
    log_trace(TRACE_MODEM_IN, data, res);
  return res;
}
