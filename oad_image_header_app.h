#ifndef OAD_IMAGE_HEADER_APP_H_
#define OAD_IMAGE_HEADER_APP_H_

#include <stdbool.h>
#include <stdint.h>

#include <ti/common/cc26xx/oad/oad_image_header.h>

extern const imgHdr_t ota_image_header;

bool oad_image_header_read(uint32_t flash_address, imgHdr_t *header);

#endif /* OAD_IMAGE_HEADER_APP_H_ */
