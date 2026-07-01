#ifndef OTA_H_
#define OTA_H_

#include "contiki.h"
#include "net/ipv6/simple-udp.h"
#include "sys/node-id.h"
#include "sys/ctimer.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/netstack.h"
#include "net/routing/routing.h"
#include "services/orchestra/orchestra.h"
#include "services/simple-energest/simple-energest.h"

#ifndef OTA_WITH_BIM_DUAL_ONCHIP
#define OTA_WITH_BIM_DUAL_ONCHIP 0
#endif
#ifndef OTA_WITH_BIM_OFFCHIP
#define OTA_WITH_BIM_OFFCHIP 0
#endif
#if OTA_WITH_BIM_OFFCHIP
#include "ext-flash.h"
#endif

#include "lib/crc16.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDP_PORT 8765

/* -------------------------------------------------------------------------- */
/* Wire protocol                                                               */
/* -------------------------------------------------------------------------- */

#define PKT_TYPE_START 0
#define PKT_TYPE_DATA 1
#define PKT_TYPE_VERIFY 2
#define PKT_TYPE_BITMAP_REPORT 3
#define PKT_TYPE_PAGE_END 4
#define PKT_TYPE_START_REPORT 5
#define PKT_TYPE_STAGE_REPORT 6

/* PAGE/bitmap report status values. */
#define OTA_REPORT_STATUS_OK 0
#define OTA_REPORT_STATUS_REJECTED_SAME_SLOT 1

/* START admission status values. */
#define OTA_START_STATUS_ACCEPTED 0
#define OTA_START_STATUS_REJECTED_VERSION 1
#define OTA_START_STATUS_REJECTED_TARGET 2
#define OTA_START_STATUS_REJECTED_SAME_SLOT 3

/* Stage/reboot status values. */
#define OTA_STAGE_STATUS_STAGED 0
#define OTA_STAGE_STATUS_REBOOTING_SOON 1
#define OTA_STAGE_STATUS_STAGE_FAILED 2

#ifndef OTA_STAGE_RESET_AFTER_VERIFY
#define OTA_STAGE_RESET_AFTER_VERIFY 1
#endif
#ifndef OTA_STAGE_LEAF_STABILITY_SECONDS
#define OTA_STAGE_LEAF_STABILITY_SECONDS 5
#endif
#ifndef OTA_STAGE_REBOOT_DELAY_SECONDS
#define OTA_STAGE_REBOOT_DELAY_SECONDS 2
#endif

/* Architecture-neutral target identifiers carried in fw_packet_t.target_slot. */
#ifndef OTA_SLOT_A
#define OTA_SLOT_A 0
#endif
#ifndef OTA_SLOT_B
#define OTA_SLOT_B 1
#endif
#define OTA_SLOT_INVALID 0xff

#define OTA_TARGET_OFFCHIP 0x80
#define OTA_TARGET_INVALID 0xff

#define OTA_START_SEC_VER_UNKNOWN 0

#define PAGE_SIZE 4096

/* -------------------------------------------------------------------------- */
/* Flash layout                                                                */
/* -------------------------------------------------------------------------- */

#define OTA_SLOT_A_START 0x00000000UL
#define OTA_SLOT_A_SIZE 0x0002A000UL
#define OTA_FLASH_METADATA_START 0x0002A000UL
#define OTA_SLOT_B_START 0x0002E000UL
#define OTA_SLOT_B_SIZE 0x00028000UL
#define OTA_BIM_START 0x00056000UL
#define OTA_FLASH_END 0x00058000UL
#define OTA_FLASH_ERASE_SECTOR_SIZE 0x2000UL

#define OTA_EXT_METADATA_ADDR 0x00000UL
#define OTA_EXT_IMAGE_ADDR 0x10000UL

#define OTA_INTERNAL_IMAGE_START_ADDR 0x00000000UL
#define OTA_INTERNAL_VECTOR_MIN_ADDR 0x000000A8UL
#define OTA_INTERNAL_BIM_ADDR 0x00056000UL

/* -------------------------------------------------------------------------- */
/* Packet structures                                                           */
/* -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
  uint8_t type;
  uint32_t offset;
  uint16_t length;
  uint8_t target_slot;
  uint8_t data[64];
} fw_packet_t;

#define FW_PACKET_HEADER_LEN offsetof(fw_packet_t, data)

typedef struct __attribute__((packed)) {
  uint8_t type; /* PKT_TYPE_BITMAP_REPORT */
  uint8_t status;
  uint32_t page_offset;
  uint8_t bitmap[8];
} bitmap_report_t;

typedef struct __attribute__((packed)) {
  uint8_t type; /* PKT_TYPE_START_REPORT */
  uint8_t status;
  uint16_t image_sec_ver;
  uint16_t running_sec_ver;
  uint8_t target_slot;
} start_report_t;

typedef struct __attribute__((packed)) {
  uint8_t type; /* PKT_TYPE_STAGE_REPORT */
  uint8_t status;
  uint8_t target_slot;
  uint16_t image_sec_ver;
  uint16_t running_sec_ver;
  uint8_t is_leaf;
} stage_report_t;

/* -------------------------------------------------------------------------- */
/* Shared transfer state                                                       */
/* -------------------------------------------------------------------------- */

extern uint32_t current_file_size;
extern uint8_t page_buffer[PAGE_SIZE];
extern uint32_t page_start_offset;
extern uint16_t page_bytes_received;
extern uint16_t expected_page_size;
extern uint8_t distribution_in_progress;
extern uint8_t ota_target_slot;
extern struct simple_udp_connection udp_conn;

#define MAX_NODES 16
extern uip_ipaddr_t session_nodes[MAX_NODES];
extern uint8_t session_nodes_count;
extern uint8_t node_replied[MAX_NODES];
extern uint8_t session_bitmaps[MAX_NODES][8];
extern uint8_t retry_count;

uint8_t hex2val(char c);
void send_to_all(const fw_packet_t *pkt, uint16_t len);

PROCESS_NAME(distribute_process);
extern process_event_t event_bitmap_received;

/* -------------------------------------------------------------------------- */
/* Coordinator API                                                             */
/* -------------------------------------------------------------------------- */

int ota_coordinator_has_routes(void);
int find_session_node_by_addr(const uip_ipaddr_t *addr);
void remove_session_node(int index);
void ota_notify_uart_no_targets(void);
void ota_reset_transfer_state(uint32_t file_size);
void ota_populate_session_nodes_from_routes(void);
uint8_t ota_parse_target(const char *target);
const char *ota_target_name(uint8_t target);
uint8_t ota_page_is_complete(uint16_t total_chunks);
uint8_t ota_start_admission_is_in_progress(void);
void ota_start_admission_cancel(void);
void ota_start_admission_begin(void);
void ota_coordinator_handle_uart(const char *str);
void ota_coordinator_handle_start_report(const start_report_t *report,
                                         const uip_ipaddr_t *sender_addr);

/* -------------------------------------------------------------------------- */
/* Sensor-node API                                                             */
/* -------------------------------------------------------------------------- */

void ota_sensor_boot_check(void);
void ota_sensor_confirm_stable(void);

void udp_rx_callback(struct simple_udp_connection *c,
                     const uip_ipaddr_t *sender_addr,
                     uint16_t sender_port,
                     const uip_ipaddr_t *receiver_addr,
                     uint16_t receiver_port, const uint8_t *data,
                     uint16_t datalen);

#endif /* OTA_H_ */
