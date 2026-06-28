#include "ota-flash.h"
#include "contiki.h"
#include "oad_image_header_app.h"
#include "ota.h"
#include "ota-metadata.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/flash.h)
#include DeviceFamily_constructPath(driverlib/vims.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include <ti/drivers/dpl/HwiP.h>

/* Base address of the reserved memory region from the linker script */
extern uint8_t _metadata_flash_addr[];
#define NV_METADATA_PAGE_ADDRESS ((uint32_t)_metadata_flash_addr)

static uint32_t erased_sector_addr = 0xFFFFFFFFUL;

static uint32_t disable_flash_cache(void) {
  uint32_t mode = VIMSModeGet(VIMS_BASE);

  VIMSLineBufDisable(VIMS_BASE);
  if (mode != VIMS_MODE_DISABLED) {
    VIMSModeSet(VIMS_BASE, VIMS_MODE_DISABLED);
    while (VIMSModeGet(VIMS_BASE) != VIMS_MODE_DISABLED) {
    }
  }

  return mode;
}

static void restore_flash_cache(uint32_t mode) {
  if (mode != VIMS_MODE_DISABLED) {
    VIMSModeSet(VIMS_BASE, mode);
    while (VIMSModeGet(VIMS_BASE) != mode) {
    }
  }
  VIMSLineBufEnable(VIMS_BASE);
}

static bool program_flash(uint32_t address, const uint8_t *data,
                          uint32_t length) {
  uint32_t cache_mode;
  uintptr_t key;
  uint32_t status;

  cache_mode = disable_flash_cache();
  key = HwiP_disable();
  status = FlashProgram((uint8_t *)data, address, length);
  HwiP_restore(key);
  restore_flash_cache(cache_mode);

  return status == FAPI_STATUS_SUCCESS;
}

static bool erase_flash_sector(uint32_t address) {
  uint32_t cache_mode;
  uintptr_t key;
  uint32_t status;

  cache_mode = disable_flash_cache();
  key = HwiP_disable();
  status = FlashSectorErase(address);
  HwiP_restore(key);
  restore_flash_cache(cache_mode);

  return status == FAPI_STATUS_SUCCESS;
}

/* ========================================================================
   Function to Persist Metadata to Flash
   ======================================================================= */
bool ota_flash_write_metadata(const ota_boot_metadata_t *metadata) {
  bool success = true;

  /* Check if flash already contains the exact same metadata.
     If it does, skip erasing and programming to prevent flash wear-out. */
  ota_boot_metadata_t current_flash_meta;
  if (ota_flash_read_metadata(&current_flash_meta)) {
    if (memcmp(metadata, &current_flash_meta, sizeof(ota_boot_metadata_t)) ==
        0) {
      return true; /* Flash already matches, no wear needed */
    }
  }

  /* We must fully erase the 8 KB page before writing to it */
  success = erase_flash_sector(NV_METADATA_PAGE_ADDRESS);

  if (success) {
    /* Write the struct from RAM to the cleared page byte by byte */
    success = program_flash(NV_METADATA_PAGE_ADDRESS, (const uint8_t *)metadata,
                            sizeof(ota_boot_metadata_t));
  }

  return success;
}

/* ========================================================================
   Function to Read from Flash to RAM at Boot-up (or on Demand)
   ======================================================================== */
bool ota_flash_read_metadata(ota_boot_metadata_t *metadata) {
  /* Flash memory is directly readable in ARM architecture.
      All we need to do is point a pointer to that address and copy the data. */
  ota_boot_metadata_t *flash_backup =
      (ota_boot_metadata_t *)NV_METADATA_PAGE_ADDRESS;

  /* Copying operation */
  *metadata = *flash_backup;

  /* Is the data we read a valid metadata (Does the magic word match)? */
  return ota_metadata_crc_is_valid(metadata);
}

/* ========================================================================
   Function to invalidate the OAD Image Header
   ======================================================================== */
bool ota_flash_invalidate_image_header(uint32_t header_physical_address) {
  /*
   * The 'crcStat' field is located at offset 13 (0x0D) in the fixed header.
   * We will write 0xFC to it, which BIM interprets as "Invalid CRC".
   */
  uint32_t crc_stat_address = header_physical_address + 13;
  const uint8_t invalid_status = 0xFC;
  return program_flash(crc_stat_address, &invalid_status, sizeof(invalid_status));
}

uint8_t ota_flash_current_slot(void) {
  uint32_t header_addr = (uint32_t)&ota_image_header;

  return header_addr < OTA_SLOT_B_START ? OTA_SLOT_A : OTA_SLOT_B;
}

uint32_t ota_flash_slot_start(uint8_t slot) {
  return slot == OTA_SLOT_A ? OTA_SLOT_A_START : OTA_SLOT_B_START;
}

uint32_t ota_flash_slot_size(uint8_t slot) {
  return slot == OTA_SLOT_A ? OTA_SLOT_A_SIZE : OTA_SLOT_B_SIZE;
}

void ota_flash_reset_erase_tracking(void) {
  erased_sector_addr = 0xFFFFFFFFUL;
}

bool ota_flash_target_bounds_ok(uint8_t slot, uint32_t offset,
                                uint32_t length) {
  uint32_t size;

  if (slot > OTA_SLOT_B || offset > UINT32_MAX - length) {
    return false;
  }

  size = ota_flash_slot_size(slot);
  return offset + length <= size;
}

bool ota_flash_erase_target_sector(uint8_t slot, uint32_t sector_offset) {
  uint32_t abs_addr;

  if ((sector_offset % OTA_FLASH_ERASE_SECTOR_SIZE) != 0 ||
      !ota_flash_target_bounds_ok(slot, sector_offset,
                                  OTA_FLASH_ERASE_SECTOR_SIZE)) {
    return false;
  }

  abs_addr = ota_flash_slot_start(slot) + sector_offset;
  if (abs_addr == erased_sector_addr) {
    return true;
  }

  if (!erase_flash_sector(abs_addr)) {
    return false;
  }

  erased_sector_addr = abs_addr;
  return true;
}

bool ota_flash_write_target(uint8_t slot, uint32_t offset, const uint8_t *data,
                            uint16_t length) {
  uint32_t words[17];
  uint16_t padded_len;

  if (length == 0) {
    return true;
  }

  if ((offset & 0x3) != 0 || length > 64 ||
      !ota_flash_target_bounds_ok(slot, offset, length)) {
    return false;
  }

  padded_len = (length + 3) & ~0x3;
  memset(words, 0xff, sizeof(words));
  memcpy(words, data, length);

  return program_flash(ota_flash_slot_start(slot) + offset,
                       (const uint8_t *)words, padded_len);
}

bool ota_flash_read_target(uint8_t slot, uint32_t offset, uint16_t length,
                           uint8_t *buf) {
  if (!ota_flash_target_bounds_ok(slot, offset, length)) {
    return false;
  }

  memcpy(buf, (const void *)(ota_flash_slot_start(slot) + offset), length);
  return true;
}
