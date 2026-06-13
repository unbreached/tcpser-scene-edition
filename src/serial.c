#include <sys/file.h>
#include <unistd.h>
#include <termios.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include "dce.h"
#include "debug.h"

#include "serial.h"

// Best-effort write loop that handles EINTR and short writes.
// For non-blocking fds, we return bytes written so far on EAGAIN/EWOULDBLOCK.
static int write_all(int fd, const unsigned char *data, int len) {
  int total = 0;
  while (total < len) {
    ssize_t rc = write(fd, data + total, (size_t)(len - total));
    if (rc > 0) {
      total += (int)rc;
      continue;
    }
    if (rc == 0) break;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return total;
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


int ser_get_control_lines(int fd) {
  int status;

  if (ioctl(fd, TIOCMGET, &status) < 0) {
    ELOG(LOG_FATAL, "Could not obtain serial port status");
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
    ELOG(LOG_FATAL, "Could not obtain serial port status");
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

  if(res > 0)
    log_trace(TRACE_MODEM_IN, data, res);
  return res;
}
