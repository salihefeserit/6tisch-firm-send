#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "ota-sensor-backend.h"
#include "ota.h"
#include "sys/log.h"
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#include <ti/common/cc26xx/oad/ext_flash_layout.h>
#include <ti/common/cc26xx/oad/oad_image_header.h>

static struct ctimer ota_reset_timer;

static const uint8_t expected_img_id[OAD_IMG_ID_LEN] = OAD_IMG_ID_VAL;
static const uint8_t expected_metadata_id[OAD_IMG_ID_LEN] = OAD_EXTFL_ID_VAL;

const char *
ota_sensor_backend_target_name(uint8_t target)
{
  return ota_target_name(target);
}

uint16_t
ota_sensor_backend_running_sec_ver(void)
{
  return OTA_RUNNING_SEC_VER;
}

static uint32_t
image_softver_to_u32(const imgFixedHdr_t *fixed_hdr)
{
  uint32_t version;
#ifdef BIM_VERIFY_VERSION_IMAGE
  version = fixed_hdr->softVer;
#else
  memcpy(&version, fixed_hdr->softVer, sizeof(version));
#endif
  return version;
}

static int
validate_offchip_image(const imgHdr_t *header)
{
  if(memcmp(header->fixedHdr.imgID, expected_img_id,
            sizeof(expected_img_id)) != 0) {
    LOG_WARN("[OTA] Rejecting off-chip image: invalid image ID\n");
    return 0;
  }

  if(header->secInfoSeg.secVer <= OTA_RUNNING_SEC_VER) {
    LOG_WARN("[OTA] Rejecting off-chip image: secVer %u <= running secVer %u\n",
             header->secInfoSeg.secVer, OTA_RUNNING_SEC_VER);
    return 0;
  }

  if(header->fixedHdr.techType != OAD_WIRELESS_TECH_TIMAC_2_4G ||
     header->imgPayload.wirelessTech != OAD_WIRELESS_TECH_TIMAC_2_4G) {
    LOG_WARN("[OTA] Rejecting off-chip image: techType 0x%04x payloadTech "
             "0x%04x\n",
             (unsigned int)header->fixedHdr.techType,
             (unsigned int)header->imgPayload.wirelessTech);
    return 0;
  }

  if(header->fixedHdr.imgType != OTA_EXPECTED_IMG_TYPE) {
    LOG_WARN("[OTA] Rejecting off-chip image: imgType %u != expected %u\n",
             header->fixedHdr.imgType, OTA_EXPECTED_IMG_TYPE);
    return 0;
  }

  if(header->fixedHdr.bimVer != BIM_VER ||
     header->fixedHdr.metaVer != META_VER ||
     header->fixedHdr.crcStat == CRC_INVALID) {
    LOG_WARN("[OTA] Rejecting off-chip image: invalid BIM metadata fields "
             "(bim %u meta %u copy 0x%02x crc 0x%02x)\n",
             header->fixedHdr.bimVer, header->fixedHdr.metaVer,
             header->fixedHdr.imgCpStat, header->fixedHdr.crcStat);
    return 0;
  }

  if(header->fixedHdr.hdrLen != sizeof(imgHdr_t)) {
    LOG_WARN("[OTA] Rejecting off-chip image: hdrLen %u != %u\n",
             header->fixedHdr.hdrLen, (unsigned int)sizeof(imgHdr_t));
    return 0;
  }

  if(header->imgPayload.segTypeImg != IMG_PAYLOAD_SEG_ID ||
     header->imgPayload.startAddr != OTA_INTERNAL_IMAGE_START_ADDR) {
    LOG_WARN("[OTA] Rejecting off-chip image: payload segment invalid "
             "(type %u start 0x%08lx)\n",
             header->imgPayload.segTypeImg,
             (unsigned long)header->imgPayload.startAddr);
    return 0;
  }

  if(header->fixedHdr.prgEntry < OTA_INTERNAL_VECTOR_MIN_ADDR ||
     header->fixedHdr.prgEntry > header->fixedHdr.imgEndAddr ||
     header->fixedHdr.imgEndAddr >= OTA_INTERNAL_BIM_ADDR) {
    LOG_WARN("[OTA] Rejecting off-chip image: invalid internal range "
             "entry=0x%08lx end=0x%08lx\n",
             (unsigned long)header->fixedHdr.prgEntry,
             (unsigned long)header->fixedHdr.imgEndAddr);
    return 0;
  }

  if(header->fixedHdr.len == 0 || header->fixedHdr.len > current_file_size) {
    LOG_WARN("[OTA] Rejecting off-chip image: invalid length %lu "
             "(file %lu)\n",
             (unsigned long)header->fixedHdr.len,
             (unsigned long)current_file_size);
    return 0;
  }

  LOG_INFO("[OTA] Candidate off-chip image validated: secVer %u > %u, "
           "size %lu, crc32 0x%08lx\n",
           header->secInfoSeg.secVer, OTA_RUNNING_SEC_VER,
           (unsigned long)header->fixedHdr.len,
           (unsigned long)header->fixedHdr.crc32);
  return 1;
}

static int
read_offchip_image_header(imgHdr_t *header)
{
  int ok = 0;

  if(!ext_flash_open(NULL)) {
    LOG_WARN("[OTA] Failed to open external flash for OAD header read\n");
    return 0;
  }

  ok = ext_flash_read(NULL, OTA_EXT_IMAGE_ADDR, sizeof(*header),
                      (uint8_t *)header);
  ext_flash_close(NULL);

  if(!ok) {
    LOG_WARN("[OTA] Failed to read OAD header at 0x%05lx\n",
             (unsigned long)OTA_EXT_IMAGE_ADDR);
  }

  return ok;
}

static int
write_offchip_bim_metadata(const imgHdr_t *header)
{
  ExtImageInfo_t metadata;
  ExtImageInfo_t readback;
  int ok = 0;

  memset(&metadata, 0xff, sizeof(metadata));
  metadata.fixedHdr = header->fixedHdr;
  memcpy(metadata.fixedHdr.imgID, expected_metadata_id,
         sizeof(metadata.fixedHdr.imgID));
  metadata.fixedHdr.imgCpStat = NEED_COPY;
  metadata.fixedHdr.crcStat = CRC_VALID;
  metadata.extFlAddr = OTA_EXT_IMAGE_ADDR;
  metadata.counter = image_softver_to_u32(&header->fixedHdr);

  if(!ext_flash_open(NULL)) {
    LOG_WARN("[OTA] Failed to open external flash for BIM metadata write\n");
    return 0;
  }

  if(!ext_flash_erase(NULL, OTA_EXT_METADATA_ADDR,
                      EXT_FLASH_ERASE_SECTOR_SIZE)) {
    LOG_WARN("[OTA] Failed to erase BIM metadata sector at 0x%05lx\n",
             (unsigned long)OTA_EXT_METADATA_ADDR);
    goto done;
  }

  if(!ext_flash_write(NULL, OTA_EXT_METADATA_ADDR, sizeof(metadata),
                      (const uint8_t *)&metadata)) {
    LOG_WARN("[OTA] Failed to write BIM metadata at 0x%05lx\n",
             (unsigned long)OTA_EXT_METADATA_ADDR);
    goto done;
  }

  if(!ext_flash_read(NULL, OTA_EXT_METADATA_ADDR, sizeof(readback),
                     (uint8_t *)&readback)) {
    LOG_WARN("[OTA] Failed to read back BIM metadata\n");
    goto done;
  }

  if(memcmp(readback.fixedHdr.imgID, expected_metadata_id,
            sizeof(readback.fixedHdr.imgID)) != 0) {
    LOG_WARN("[OTA] BIM metadata readback ID mismatch\n");
    goto done;
  }

  LOG_INFO("[OTA] Off-chip BIM metadata written at 0x%05lx "
           "(payload 0x%05lx, counter %lu)\n",
           (unsigned long)OTA_EXT_METADATA_ADDR,
           (unsigned long)metadata.extFlAddr,
           (unsigned long)metadata.counter);
  ok = 1;

done:
  ext_flash_close(NULL);
  return ok;
}

static void
ota_reset_callback(void *ptr)
{
  (void)ptr;
  LOG_INFO("[OTA] Resetting to boot staged image\n");
  SysCtrlSystemReset();
}

uint8_t
ota_sensor_backend_stage(uint8_t target)
{
  imgHdr_t header;

  (void)target;

  if(!read_offchip_image_header(&header)) {
    return 0;
  }
  if(!validate_offchip_image(&header)) {
    return 0;
  }
  if(!write_offchip_bim_metadata(&header)) {
    return 0;
  }

#if OTA_STAGE_RESET_AFTER_VERIFY
  LOG_INFO("[OTA] Reset scheduled in 5 seconds\n");
  ctimer_set(&ota_reset_timer, 5 * CLOCK_SECOND, ota_reset_callback, NULL);
#else
  LOG_INFO("[OTA] Staged image metadata written; reset disabled by "
           "OTA_STAGE_RESET_AFTER_VERIFY=0\n");
#endif

  return 1;
}

uint8_t
ota_sensor_backend_start(uint8_t target, uint32_t file_size,
                         uint16_t image_sec_ver, uint8_t *reject_status)
{
  if(target != OTA_TARGET_OFFCHIP) {
    LOG_WARN("[OTA] START rejected: invalid off-chip target %u\n", target);
    *reject_status = OTA_START_STATUS_REJECTED_TARGET;
    return 0;
  }

  if(image_sec_ver != OTA_START_SEC_VER_UNKNOWN &&
     image_sec_ver <= OTA_RUNNING_SEC_VER) {
    LOG_WARN("[OTA] START rejected: image secVer %u <= running secVer %u\n",
             image_sec_ver, OTA_RUNNING_SEC_VER);
    *reject_status = OTA_START_STATUS_REJECTED_VERSION;
    return 0;
  }

  if(file_size == 0) {
    LOG_WARN("[OTA] START rejected: empty image\n");
    *reject_status = OTA_START_STATUS_REJECTED_TARGET;
    return 0;
  }

  if(!ext_flash_open(NULL)) {
    LOG_WARN("[OTA] Failed to open external flash for initial erase\n");
    *reject_status = OTA_START_STATUS_REJECTED_TARGET;
    return 0;
  }

  LOG_INFO("[OTA] Erasing initial OTA image sector at 0x%05lx...\n",
           (unsigned long)OTA_EXT_IMAGE_ADDR);
  uint8_t ok = ext_flash_erase(NULL, OTA_EXT_IMAGE_ADDR,
                               EXT_FLASH_ERASE_SECTOR_SIZE);
  ext_flash_close(NULL);

  if(!ok) {
    LOG_WARN("[OTA] Failed to erase initial OTA image sector\n");
    *reject_status = OTA_START_STATUS_REJECTED_TARGET;
    return 0;
  }

  return 1;
}

uint8_t
ota_sensor_backend_bounds_ok(uint8_t target, uint32_t offset, uint16_t length)
{
  (void)target;
  return length > 0 && offset + length <= current_file_size;
}

uint8_t
ota_sensor_backend_erase_sector(uint8_t target, uint32_t sector_offset)
{
  uint8_t ok;

  (void)target;
  if(!ext_flash_open(NULL)) {
    LOG_WARN("[OTA] Failed to open external flash for erase\n");
    return 0;
  }

  LOG_INFO("[OTA] Erasing image sector at ext-flash 0x%05lx...\n",
           (unsigned long)(OTA_EXT_IMAGE_ADDR + sector_offset));
  ok = ext_flash_erase(NULL, OTA_EXT_IMAGE_ADDR + sector_offset,
                       EXT_FLASH_ERASE_SECTOR_SIZE);
  ext_flash_close(NULL);
  return ok;
}

uint8_t
ota_sensor_backend_write(uint8_t target, uint32_t offset, const uint8_t *data,
                         uint16_t length)
{
  uint8_t ok;

  (void)target;
  if(!ext_flash_open(NULL)) {
    LOG_WARN("[OTA] Failed to open external flash for write\n");
    return 0;
  }

  ok = ext_flash_write(NULL, OTA_EXT_IMAGE_ADDR + offset, length, data);
  ext_flash_close(NULL);
  return ok;
}

uint8_t
ota_sensor_backend_read(uint8_t target, uint32_t offset, uint16_t length,
                        uint8_t *buf)
{
  uint8_t ok;

  (void)target;
  if(!ext_flash_open(NULL)) {
    LOG_WARN("[OTA] Failed to open external flash for read\n");
    return 0;
  }

  ok = ext_flash_read(NULL, OTA_EXT_IMAGE_ADDR + offset, length, buf);
  ext_flash_close(NULL);
  return ok;
}

void
ota_sensor_boot_check(void)
{
  LOG_INFO("[OTA] Running secVer=%u, off-chip payload base=0x%05lx, "
           "BIM metadata base=0x%05lx\n",
           OTA_RUNNING_SEC_VER,
           (unsigned long)OTA_EXT_IMAGE_ADDR,
           (unsigned long)OTA_EXT_METADATA_ADDR);
}

void
ota_sensor_confirm_stable(void)
{
}
