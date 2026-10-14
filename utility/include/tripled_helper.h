#ifndef __TRIPLED_HELPER_H__
#define __TRIPLED_HELPER_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* Worst-case escaped frame: 1 + 2 + 2 + 8 + 2 + 2*64 + 1 = 144 bytes */
#define  TRIPLE_MTU                 160
#define  TRIPLE_MAGIC               0x739A//0x729B

/* Line discipline number, must match the driver (driver/include/triple_helper.h).
 * 29 is N_DEVELOPMENT on kernels >= 5.18. */
#define  N_TRIPLE                   29

#define  ID_LEN                     4
#define  DATA_LEN                   8
#define  DATA_FD_LEN                64
#define  COM_BUF_LEN                TRIPLE_MTU
#define  DATA_LEN_ERR               12
#define  TIME_CHAR_NUM              13

#define  U2C_TR_FIRST_BYTE          0x0F
#define  U2C_TR_LAST_BYTE           0xEF
#define  U2C_TR_SPEC_BYTE           0x1F

#define  U2C_TR_CMD_TX_CAN          0x81
#define  U2C_TR_CMD_TX_CAN_TS       0x82
#define  U2C_TR_CMD_MARKER          0x87
#define  U2C_TR_CMD_SETTINGS        0x88
#define  U2C_TR_CMD_BITTIMING       0x89
#define  U2C_TR_CMD_STATUS          0x8A
#define  U2C_TR_CMD_TIMESTAMP       0x8B
#define  U2C_TR_CMD_FW_VER          0x90
#define  U2C_TR_CMD_SPEED_DOWN      0x91
#define  U2C_TR_CMD_SPEED_UP        0x92
#define  U2C_TR_CMD_BUFFER_MODE 0x8D

enum CAN_SPEED
{
  SPEED_10k     = 10,
  SPEED_20k     = 20,
  SPEED_33_3k   = 33,
  SPEED_50k     = 50,
  SPEED_62_5k   = 62,
  SPEED_83_3k   = 83,
  SPEED_100k    = 100,
  SPEED_125k    = 125,
  SPEED_250k    = 250,
  SPEED_500k    = 500,
  SPEED_1M      = 1000,
  SPEED_USR     = 0,
};

enum CAN_FD_SPEED
{
  CAN_USR_SPEED = 0,
  CAN_125K_250K = 125250,
  CAN_125K_500K = 125500,
  CAN_125K_833K = 125833,
  CAN_125K_1M   = 1251000,
  CAN_125K_1M5  = 1251500,
  CAN_125K_2M   = 1252000,
  CAN_125K_3M   = 1253000,
  CAN_125K_4M   = 1254000,
  CAN_125K_5M   = 1255000,
  CAN_125K_6M7  = 1256700,
  CAN_125K_8M   = 1258000,
  CAN_125K_10M  = 1259999,

  CAN_250K_500K = 250500,
  CAN_250K_833K = 250833,
  CAN_250K_1M   = 2501000,
  CAN_250K_1M5  = 2501500,
  CAN_250K_2M   = 2502000,
  CAN_250K_3M   = 2503000,
  CAN_250K_4M   = 2504000,
  CAN_250K_5M   = 2505000,
  CAN_250K_6M7  = 2506700,
  CAN_250K_8M   = 2508000,
  CAN_250K_10M  = 2509999,

  CAN_500K_833K = 500833,
  CAN_500K_1M   = 5001000,
  CAN_500K_1M5  = 5001500,
  CAN_500K_2M   = 5002000,
  CAN_500K_3M   = 5003000,
  CAN_500K_4M   = 5004000,
  CAN_500K_5M   = 5005000,
  CAN_500K_6M7  = 5006700,
  CAN_500K_8M   = 5008000,
  CAN_500K_10M  = 5009999,

  CAN_1000K_1M5 = 10001500,
  CAN_1000K_2M  = 10002000,
  CAN_1000K_3M  = 10003000,
  CAN_1000K_4M  = 10004000,
  CAN_1000K_5M  = 10005000,
  CAN_1000K_6M7 = 10006700,
  CAN_1000K_8M  = 10008000,
  CAN_1000K_10M = 10009999,
};

enum CAN_PORT
{
  PORT_1 = 0,
  PORT_2,
  PORT_3
};
enum USER_BITTIMING_PAR {
  NBRP = 0,
  NTSEG1,
  NTSEG2,
  NSJW,
  DBRP,
  DTSEG1,
  DTSEG2,
  DSJW,
  TDCO,
  TDCV,
  TDCMOD,
};

enum TDC_MOD {
  TDCMOD_OFF = 0,
  TDCMOD_MAN,
  TDCMOD_AUTO,
};

/* Copy n escaped bytes from src to dest, dropping the escape markers.
 * Returns the number of bytes written to dest (<= n). */
inline static size_t escape_memcpy(void *dest, const void *src, size_t n)
{
  bool escape = false;
  size_t out = 0;
  size_t i;
  const unsigned char *csrc = (const unsigned char *)src;
  unsigned char *cdest = (unsigned char *)dest;

  for (i = 0; i < n; i++)
  {
    if (csrc[i] == U2C_TR_SPEC_BYTE && !escape)
    {
      escape = true;
      continue;
    }
    cdest[out++] = csrc[i];
    escape = false;
  }
  return out;
}

inline static unsigned char USB2CAN_TRIPLE_PushByte(const unsigned char value, unsigned char *buffer)
{
  if ((value == U2C_TR_FIRST_BYTE)
      || (value == U2C_TR_LAST_BYTE)
      || (value == U2C_TR_SPEC_BYTE))
  {
    buffer[0] = U2C_TR_SPEC_BYTE;
    buffer[1] = value;
    return 2;
  }
  else
  {
    buffer[0] = value;
    return 1;
  }
}

inline static unsigned char USB2CAN_TRIPLE_PushByteClear(const unsigned char value, unsigned char *buffer)
{
  buffer[0] = value;
  return 1;
}
inline static bool USB2CAN_TRIPLE_DLCFromLength(unsigned char *dlc, const unsigned char length)
{
  //nuluji bity kde je delka
  *dlc = *dlc & 0xF0;
  switch (length)
  {
  case 0: return true;
  case 1: *dlc |= 0x01; return true;
  case 2: *dlc |= 0x02; return true;
  case 3: *dlc |= 0x03; return true;
  case 4: *dlc |= 0x04; return true;
  case 5: *dlc |= 0x05; return true;
  case 6: *dlc |= 0x06; return true;
  case 7: *dlc |= 0x07; return true;
  case 8: *dlc |= 0x08; return true;
  }
  return false;
}

inline static unsigned char USB2CAN_TRIPLE_LengthFromDLC(const unsigned char dlc)
{
  switch (dlc & 0x0F)
  {
  case 0x00: return 0;
  case 0x01: return 1;
  case 0x02: return 2;
  case 0x03: return 3;
  case 0x04: return 4;
  case 0x05: return 5;
  case 0x06: return 6;
  case 0x07: return 7;
  case 0x08: return 8;
  }
  return 0;
}

inline static bool USB2CAN_TRIPLE_CANFD_DLCFromLength(unsigned char *dlc, const unsigned char length)
{
  //nuluji bity kde je delka
  *dlc = *dlc & 0xF0;
  switch (length)
  {
  case 0: return true;
  case 1: *dlc |= 0x01; return true;
  case 2: *dlc |= 0x02; return true;
  case 3: *dlc |= 0x03; return true;
  case 4: *dlc |= 0x04; return true;
  case 5: *dlc |= 0x05; return true;
  case 6: *dlc |= 0x06; return true;
  case 7: *dlc |= 0x07; return true;
  case 8: *dlc |= 0x08; return true;
  case 12: *dlc |= 0x09; return true;
  case 16: *dlc |= 0x0A; return true;
  case 20: *dlc |= 0x0B; return true;
  case 24: *dlc |= 0x0C; return true;
  case 32: *dlc |= 0x0D; return true;
  case 48: *dlc |= 0x0E; return true;
  case 64: *dlc |= 0x0F; return true;
  }
  return false;
}
inline static unsigned char USB2CAN_TRIPLE_CANFD_LengthFromDLC(const unsigned char dlc)
{
  switch (dlc & 0x0F)
  {
  case 0x00: return 0;
  case 0x01: return 1;
  case 0x02: return 2;
  case 0x03: return 3;
  case 0x04: return 4;
  case 0x05: return 5;
  case 0x06: return 6;
  case 0x07: return 7;
  case 0x08: return 8;
  case 0x09: return 12;
  case 0x0A: return 16;
  case 0x0B: return 20;
  case 0x0C: return 24;
  case 0x0D: return 32;
  case 0x0E: return 48;
  case 0x0F: return 64;
  }
  return 0;
}
inline static void USB2CAN_TRIPLE_SendBufferMode(int fd)
{
	unsigned char buffer[16];
	unsigned char length=3;
	buffer[0] = U2C_TR_FIRST_BYTE;
	buffer[1] = 4;
	buffer[2] = U2C_TR_CMD_BUFFER_MODE;
  u_int32_t buffer_mode = 1;
	length+=USB2CAN_TRIPLE_PushByte(buffer_mode >>24,&buffer[length]);
	length+=USB2CAN_TRIPLE_PushByte(buffer_mode>>16,&buffer[length]);
	length+=USB2CAN_TRIPLE_PushByte(buffer_mode>>8,&buffer[length]);
	length+=USB2CAN_TRIPLE_PushByte(buffer_mode>>0,&buffer[length]);
	buffer[length] = U2C_TR_LAST_BYTE;
	length++;
	buffer[1] = length;

  if (write(fd, buffer, length) <= 0)
  {
    perror("write USB2CAN_TRIPLE_SendCANSpeed");
  }
}

inline static void USB2CAN_TRIPLE_GetFWVersion(int fd)
{
  unsigned char buffer[16];
  int i = 0;
  buffer[0] = U2C_TR_FIRST_BYTE;
  buffer[1] = 4;
  USB2CAN_TRIPLE_PushByte(U2C_TR_CMD_FW_VER, &buffer[2]);
  buffer[3] = U2C_TR_LAST_BYTE;

  if (write(fd, buffer, 4) <= 0)
  {
    perror("write USB2CAN_TRIPLE_GetFWVersion");
    exit(EXIT_FAILURE);
  }
}

inline static void USB2CAN_TRIPLE_SendCANSpeed(unsigned int port, int speed, bool listen_only, int fd)
{
  unsigned char buffer[16];
  unsigned char length = 3;
  int i = 0;
  buffer[0] = U2C_TR_FIRST_BYTE;
  buffer[1] = 1;
  buffer[2] = U2C_TR_CMD_SETTINGS;
  length += USB2CAN_TRIPLE_PushByte(port, &buffer[length]);
  u_int16_t s = speed;
  length += USB2CAN_TRIPLE_PushByte(s >> 8, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(s >> 0, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(listen_only ? 1 : 0, &buffer[length]);
  buffer[length] = U2C_TR_LAST_BYTE;
  length++;
  buffer[1] = length;

  if (write(fd, buffer, length) <= 0)
  {
    perror("write USB2CAN_TRIPLE_SendCANSpeed");
    exit(EXIT_FAILURE);
  }
}

inline static void USB2CAN_TRIPLE_SendFDCANSpeed(int speed, bool listen_only, bool esi, bool iso_crc, int fd)
{
  unsigned char buffer[16];
  unsigned char length = 3;
  int i = 0;
  buffer[0] = U2C_TR_FIRST_BYTE;
  buffer[1] = 1;
  buffer[2] = U2C_TR_CMD_SETTINGS;
  length += USB2CAN_TRIPLE_PushByte(3, &buffer[length]);
  u_int32_t s = (u_int32_t)speed;
  length += USB2CAN_TRIPLE_PushByte(s >> 24, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(s >> 16, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(s >> 8, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(s >> 0, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(listen_only ? 1 : 0, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(iso_crc ? 1 : 0, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(esi ? 1 : 0, &buffer[length]);
  buffer[length] = U2C_TR_LAST_BYTE;
  length++;
  buffer[1] = length;

  if (write(fd, buffer, length) <= 0)
  {
    perror("write USB2CAN_TRIPLE_SendFDCANSpeed");
    exit(EXIT_FAILURE);
  }
}

inline static void USB2CAN_TRIPLE_SendTimeStampMode(bool mode, int fd)
{
  unsigned char buffer[16];
  unsigned char l = 3;
  int i = 0;
  buffer[0] = U2C_TR_FIRST_BYTE;
  buffer[2] = U2C_TR_CMD_TIMESTAMP;
  l += USB2CAN_TRIPLE_PushByte((unsigned char)mode, &buffer[l]);
  buffer[l] = U2C_TR_LAST_BYTE;
  buffer[1] = l + 1;

  if (write(fd, buffer, l + 1) <= 0)
  {
    perror("write USB2CAN_TRIPLE_SendTimeStampMode");
    exit(EXIT_FAILURE);
  }
}

inline static void USB2CAN_TRIPLE_SendFDCANUsrSpeed(unsigned int NBRP, unsigned int NTSEG1, unsigned int NTSEG2, unsigned int NSJW,
    unsigned int DBRP, unsigned int DTSEG1, unsigned int DTSEG2, unsigned int DSJW, unsigned int TDCO, unsigned int TDCV, unsigned int TDCMOD,
    bool listen_only, bool esi, bool iso_crc, int fd)
{

  unsigned char buffer[64];
  unsigned char length = 3;
  buffer[0] = U2C_TR_FIRST_BYTE;
  buffer[1] = 1;
  buffer[2] = U2C_TR_CMD_BITTIMING;
  length += USB2CAN_TRIPLE_PushByte(3, &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(NBRP >> 8) , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) NBRP , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(NTSEG1 >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) NTSEG1   , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(NTSEG2 >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) NTSEG2   , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(NSJW >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) NSJW   , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(DBRP >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) DBRP   , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(DTSEG1 >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) DTSEG1   , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(DTSEG2 >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) DTSEG2   , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(DSJW >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) DSJW   , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(TDCO >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) TDCO, &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)(TDCV >> 8)  , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char) TDCV , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte((unsigned char)((u_int16_t)(TDCMOD) >> 8) , &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte((unsigned char)((u_int16_t)(TDCMOD))    , &buffer[length]);

  length += USB2CAN_TRIPLE_PushByte(listen_only ? 1 : 0, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(iso_crc ? 1 : 0, &buffer[length]);
  length += USB2CAN_TRIPLE_PushByte(esi ? 1 : 0, &buffer[length]);
  buffer[length] = U2C_TR_LAST_BYTE;
  length++;
  buffer[1] = length;

  if (write(fd, buffer, length + 1) <= 0)
  {
    perror("write USB2CAN_TRIPLE_SendFDCANUsrSpeed");
    exit(EXIT_FAILURE);
  }
}

#endif //__TRIPLED_HELPER_H__