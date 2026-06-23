#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "sys/log.h"
#include "ota.h"

/* Global variables shared between modules */
uint32_t current_file_size = 0;
uint8_t page_buffer[PAGE_SIZE];
uint32_t page_start_offset = 0;
uint16_t page_bytes_received = 0;
uint16_t expected_page_size = 0;
uint8_t distribution_in_progress = 0;
struct simple_udp_connection udp_conn;
struct etimer ota_timeout_timer;
uint8_t shared_period_is_fast = 0;

/* Fast hex char to byte converter */
uint8_t hex2val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return 0;
}

/* Set Orchestra shared slotframe period dynamically */
void set_shared_period(uint16_t new_size) {
  struct tsch_slotframe *sf = tsch_schedule_slotframe_head();
  while (sf != NULL) {
    if (sf->size.val == 61 || sf->size.val == 11) {
      if (sf->size.val != new_size) {
        LOG_INFO("[OTA] Changing shared slotframe size from %u to %u\n",
                 sf->size.val, new_size);
        TSCH_ASN_DIVISOR_INIT(sf->size, new_size);
      }
      if (new_size == 11) {
        shared_period_is_fast = 1;
        etimer_set(&ota_timeout_timer, OTA_TIMEOUT_DURATION);
      } else {
        shared_period_is_fast = 0;
        etimer_stop(&ota_timeout_timer);
      }
      break;
    }
    sf = tsch_schedule_slotframe_next(sf);
  }
}

/* Reset dynamic period timeout timer */
void reset_ota_timer(void) {
  if (shared_period_is_fast) {
    etimer_set(&ota_timeout_timer, OTA_TIMEOUT_DURATION);
  }
}

/* Broadcast an OTA packet to all nodes */
void send_to_all(fw_packet_t *pkt, uint16_t len) {
  uip_ipaddr_t dest_ip;
  uip_create_linklocal_allnodes_mcast(&dest_ip);
  simple_udp_sendto(&udp_conn, pkt, len, &dest_ip);
}
