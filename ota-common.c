#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "sys/log.h"
#include "ota.h"
#include "net/ipv6/uip-ds6-route.h"

/* Global variables shared between modules */
uint32_t current_file_size = 0;
uint8_t page_buffer[PAGE_SIZE];
uint32_t page_start_offset = 0;
uint16_t page_bytes_received = 0;
uint16_t expected_page_size = 0;
uint8_t distribution_in_progress = 0;
struct simple_udp_connection udp_conn;

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

/* Broadcast an OTA packet to all nodes (optimized for line topology hop-by-hop unicast) */
void send_to_all(const fw_packet_t *pkt, uint16_t len) {
  uip_ds6_route_t *r = uip_ds6_route_head();
  if (r != NULL) {
    const uip_ipaddr_t *nexthop = uip_ds6_route_nexthop(r);
    if (nexthop != NULL) {
      LOG_INFO("[OTA] Unicasting packet (type %u) to next-hop: ", pkt->type);
      LOG_INFO_6ADDR(nexthop);
      LOG_INFO_("\n");
      simple_udp_sendto(&udp_conn, pkt, len, nexthop);
      return;
    }
  }
  uip_ipaddr_t dest_ip;
  uip_create_linklocal_allnodes_mcast(&dest_ip);
  simple_udp_sendto(&udp_conn, pkt, len, &dest_ip);
}
