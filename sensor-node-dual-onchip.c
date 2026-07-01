#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "dev/leds.h"
#include "oad_image_header_app.h"
#include "ota-flash.h"
#include "ota-metadata.h"
#include "ota-sensor-backend.h"
#include "ota.h"
#include "sys/log.h"
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)

#define MAX_BOOT_ATTEMPTS 3

static ota_boot_metadata_t current_metadata;
static uint8_t current_slot = OTA_SLOT_INVALID;
static uint8_t rejected_same_slot = 0;

static char
slot_char(uint8_t slot)
{
  return slot == OTA_SLOT_A ? 'A' : (slot == OTA_SLOT_B ? 'B' : '?');
}

const char *
ota_sensor_backend_target_name(uint8_t target)
{
  return ota_target_name(target);
}

uint16_t
ota_sensor_backend_running_sec_ver(void)
{
  return ota_image_header.secInfoSeg.secVer;
}

uint32_t
ota_sensor_backend_erase_sector_size(void)
{
  return OTA_FLASH_ERASE_SECTOR_SIZE;
}

static uint32_t
image_softver_to_u32(const uint8_t soft_ver[4])
{
  uint32_t version;
  memcpy(&version, soft_ver, sizeof(version));
  return version;
}

static int
validate_downloaded_image(uint8_t target_slot, imgHdr_t *header,
                          uint32_t *image_version, uint32_t *image_size,
                          uint32_t *image_crc32)
{
  uint32_t slot_start = ota_flash_slot_start(target_slot);
  uint32_t slot_end = slot_start + ota_flash_slot_size(target_slot);
  uint8_t current_sec_ver = ota_image_header.secInfoSeg.secVer;
  uint8_t candidate_sec_ver;

  if(!oad_image_header_read(slot_start, header)) {
    LOG_WARN("[OTA] Target slot %c does not contain a valid OAD header\n",
             slot_char(target_slot));
    return 0;
  }

  candidate_sec_ver = header->secInfoSeg.secVer;
  if(candidate_sec_ver <= current_sec_ver) {
    LOG_WARN("[OTA] Rejecting slot %c image: secVer %u <= running secVer %u\n",
             slot_char(target_slot), candidate_sec_ver, current_sec_ver);
    return 0;
  }

  if(header->fixedHdr.imgType != OAD_IMG_TYPE_APPSTACKLIB ||
     header->fixedHdr.techType != OAD_WIRELESS_TECH_TIMAC_2_4G) {
    LOG_WARN("[OTA] Rejecting slot %c image: imgType %u techType 0x%08lx\n",
             slot_char(target_slot), header->fixedHdr.imgType,
             (unsigned long)header->fixedHdr.techType);
    return 0;
  }

  if(header->imgPayload.startAddr != slot_start ||
     header->fixedHdr.prgEntry < slot_start ||
     header->fixedHdr.prgEntry >= slot_end ||
     header->fixedHdr.imgEndAddr < slot_start ||
     header->fixedHdr.imgEndAddr >= slot_end ||
     header->fixedHdr.imgEndAddr < header->imgPayload.startAddr) {
    LOG_WARN("[OTA] Rejecting slot %c image: invalid address range "
             "start=0x%08lx entry=0x%08lx end=0x%08lx\n",
             slot_char(target_slot),
             (unsigned long)header->imgPayload.startAddr,
             (unsigned long)header->fixedHdr.prgEntry,
             (unsigned long)header->fixedHdr.imgEndAddr);
    return 0;
  }

  *image_size = header->fixedHdr.imgEndAddr - slot_start + 1;
  if(*image_size == 0 || *image_size > current_file_size ||
     *image_size > ota_flash_slot_size(target_slot)) {
    LOG_WARN("[OTA] Rejecting slot %c image: invalid image size %lu "
             "(file %lu, slot %lu)\n",
             slot_char(target_slot), (unsigned long)*image_size,
             (unsigned long)current_file_size,
             (unsigned long)ota_flash_slot_size(target_slot));
    return 0;
  }

  *image_version = image_softver_to_u32(header->fixedHdr.softVer);
  *image_crc32 = header->fixedHdr.crc32;
  LOG_INFO("[OTA] Candidate slot %c validated: secVer %u > %u, "
           "size %lu, crc32 0x%08lx\n",
           slot_char(target_slot), candidate_sec_ver, current_sec_ver,
           (unsigned long)*image_size, (unsigned long)*image_crc32);
  return 1;
}

uint8_t
ota_sensor_backend_stage(uint8_t target)
{
  imgHdr_t target_header;
  ota_boot_metadata_t metadata;
  uint32_t image_version;
  uint32_t image_size;
  uint32_t image_crc32;

  if(!validate_downloaded_image(target, &target_header, &image_version,
                                &image_size, &image_crc32)) {
    return 0;
  }

  if(!ota_flash_read_metadata(&metadata)) {
    uint32_t running_version =
        image_softver_to_u32(ota_image_header.fixedHdr.softVer);
    uint32_t running_start = (uint32_t)&ota_image_header;
    uint32_t running_size =
        ota_image_header.fixedHdr.imgEndAddr - running_start + 1;
    LOG_WARN("[OTA] Metadata missing/invalid. Reinitializing before staging.\n");
    ota_metadata_init_factory(&metadata, current_slot, running_version,
                              running_size, ota_image_header.fixedHdr.crc32);
  }

  if(!ota_metadata_mark_verified(&metadata, target, image_version,
                                 image_size, image_crc32) ||
     !ota_metadata_stage_verified_image(&metadata, target)) {
    LOG_WARN("[OTA] Failed to stage slot %c in metadata\n", slot_char(target));
    return 0;
  }

  if(!ota_flash_write_metadata(&metadata)) {
    LOG_WARN("[OTA] Failed to persist metadata for slot %c\n",
             slot_char(target));
    return 0;
  }

  current_metadata = metadata;
  LOG_INFO("[OTA] Slot %c staged\n", slot_char(target));
  return 1;
}

void
ota_sensor_backend_reset_to_staged_image(void)
{
  LOG_INFO("[OTA] Resetting to boot staged image\n");
  SysCtrlSystemReset();
}

uint8_t
ota_sensor_backend_start(uint8_t target, uint32_t file_size,
                         uint16_t image_sec_ver, uint8_t *reject_status)
{
  rejected_same_slot = 0;
  ota_flash_reset_erase_tracking();

  if(target > OTA_SLOT_B) {
    LOG_WARN("[OTA] Invalid dual-onchip target %u\n", target);
    *reject_status = OTA_START_STATUS_REJECTED_TARGET;
    return 0;
  }

  if(image_sec_ver != OTA_START_SEC_VER_UNKNOWN &&
     image_sec_ver <= ota_sensor_backend_running_sec_ver()) {
    LOG_WARN("[OTA] Rejecting START: image secVer %u <= running secVer %u\n",
             image_sec_ver, ota_sensor_backend_running_sec_ver());
    *reject_status = OTA_START_STATUS_REJECTED_VERSION;
    return 0;
  }

  if(file_size == 0 || file_size > ota_flash_slot_size(target)) {
    LOG_WARN("[OTA] Image size %lu does not fit slot %c (max %lu)\n",
             (unsigned long)file_size, slot_char(target),
             (unsigned long)ota_flash_slot_size(target));
    *reject_status = OTA_START_STATUS_REJECTED_TARGET;
    return 0;
  }

  current_slot = ota_flash_current_slot();
  if(target == current_slot) {
    LOG_WARN("[OTA] Rejecting image for current running slot %c\n",
             slot_char(target));
    rejected_same_slot = 1;
    *reject_status = OTA_START_STATUS_REJECTED_SAME_SLOT;
    return 0;
  }

  LOG_INFO("[OTA] Current slot %c, target slot %c, size %lu\n",
           slot_char(current_slot), slot_char(target), (unsigned long)file_size);

  if(!ota_flash_erase_target_sector(target, 0)) {
    LOG_WARN("[OTA] Failed to erase initial sector in slot %c\n",
             slot_char(target));
    *reject_status = rejected_same_slot ? OTA_START_STATUS_REJECTED_SAME_SLOT :
                                          OTA_START_STATUS_REJECTED_TARGET;
    return 0;
  }

  return 1;
}

uint8_t
ota_sensor_backend_bounds_ok(uint8_t target, uint32_t offset, uint16_t length)
{
  return ota_flash_target_bounds_ok(target, offset, length);
}

uint8_t
ota_sensor_backend_erase_sector(uint8_t target, uint32_t sector_offset)
{
  return ota_flash_erase_target_sector(target, sector_offset);
}

uint8_t
ota_sensor_backend_write(uint8_t target, uint32_t offset, const uint8_t *data,
                         uint16_t length)
{
  return ota_flash_write_target(target, offset, data, length);
}

uint8_t
ota_sensor_backend_read(uint8_t target, uint32_t offset, uint16_t length,
                        uint8_t *buf)
{
  return ota_flash_read_target(target, offset, length, buf);
}

void
ota_sensor_boot_check(void)
{
  uint32_t header_physical_address;
  uint32_t current_version;
  uint32_t physical_crc;
  uint32_t current_size;
  uint32_t saved_crc;
  uint32_t reset_source;
  uint32_t shadow_addr;
  imgHdr_t shadow_header;

  current_slot = ota_flash_current_slot();
  header_physical_address = (uint32_t)&ota_image_header;
  physical_crc = ota_image_header.fixedHdr.crc32;
  memcpy(&current_version, ota_image_header.fixedHdr.softVer,
         sizeof(current_version));
  current_size = ota_image_header.fixedHdr.imgEndAddr -
                 header_physical_address + 1;

  LOG_INFO("[BIM] Running slot %c at 0x%08lx, SecVer %u\n",
           slot_char(current_slot), (unsigned long)header_physical_address,
           ota_image_header.secInfoSeg.secVer);

  if(!ota_flash_read_metadata(&current_metadata)) {
    LOG_INFO("[BIM] Metadata not found. Initializing factory metadata.\n");
    ota_metadata_init_factory(&current_metadata, current_slot, current_version,
                              current_size, physical_crc);
    ota_flash_write_metadata(&current_metadata);
    return;
  }

  saved_crc =
      current_slot == OTA_SLOT_A ? current_metadata.crc_a :
                                   current_metadata.crc_b;
  if((current_slot != current_metadata.active_slot &&
      current_slot != current_metadata.candidate_slot) ||
     physical_crc != saved_crc) {
    LOG_INFO("[BIM] New image detected in slot %c\n", slot_char(current_slot));
    ota_metadata_set_candidate(&current_metadata, current_slot,
                               current_version, current_size, physical_crc);
  } else {
    reset_source = SysCtrlResetSourceGet();
    if(reset_source != RSTSRC_PWR_ON && reset_source != RSTSRC_PIN_RESET &&
       reset_source != RSTSRC_WAKEUP_FROM_SHUTDOWN) {
      ota_metadata_increment_boot_attempts(&current_metadata);
      LOG_WARN("[BIM] Abnormal reset source %lu, boot attempts %lu/%u\n",
               (unsigned long)reset_source,
               (unsigned long)current_metadata.boot_attempts,
               MAX_BOOT_ATTEMPTS);
    }
  }

  shadow_addr = current_slot == OTA_SLOT_A ? OTA_SLOT_B_START :
                                            OTA_SLOT_A_START;
  if(oad_image_header_read(shadow_addr, &shadow_header)) {
    uint32_t shadow_phys_crc = shadow_header.fixedHdr.crc32;
    uint32_t shadow_saved_crc =
        current_slot == OTA_SLOT_A ? current_metadata.crc_b :
                                     current_metadata.crc_a;
    if(shadow_phys_crc != shadow_saved_crc) {
      uint8_t fallback_slot =
          current_slot == OTA_SLOT_A ? OTA_SLOT_B : OTA_SLOT_A;
      ota_metadata_clear_slot(&current_metadata, fallback_slot);
    }
  }

  ota_flash_write_metadata(&current_metadata);

  if(current_metadata.boot_attempts > MAX_BOOT_ATTEMPTS) {
    uint8_t fallback_slot = current_slot == OTA_SLOT_A ? OTA_SLOT_B :
                                                        OTA_SLOT_A;
    uint32_t fallback_addr = ota_flash_slot_start(fallback_slot);
    const uint8_t oad_id[8] = {'C', 'C', '1', '3', 'x', '2', 'R', '1'};
    bool fallback_invalid =
        fallback_slot == OTA_SLOT_A ?
            current_metadata.state_a == OTA_IMAGE_STATE_INVALID :
            current_metadata.state_b == OTA_IMAGE_STATE_INVALID;

    LOG_WARN("[BIM] Boot attempt limit exceeded in slot %c\n",
             slot_char(current_slot));

    if(fallback_invalid || memcmp((const void *)fallback_addr, oad_id, 8) != 0) {
      LOG_ERR("[BIM] No valid fallback image. Staying in safe mode.\n");
      leds_on(LEDS_RED);
      while(1) {
      }
    }

    ota_metadata_mark_slot_invalid(&current_metadata, current_slot);
    ota_metadata_confirm_running_image(&current_metadata, fallback_slot);
    ota_flash_write_metadata(&current_metadata);

    if(!ota_flash_invalidate_image_header(header_physical_address)) {
      LOG_ERR("[BIM] Failed to invalidate crashing image header.\n");
      leds_on(LEDS_RED);
      while(1) {
      }
    }

    SysCtrlSystemReset();
  }
}

void
ota_sensor_confirm_stable(void)
{
  if(current_slot <= OTA_SLOT_B &&
     ota_metadata_crc_is_valid(&current_metadata) &&
     current_metadata.boot_attempts > 0) {
    LOG_INFO("[BIM] Stability window passed. Confirming slot %c\n",
             slot_char(current_slot));
    ota_metadata_confirm_running_image(&current_metadata, current_slot);
    ota_flash_write_metadata(&current_metadata);
  }
}
