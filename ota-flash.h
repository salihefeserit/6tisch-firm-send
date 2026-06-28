#ifndef OTA_FLASH_H_
#define OTA_FLASH_H_

#include "ota-metadata.h"
#include <stdbool.h>
#include <stdint.h>

bool ota_flash_read_metadata(ota_boot_metadata_t *metadata);
bool ota_flash_write_metadata(const ota_boot_metadata_t *metadata);
bool ota_flash_invalidate_image_header(uint32_t header_physical_address);
uint8_t ota_flash_current_slot(void);
uint32_t ota_flash_slot_start(uint8_t slot);
uint32_t ota_flash_slot_size(uint8_t slot);
void ota_flash_reset_erase_tracking(void);
bool ota_flash_target_bounds_ok(uint8_t slot, uint32_t offset, uint32_t length);
bool ota_flash_erase_target_sector(uint8_t slot, uint32_t sector_offset);
bool ota_flash_write_target(uint8_t slot, uint32_t offset, const uint8_t *data,
                            uint16_t length);
bool ota_flash_read_target(uint8_t slot, uint32_t offset, uint16_t length,
                           uint8_t *buf);

#endif
