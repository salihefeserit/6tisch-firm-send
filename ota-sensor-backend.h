#ifndef OTA_SENSOR_BACKEND_H_
#define OTA_SENSOR_BACKEND_H_

#include "ota.h"

const char *ota_sensor_backend_target_name(uint8_t target);
uint16_t ota_sensor_backend_running_sec_ver(void);
uint8_t ota_sensor_backend_start(uint8_t target, uint32_t file_size,
                                 uint16_t image_sec_ver,
                                 uint8_t *reject_status);
uint8_t ota_sensor_backend_bounds_ok(uint8_t target, uint32_t offset,
                                     uint16_t length);
uint8_t ota_sensor_backend_erase_sector(uint8_t target, uint32_t sector_offset);
uint8_t ota_sensor_backend_write(uint8_t target, uint32_t offset,
                                 const uint8_t *data, uint16_t length);
uint8_t ota_sensor_backend_read(uint8_t target, uint32_t offset,
                                uint16_t length, uint8_t *buf);
uint8_t ota_sensor_backend_stage(uint8_t target);

#endif /* OTA_SENSOR_BACKEND_H_ */
