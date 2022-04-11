/*
 * tripled.c - userspace daemon CAN interface driver
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Send feedback to <koupy@canlab.cz>
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <syslog.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <termios.h>
#include <linux/tty.h>
#include <linux/sockios.h>
#include <linux/version.h>

#include "version.h"
#include "tripled_helper.h"
/*
 * Before 3.1.0, the ldisc number is private define
 * in kernel, userspace application cannot use it.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,1,0)
#define N_TRIPLE (NR_LDISCS - 1)
#endif

#define   DAEMON_NAME      "tripled"
#define   TTYPATH_LENGTH   64

static int  tripled_running;
static int  exit_code;
static char ttypath [TTYPATH_LENGTH];

/* static function prototype */
static void print_version (char *prg);
static void print_usage (char *prg);
static void child_handler (int signum);
static int look_up_can_speed (int speed);
static int look_up_can_fd_speed (int speed);
static void run_interactive ();
static void print_bittiming();
static void parse_bittiming();
static void print_speed();


/* v2.1: change some variable to global (for end process) */
int             port;
int             ldisc;
int             fd;
speed_t         old_ispeed;
speed_t         old_ospeed;
struct termios  tios;

int main (int argc, char *argv[])
{
  ldisc = N_TRIPLE;

  int             sp;
  int             opt;
  int             channel;
  int             run_as_daemon = 1;
  char           *pch;
  char           *tty = NULL;
  char            buf[IFNAMSIZ + 1];
  char const     *devprefix = "/dev/";

  int             speed[3];
  char           *name[3];
  bool listen_only[3];
  unsigned int user_speed[11];
  bool can_fd = true;
  bool iso_crc = false;
  bool esi = false;
  bool user_bittiming = false;

  name[PORT_1] = NULL;
  name[PORT_2] = NULL;
  name[PORT_3] = NULL;
  listen_only[PORT_1] = false;
  listen_only[PORT_2] = false;
  listen_only[PORT_3] = false;
  ttypath[0] = '\0';
  const char delim[] = ":";
  int i = 0;

  char *tmp[11];

  while ((opt = getopt(argc, argv, "s:n:l:duvtwh?f:c:")) != -1)
  {
    switch (opt)
    {
    case 's'://set speed
      tmp[i] = strtok(optarg, ":");
      while (tmp[i] != NULL)
      {
        tmp[++i] = strtok(NULL, ":");
      }
      speed[PORT_1] = look_up_can_speed(strtol(tmp[0], NULL, 16));
      speed[PORT_2] = look_up_can_speed(strtol(tmp[1], NULL, 16));
      speed[PORT_3] = look_up_can_fd_speed(strtol(tmp[2], NULL, 16));
      break;
    case 'n'://set names
      name[i] = strtok(optarg, ":");
      while (name[i] != NULL)
      {
        name[++i] = strtok(NULL, ":");
      }
      break;
    case 'l'://set listen-only
      tmp[i] = strtok(optarg, ":");
      while (tmp[i] != NULL)
      {
        tmp[++i] = strtok(NULL, ":");
      }
      for (i = 0; i < 3; i++)
        listen_only[i] = (bool)atoi(tmp[i]);

      break;
    case 'd'://run a deamon
      run_as_daemon = 0;
      break;
    case 'v':// print version
      if (argc == 3)
      {
        print_version(argv[2]);
      }
      break;
    case 'f':// CAN FD on
      can_fd = true;
      tmp[i] = strtok(optarg, ":");
      while (tmp[i] != NULL)
      {
        tmp[++i] = strtok(NULL, ":");
      }
      esi = (bool)(atoi(tmp[0]));
      iso_crc = (bool)(atoi(tmp[1]));
      break;
    case 'c':// User defined CAND FD bittiming
      user_bittiming = true;
      tmp[i] = strtok(optarg, ":");
      while (tmp[i] != NULL)
      {
        tmp[++i] = strtok(NULL, ":");
      }
      for (i = 0; i < 11; i++)
        user_speed[i] = strtoul(tmp[i], NULL, 10);
      break;
    case 'u':
      print_bittiming();
      break;
    case 't':// print Speeds
      print_speed();
      break;
    case 'h'://help
    case '?':
    default:
      print_usage(argv[0]);
      break;
    }
    i=0;
  }

  /* Initialize the logging interface */
  openlog(DAEMON_NAME, LOG_PID, LOG_LOCAL5);

  /* Parse serial device name and optional can interface name */
  tty = argv[optind];
  if (NULL == tty)
    print_usage(argv[0]);
  /*
    name[PORT_1] = argv[optind + 1];
    if (name[PORT_1])
      name[PORT_2] = argv[optind + 2];
    if (name[PORT_2])
      name[PORT_3] = argv[optind + 3];
  */
  /* Prepare the tty device name string */
  pch = strstr(tty, devprefix);
  if (pch != tty)
    snprintf(ttypath, TTYPATH_LENGTH, "%s%s", devprefix, tty);
  else
    snprintf(ttypath, TTYPATH_LENGTH, "%s", tty);

  syslog(LOG_INFO, "starting on TTY device %s", ttypath);

  fd = open(ttypath, O_RDWR | O_NONBLOCK | O_NOCTTY);

  if (fd < 0)
  {
    syslog(LOG_NOTICE, "failed to open TTY device %s\n", ttypath);
    perror(ttypath);
    exit(EXIT_FAILURE);
  }
  /****************************************************************************************************/
  /* Configure baud rate */
  memset(&tios, 0, sizeof(struct termios));
  if (tcgetattr(fd, &tios) < 0)
  {
    syslog(LOG_NOTICE, "failed to get attributes for TTY device %s: %s\n", ttypath, strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* Get old values for later restore */
  old_ispeed = cfgetispeed(&tios);
  old_ospeed = cfgetospeed(&tios);

  /* Reset UART settings */
  cfmakeraw(&tios);

  /* Reset UART settings */
  tios.c_iflag &= ~IXOFF;
  tios.c_cflag &= ~CRTSCTS;

  /* Baud Rate */
  cfsetispeed(&tios, B115200);
  cfsetospeed(&tios, B115200);

  /* apply changes */
  if (tcsetattr(fd, TCSADRAIN, &tios) < 0)
    syslog(LOG_NOTICE, "Cannot set attributes for device \"%s\": %s!\n", ttypath, strerror(errno));

  /*Set speed on can port, timestamp mode and get FW version*/
  //USB2CAN_TRIPLE_Init(speed, fd);
 USB2CAN_TRIPLE_SendBufferMode(fd);
  sleep(2);
  USB2CAN_TRIPLE_SendTimeStampMode(false, fd);
  sleep(2);
  USB2CAN_TRIPLE_SendCANSpeed(1, speed[PORT_1], listen_only[PORT_1], fd);
    syslog(LOG_NOTICE, "can1 setting speed: %d listen only: %d!\n", speed[PORT_1], listen_only[PORT_1]);

  sleep(2);
  USB2CAN_TRIPLE_SendCANSpeed(2, speed[PORT_2], listen_only[PORT_2], fd);
    syslog(LOG_NOTICE, "can1 setting speed: %d listen only: %d!\n", speed[PORT_2], listen_only[PORT_2]);

  sleep(2);
  if (!user_bittiming)
  {
    USB2CAN_TRIPLE_SendFDCANSpeed(speed[PORT_3], listen_only[PORT_3], esi, iso_crc, fd);
  }
  else
  {
    USB2CAN_TRIPLE_SendFDCANUsrSpeed(user_speed[NBRP], user_speed[NTSEG1], user_speed[NTSEG2], user_speed[NSJW],
                                     user_speed[DBRP], user_speed[DTSEG1], user_speed[DTSEG2], user_speed[DSJW],
                                     user_speed[TDCO], user_speed[TDCV], user_speed[TDCMOD],
                                     listen_only[PORT_3], esi, iso_crc, fd);
  }
    syslog(LOG_NOTICE, "can1 setting speed: %d listen only: %d!\n", speed[PORT_3], listen_only[PORT_3]);

  sleep(1);
  USB2CAN_TRIPLE_GetFWVersion(fd);
  sleep(2);

  if (ioctl(fd, TIOCSETD, &ldisc) < 0)
  {
    perror("ioctl TIOCSETD");
    exit(EXIT_FAILURE);
  }
  /************* try to rename the created netdevice **************************************************/
  for (channel = 0; channel < 3; channel++)
  {

    if (ioctl(fd, SIOCGIFNAME, buf) < 0)
    {
      perror("ioctl SIOCGIFNAME");
      exit(EXIT_FAILURE);
    }

    //rename of interfaces
    if (name[channel])
    {
      struct ifreq ifr;
      int s = socket(PF_INET, SOCK_DGRAM, 0);

      if (s < 0)
      {
        perror("socket for interface rename");
      }
      else
      {
        strncpy(ifr.ifr_name, buf, IFNAMSIZ);
        strncpy(ifr.ifr_newname, name[channel], IFNAMSIZ);
      }
      if (ioctl(s, SIOCSIFNAME, &ifr) < 0)
      {
        perror("ioctl SIOCSIFNAME rename");
        exit(EXIT_FAILURE);
      }
      else
      {
        syslog(LOG_NOTICE, "netdevice %s renamed to %s\n", buf, name[channel]);
      }

      close(s);
    }
  }
  /* Daemonize */
  if (run_as_daemon)
  {
    if (daemon(0, 0))
    {
      syslog(LOG_ERR, "failed to daemonize");
      exit(EXIT_FAILURE);
    }
  }
  /* Trap signals that we expect to receive */
  /* End process */
  signal(SIGINT, child_handler);
  signal(SIGTERM, child_handler);

  tripled_running = 1;
  /* The Big Loop */
  while (tripled_running)
  {
    sleep(1); /* wait 1 second */
  }
  /* Reset line discipline */
  syslog(LOG_INFO, "stopping on TTY device %s", ttypath);
  ldisc = N_TTY;
  if (ioctl(fd, TIOCSETD, &ldisc) < 0)
  {
    perror("ioctl TIOCSETD");
    exit(EXIT_FAILURE);
  }

  /* Reset old rates */
  cfsetispeed(&tios, old_ispeed);
  cfsetospeed(&tios, old_ospeed);

  /* apply changes */
  if (tcsetattr(fd, TCSADRAIN, &tios) < 0)
    syslog(LOG_NOTICE, "Cannot set attributes for device \"%s\": %s!\n", ttypath, strerror(errno));

  /* Finish up */
  syslog(LOG_NOTICE, "terminated on %s", ttypath);
  closelog();

  return exit_code;
} /* END: main() */

static void print_version (char *prg)
{
  int          com_port;
  char        *pch;
  char const  *devprefix = "/dev/";

  pch = strstr(prg, devprefix);
  if (pch != prg)
  {
    snprintf(ttypath, TTYPATH_LENGTH, "%s%s", devprefix, prg);
  }
  else
  {
    snprintf(ttypath, TTYPATH_LENGTH, "%s", prg);
  }
  exit(EXIT_SUCCESS);

}

static void child_handler (int signum)
{
  switch (signum)
  {
  case SIGUSR1:
    /* exit parent */
    exit(EXIT_SUCCESS);
    break;
  case SIGALRM:
  case SIGCHLD:
    syslog(LOG_NOTICE, "received signal %i on %s", signum, ttypath);
    exit_code = EXIT_FAILURE;
    tripled_running = 0;
    break;
  case SIGINT:
  case SIGTERM:
    syslog(LOG_NOTICE, "received signal %i on %s", signum, ttypath);
    exit_code = EXIT_SUCCESS;
    tripled_running = 0;
    break;
  }

} /* END: child_handler() */

static int look_up_can_speed (int speed)
{
  switch (speed)
  {

  case 0x01:   return SPEED_10k;
  case 0x02:   return SPEED_20k;
  case 0x03:   return SPEED_33_3k;
  case 0x04:   return SPEED_50k;
  case 0x05:   return SPEED_62_5k;
  case 0x06:   return SPEED_83_3k;
  case 0x07:   return SPEED_100k;
  case 0x08:   return SPEED_125k;
  case 0x09:   return SPEED_250k;
  case 0x0A:   return SPEED_500k;
  case 0x0B:   return SPEED_1M;

  default:  return 250;
  }
}

static int look_up_can_fd_speed (int speed)
{
  switch (speed)
  {
  //nominal 125
  case 0x01:   return CAN_125K_250K;
  case 0x02:   return CAN_125K_500K;
  case 0x03:   return CAN_125K_833K;
  case 0x04:   return CAN_125K_1M;
  case 0x05:   return CAN_125K_1M5;
  case 0x06:   return CAN_125K_2M;
  case 0x07:   return CAN_125K_3M;
  case 0x08:   return CAN_125K_4M;
  case 0x09:   return CAN_125K_5M;
  case 0x0A:   return CAN_125K_6M7;
  case 0x0B:   return CAN_125K_8M;
  case 0x0C:   return CAN_125K_10M;

  case 0x11:   return CAN_250K_500K;
  case 0x12:   return CAN_250K_833K;
  case 0x13:   return CAN_250K_1M;
  case 0x14:   return CAN_250K_1M5;
  case 0x15:   return CAN_250K_2M;
  case 0x16:   return CAN_250K_3M;
  case 0x17:   return CAN_250K_4M;
  case 0x18:   return CAN_250K_5M;
  case 0x19:   return CAN_250K_6M7;
  case 0x1A:   return CAN_250K_8M;
  case 0x1B:   return CAN_250K_10M;

  case 0x21:   return CAN_500K_833K;
  case 0x22:   return CAN_500K_1M;
  case 0x23:   return CAN_500K_1M5;
  case 0x24:   return CAN_500K_2M;
  case 0x25:   return CAN_500K_3M;
  case 0x26:   return CAN_500K_4M;
  case 0x27:   return CAN_500K_5M;
  case 0x28:   return CAN_500K_6M7;
  case 0x29:   return CAN_500K_8M;
  case 0x2A:   return CAN_500K_10M;

  case 0x31:   return CAN_1000K_1M5;
  case 0x32:   return CAN_1000K_2M;
  case 0x33:   return CAN_1000K_3M;
  case 0x34:   return CAN_1000K_4M;
  case 0x35:   return CAN_1000K_5M;
  case 0x36:   return CAN_1000K_6M7;
  case 0x37:   return CAN_1000K_8M;
  case 0x38:   return CAN_1000K_10M;

  default:  return 2501000;
  }
}

static void print_bittiming()
{
  fprintf(stderr, "For detailed information see MCP2517FD datasheet\n");
  fprintf(stderr, "No validation is provided, so be careful !!\n");
  fprintf(stderr, "You can find online calculators\n");
  fprintf(stderr, "<bittiming options> --> ./tripled_64 -c[NBRP]:[NTSEG1]:[NTSEG2]:[NSJW]:[DBRP]:[DTSEG1]:[DTSEG2]:[DSJW]:[TDCO]:[TDCV]:[TDCMOD]\n");
  fprintf(stderr, "Nominal bittiming\n");
  fprintf(stderr, "NBRP - Nominal Baud Rate Prescaler\n");
  fprintf(stderr, "NTSEG1 - Time Segment 1\n");
  fprintf(stderr, "NTSEG2 - Time Segment 2\n");
  fprintf(stderr, "NSJW - Synchronization Jump Width \n");
  fprintf(stderr, "Data bittiming\n");
  fprintf(stderr, "DBRP - Data Baud Rate Prescaler\n");
  fprintf(stderr, "DTSEG1 - Time Segment 1\n");
  fprintf(stderr, "DTSEG2 - Time Segment 2\n");
  fprintf(stderr, "DSJW - Synchronzation Jump Width\n");
  fprintf(stderr, "TDCO- Transmitter Delay Compensation Offset\n");
  fprintf(stderr, "TDCV - Transmitter Delay Compensation Value\n");
  fprintf(stderr, "TDCMOD - Transmitter Delay Compensation Mode\n");
  fprintf(stderr, "TDCMOD options: 0 -> off, 1 -> manual, 2 -> auto\n");
  fprintf(stderr, "\n");
  exit(EXIT_FAILURE);
}

static void print_speed()
{
  fprintf(stderr, "--------------------CAN 2.0--------------------\n");
  fprintf(stderr, "               0x01 -> 10: 10 KBPS\n");
  fprintf(stderr, "               0x02 -> 20: 20 KBPS\n");
  fprintf(stderr, "               0x03 -> 33: 33 KBPS\n");
  fprintf(stderr, "               0x04 -> 50 : 50 KBPS\n");
  fprintf(stderr, "               0x05 -> 62: 62 KBPS\n");
  fprintf(stderr, "               0x06 -> 83: 83 KBPS\n");
  fprintf(stderr, "               0x07 -> 100: 100 KBPS\n");
  fprintf(stderr, "               0x08 -> 125: 125 KBPS\n");
  fprintf(stderr, "               0x09 -> 250: 250 KBPS\n");
  fprintf(stderr, "               0x0A -> 500: 500 KBPS\n");
  fprintf(stderr, "               0x0B -> 1000: 1 MBPS\n");
  fprintf(stderr, "-----------------------------------------------\n");

  fprintf(stderr, "-----------------CAN FD spped------------------\n");
  fprintf(stderr, "0xXX  -> CAN_<nominal speed>_<data speed>\n");
  fprintf(stderr, "               0x01  -> CAN_125K_250K\n");
  fprintf(stderr, "               0x02  -> CAN_125K_500K\n");
  fprintf(stderr, "               0x03  -> CAN_125K_833K \n");
  fprintf(stderr, "               0x04  -> CAN_125K_1M\n");
  fprintf(stderr, "               0x05  -> CAN_125K_1M5\n");
  fprintf(stderr, "               0x06  -> CAN_125K_2M\n");
  fprintf(stderr, "               0x07  -> CAN_125K_3M\n");
  fprintf(stderr, "               0x08  -> CAN_125K_4M\n");
  fprintf(stderr, "               0x09  -> CAN_125K_5M \n");
  fprintf(stderr, "               0x0A  -> CAN_125K_6M7\n");
  fprintf(stderr, "               0x0B  -> CAN_125K_8M\n");
  fprintf(stderr, "               0x0C  -> CAN_125K_10M\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "               0x11  -> CAN_250K_500K\n");
  fprintf(stderr, "               0x12  -> CAN_250K_833K\n");
  fprintf(stderr, "               0x13  -> CAN_250K_1M\n");
  fprintf(stderr, "               0x14  -> CAN_250K_1M5\n");
  fprintf(stderr, "               0x15  -> CAN_250K_2M\n");
  fprintf(stderr, "               0x16  -> CAN_250K_3M\n");
  fprintf(stderr, "               0x17  -> CAN_250K_4M\n");
  fprintf(stderr, "               0x18  -> CAN_250K_5M\n");
  fprintf(stderr, "               0x19  -> CAN_250K_6M7\n");
  fprintf(stderr, "               0x1A  -> CAN_250K_8M\n");
  fprintf(stderr, "               0x1B  -> CAN_250K_10M\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "               0x21  -> CAN_500K_833K\n");
  fprintf(stderr, "               0x22  -> CAN_500K_1M\n");
  fprintf(stderr, "               0x23 -> CAN_500K_1M5\n");
  fprintf(stderr, "               0x24  -> CAN_500K_2M\n");
  fprintf(stderr, "               0x25  -> CAN_500K_3M\n");
  fprintf(stderr, "               0x26  -> CAN_500K_4M\n");
  fprintf(stderr, "               0x27  -> CAN_500K_5M\n");
  fprintf(stderr, "               0x28  -> CAN_500K_6M7\n");
  fprintf(stderr, "               0x29  -> CAN_500K_8M\n");
  fprintf(stderr, "               0x2A  -> CAN_500K_10M\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "               0x31  -> CAN_1000K_1M5\n");
  fprintf(stderr, "               0x32  -> CAN_1000K_2M\n");
  fprintf(stderr, "               0x33  -> CAN_1000K_3M\n");
  fprintf(stderr, "               0x34  -> CAN_1000K_4M\n");
  fprintf(stderr, "               0x35  -> CAN_1000K_5M\n");
  fprintf(stderr, "               0x36  -> CAN_1000K_6M7\n");
  fprintf(stderr, "               0x37  -> CAN_1000K_8M\n");
  fprintf(stderr, "               0x38  -> CAN_1000K_10M\n");
  fprintf(stderr, "-----------------------------------------------\n");
  exit(EXIT_FAILURE);

} /* END: print_usage() */

/*------------------------------------------------------------------------------------*/
static void print_usage (char *prg)
{
  fprintf(stderr, "\nUsage: %s [options] <tty>\\n\n", prg);
  fprintf(stderr, "         -d                          (stay in foreground; no daemonize)\n");
  fprintf(stderr, "         -h                          (show this help page)\n");
  fprintf(stderr, "         -v                          (show version info)\n");
  fprintf(stderr, "         -t                          (show supported CAN 2.0 and CAN FD speeds)\n");
  fprintf(stderr, "         -u                          (show bittimng options and format)\n");
  fprintf(stderr, "         -s[port1]:[port2]:[port3]   (set speed: see CAN, see ./tripled_64 -t for available speeds)\n");
  fprintf(stderr, "         -n[port1]:[port2]:[port3]   (set name of interface)\n");
  fprintf(stderr, "         -l[1/0]:[1/0]:[1/0]         (listen-only mode )\n");
  fprintf(stderr, "         -f[1/0]:[1/0]               (CAN FD on port 3 -> [ESI]:[ISO_CRC]\n");
  fprintf(stderr, "         -c<bittiming options>       (User defined CAN FD bittiming, see ./tripled_64 -u)\n");
  fprintf(stderr, "\nExamples:\n");
  fprintf(stderr, "tripled_64 -s1:2:3 /dev/ttyACM0\n");
  fprintf(stderr, "tripled_64 /dev/ttyACM0\n");
  fprintf(stderr, "tripled_64 dev/ttyACM0 -ncan0:can1:can2\n");
  fprintf(stderr, "\n");
  exit(EXIT_FAILURE);

} /* END: print_usage() */