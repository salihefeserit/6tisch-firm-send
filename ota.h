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
#include "ext-flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDP_PORT 8765

/* OTA Packet Definitions */
#define PKT_TYPE_START 0
#define PKT_TYPE_DATA 1
#define PKT_TYPE_VERIFY 2
#define PKT_TYPE_BITMAP_REPORT 3
#define PKT_TYPE_PAGE_END 4

#define PAGE_SIZE 4096

typedef struct __attribute__((packed)) {
  uint8_t type;
  uint32_t offset;
  uint16_t length;
  uint8_t data[64];
} fw_packet_t;

typedef struct __attribute__((packed)) {
  uint8_t type; /* PKT_TYPE_BITMAP_REPORT */
  uint32_t page_offset;
  uint8_t bitmap[8];
} bitmap_report_t;

/* Global variables shared between modules */
extern uint32_t current_file_size;
extern uint8_t page_buffer[PAGE_SIZE];
extern uint32_t page_start_offset;
extern uint16_t page_bytes_received;
extern uint16_t expected_page_size;
extern uint8_t distribution_in_progress;
extern struct simple_udp_connection udp_conn;
extern struct etimer ota_timeout_timer;
extern uint8_t shared_period_is_fast;

#define MAX_NODES 16
extern uip_ipaddr_t session_nodes[MAX_NODES];
extern uint8_t session_nodes_count;
extern uint8_t node_replied[MAX_NODES];
extern uint8_t session_bitmaps[MAX_NODES][8];

#define OTA_TIMEOUT_DURATION (60 * CLOCK_SECOND)

/* Shared helper/control functions */
void set_shared_period(uint16_t new_size);
void reset_ota_timer(void);
uint8_t hex2val(char c);
void send_to_all(fw_packet_t *pkt, uint16_t len);

/* Processes declared globally */
PROCESS_NAME(distribute_process);
extern process_event_t event_bitmap_received;

/* Coordinator-specific forward declarations */
int ota_coordinator_has_routes(void);
void ota_coordinator_handle_uart(const char *str);

/* Sensor-node UDP callback */
void udp_rx_callback(struct simple_udp_connection *c,
                     const uip_ipaddr_t *sender_addr,
                     uint16_t sender_port,
                     const uip_ipaddr_t *receiver_addr,
                     uint16_t receiver_port, const uint8_t *data,
                     uint16_t datalen);

void ota_sensor_node_reset_forwarding(void);

#endif /* OTA_H_ */
