#ifndef OTA_METADATA_H_
#define OTA_METADATA_H_

#include <stdbool.h>
#include <stdint.h>

#define OTA_IMAGE_MAGIC         0x4F544131u

#define OTA_IMAGE_STATE_EMPTY       0u
#define OTA_IMAGE_STATE_DOWNLOADING 1u
#define OTA_IMAGE_STATE_VERIFIED    2u
#define OTA_IMAGE_STATE_PENDING     3u
#define OTA_IMAGE_STATE_CONFIRMED   4u
#define OTA_IMAGE_STATE_INVALID     5u

#ifndef OTA_SLOT_A
#define OTA_SLOT_A              0u
#endif
#ifndef OTA_SLOT_B
#define OTA_SLOT_B              1u
#endif
#define OTA_SLOT_NONE           0xFFFFFFFFu

typedef struct {
  uint32_t magic;
  uint32_t active_slot;
  uint32_t candidate_slot;
  uint32_t state_a;
  uint32_t state_b;
  uint32_t version_a;
  uint32_t version_b;
  uint32_t size_a;
  uint32_t size_b;
  uint32_t crc_a;
  uint32_t crc_b;
  uint32_t boot_attempts;
  uint32_t metadata_crc32;
} ota_boot_metadata_t;

uint32_t ota_crc32_buffer(const void *buf, unsigned len);
bool ota_metadata_crc_is_valid(const ota_boot_metadata_t *metadata);
bool ota_metadata_mark_verified(ota_boot_metadata_t *metadata,
                                uint32_t slot,
                                uint32_t version,
                                uint32_t image_size,
                                uint32_t image_crc32);
bool ota_metadata_stage_verified_image(ota_boot_metadata_t *metadata,
                                       uint32_t slot);
bool ota_metadata_confirm_running_image(ota_boot_metadata_t *metadata,
                                        uint32_t slot);

void ota_metadata_init_factory(ota_boot_metadata_t *metadata, uint32_t active_slot, uint32_t version, uint32_t size, uint32_t crc);
void ota_metadata_increment_boot_attempts(ota_boot_metadata_t *metadata);
void ota_metadata_mark_slot_invalid(ota_boot_metadata_t *metadata, uint32_t slot);
void ota_metadata_set_candidate(ota_boot_metadata_t *metadata, uint32_t candidate_slot, uint32_t version, uint32_t size, uint32_t crc);
void ota_metadata_clear_candidate(ota_boot_metadata_t *metadata);
void ota_metadata_clear_slot(ota_boot_metadata_t *metadata, uint32_t slot);

#endif
