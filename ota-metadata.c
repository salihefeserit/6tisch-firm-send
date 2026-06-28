#include "ota-metadata.h"

static uint32_t crc32_update(uint32_t crc, const void *buf, unsigned len) {
  const uint8_t *p = (const uint8_t *)buf;
  unsigned i;
  int bit;

  crc = ~crc;
  for (i = 0; i < len; i++) {
    crc ^= p[i];
    for (bit = 0; bit < 8; bit++) {
      if (crc & 1u) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc >>= 1;
      }
    }
  }
  return ~crc;
}

uint32_t ota_crc32_buffer(const void *buf, unsigned len) {
  return crc32_update(0u, buf, len);
}

static void ota_metadata_refresh_crc(ota_boot_metadata_t *metadata) {
  metadata->metadata_crc32 = 0u;
  metadata->metadata_crc32 = ota_crc32_buffer(metadata, sizeof(*metadata));
}

bool ota_metadata_crc_is_valid(const ota_boot_metadata_t *metadata) {
  ota_boot_metadata_t tmp;

  if (metadata->magic != OTA_IMAGE_MAGIC) {
    return false;
  }

  tmp = *metadata;
  tmp.metadata_crc32 = 0u;
  return ota_crc32_buffer(&tmp, sizeof(tmp)) == metadata->metadata_crc32;
}

bool ota_metadata_mark_verified(ota_boot_metadata_t *metadata, uint32_t slot,
                                uint32_t version, uint32_t image_size,
                                uint32_t image_crc32) {
  if (metadata->magic != OTA_IMAGE_MAGIC) {
    return false;
  }

  if (slot == OTA_SLOT_A) {
    metadata->version_a = version;
    metadata->size_a = image_size;
    metadata->crc_a = image_crc32;
    metadata->state_a = OTA_IMAGE_STATE_VERIFIED;
  } else if (slot == OTA_SLOT_B) {
    metadata->version_b = version;
    metadata->size_b = image_size;
    metadata->crc_b = image_crc32;
    metadata->state_b = OTA_IMAGE_STATE_VERIFIED;
  } else {
    return false;
  }

  ota_metadata_refresh_crc(metadata);
  return true;
}

bool ota_metadata_stage_verified_image(ota_boot_metadata_t *metadata,
                                       uint32_t slot) {
  if (!ota_metadata_crc_is_valid(metadata)) {
    return false;
  }

  if (slot == OTA_SLOT_A && metadata->state_a == OTA_IMAGE_STATE_VERIFIED) {
    metadata->candidate_slot = OTA_SLOT_A;
    metadata->state_a = OTA_IMAGE_STATE_PENDING;
  } else if (slot == OTA_SLOT_B &&
             metadata->state_b == OTA_IMAGE_STATE_VERIFIED) {
    metadata->candidate_slot = OTA_SLOT_B;
    metadata->state_b = OTA_IMAGE_STATE_PENDING;
  } else {
    return false;
  }

  metadata->boot_attempts = 0u;
  ota_metadata_refresh_crc(metadata);
  return true;
}

bool ota_metadata_confirm_running_image(ota_boot_metadata_t *metadata,
                                        uint32_t slot) {
  if (!ota_metadata_crc_is_valid(metadata)) {
    return false;
  }

  if (slot == OTA_SLOT_A) {
    metadata->active_slot = OTA_SLOT_A;
    metadata->candidate_slot = OTA_SLOT_NONE;
    metadata->state_a = OTA_IMAGE_STATE_CONFIRMED;
  } else if (slot == OTA_SLOT_B) {
    metadata->active_slot = OTA_SLOT_B;
    metadata->candidate_slot = OTA_SLOT_NONE;
    metadata->state_b = OTA_IMAGE_STATE_CONFIRMED;
  } else {
    return false;
  }

  metadata->boot_attempts = 0u;
  ota_metadata_refresh_crc(metadata);
  return true;
}

void ota_metadata_init_factory(ota_boot_metadata_t *metadata,
                               uint32_t active_slot, uint32_t version,
                               uint32_t size, uint32_t crc) {
  metadata->magic = OTA_IMAGE_MAGIC;
  metadata->active_slot = active_slot;
  metadata->candidate_slot = OTA_SLOT_NONE;
  metadata->boot_attempts = 0;

  if (active_slot == OTA_SLOT_A) {
    metadata->state_a = OTA_IMAGE_STATE_PENDING;
    metadata->state_b = OTA_IMAGE_STATE_EMPTY;
    metadata->crc_a = crc;
    metadata->crc_b = 0;
    metadata->version_a = version;
    metadata->size_a = size;
    metadata->version_b = 0;
    metadata->size_b = 0;
  } else {
    metadata->state_b = OTA_IMAGE_STATE_PENDING;
    metadata->state_a = OTA_IMAGE_STATE_EMPTY;
    metadata->crc_b = crc;
    metadata->crc_a = 0;
    metadata->version_b = version;
    metadata->size_b = size;
    metadata->version_a = 0;
    metadata->size_a = 0;
  }
  ota_metadata_refresh_crc(metadata);
}

void ota_metadata_increment_boot_attempts(ota_boot_metadata_t *metadata) {
  metadata->boot_attempts++;
  ota_metadata_refresh_crc(metadata);
}

void ota_metadata_mark_slot_invalid(ota_boot_metadata_t *metadata,
                                    uint32_t slot) {
  if (slot == OTA_SLOT_A) {
    metadata->state_a = OTA_IMAGE_STATE_INVALID;
  } else if (slot == OTA_SLOT_B) {
    metadata->state_b = OTA_IMAGE_STATE_INVALID;
  }
  ota_metadata_refresh_crc(metadata);
}

void ota_metadata_set_candidate(ota_boot_metadata_t *metadata,
                                uint32_t candidate_slot, uint32_t version,
                                uint32_t size, uint32_t crc) {
  metadata->candidate_slot = candidate_slot;
  metadata->boot_attempts = 1;

  if (candidate_slot == OTA_SLOT_A) {
    metadata->state_a = OTA_IMAGE_STATE_PENDING;
    metadata->crc_a = crc;
    metadata->version_a = version;
    metadata->size_a = size;
  } else {
    metadata->state_b = OTA_IMAGE_STATE_PENDING;
    metadata->crc_b = crc;
    metadata->version_b = version;
    metadata->size_b = size;
  }
  ota_metadata_refresh_crc(metadata);
}

void ota_metadata_clear_candidate(ota_boot_metadata_t *metadata) {
  metadata->candidate_slot = OTA_SLOT_NONE;
  metadata->boot_attempts = 0;
  ota_metadata_refresh_crc(metadata);
}

void ota_metadata_clear_slot(ota_boot_metadata_t *metadata, uint32_t slot) {
  if (slot == OTA_SLOT_A) {
    metadata->state_a = OTA_IMAGE_STATE_EMPTY;
    metadata->crc_a = 0;
    metadata->version_a = 0;
    metadata->size_a = 0;
  } else if (slot == OTA_SLOT_B) {
    metadata->state_b = OTA_IMAGE_STATE_EMPTY;
    metadata->crc_b = 0;
    metadata->version_b = 0;
    metadata->size_b = 0;
  }
  ota_metadata_refresh_crc(metadata);
}
