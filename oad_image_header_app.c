#include "oad_image_header_app.h"

#include <string.h>

#ifndef OAD_IMG_TYPE
#define OAD_IMG_TYPE OAD_IMG_TYPE_APP
#endif

#ifndef APP_SEC_VER
#define APP_SEC_VER 1
#endif

#ifndef APP_IMG_NO
#define APP_IMG_NO 0
#endif

#ifndef APP_V_MAJOR
#define APP_V_MAJOR 1
#endif

#ifndef APP_V_MINOR
#define APP_V_MINOR 0
#endif

#ifndef APP_V_PATCH
#define APP_V_PATCH 0
#endif

#ifndef APP_V_BUILD
#define APP_V_BUILD 0
#endif

#ifndef OTA_WITH_BIM_DUAL_ONCHIP
#define OTA_WITH_BIM_DUAL_ONCHIP 0
#endif

#define OAD_APP_START_ADDR 0x00000000UL

extern uint32_t _flash_end;
extern uint32_t _vectors;

__attribute__((used, section(".oad_image_header")))
const imgHdr_t ota_image_header = {
  .fixedHdr = {
    .imgID = OAD_IMG_ID_VAL,
    .crc32 = DEFAULT_CRC,
    .bimVer = BIM_VER,
    .metaVer = META_VER,
    .techType = OAD_WIRELESS_TECH_TIMAC_2_4G,
    .imgCpStat = DEFAULT_STATE,
    .crcStat = DEFAULT_STATE,
    .imgType = OAD_IMG_TYPE,
    .imgNo = APP_IMG_NO,
    .imgVld = 0xFFFFFFFF,
    .len = INVALID_LEN,
    .prgEntry = (uint32_t)&_vectors,
    .softVer = {APP_V_BUILD, APP_V_PATCH, APP_V_MINOR, APP_V_MAJOR},
    .imgEndAddr = (uint32_t)&_flash_end - 1,
    .hdrLen = sizeof(imgHdr_t),
    .rfu = 0xFFFF
  },
#if defined(SECURITY)
  .secInfoSeg = {
    .segTypeSecure = IMG_SECURITY_SEG_ID,
    .wirelessTech = OAD_WIRELESS_TECH_TIMAC_2_4G,
    .verifStat = DEFAULT_STATE,
    .secSegLen = SECURITY_SEG_LEN,
    .secVer = APP_SEC_VER,
    .secTimestamp = 0x00000000,
    .secSignerInfo = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11},
    .eccSign = {
      .sign_r = {0},
      .sign_s = {0}
    }
  },
#endif
  .imgPayload = {
    .segTypeImg = IMG_PAYLOAD_SEG_ID,
    .wirelessTech = OAD_WIRELESS_TECH_TIMAC_2_4G,
    .rfu = DEFAULT_STATE,
    .imgSegLen = INVALID_LEN,
#if OTA_WITH_BIM_DUAL_ONCHIP
    .startAddr = (uint32_t)&(ota_image_header.fixedHdr.imgID)
#else
    .startAddr = OAD_APP_START_ADDR
#endif
  }
};

bool
oad_image_header_read(uint32_t flash_address, imgHdr_t *header)
{
  static const uint8_t expected_id[8] = {'C', 'C', '1', '3',
                                         'x', '2', 'R', '1'};

  memcpy(header, (const void *)flash_address, sizeof(*header));
  return memcmp(header->fixedHdr.imgID, expected_id,
                sizeof(expected_id)) == 0;
}
