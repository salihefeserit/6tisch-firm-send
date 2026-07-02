#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "sys/log.h"
#include "ota.h"
#include "net/ipv6/uip-ds6-nbr.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/mac/tsch/tsch-queue.h"

/* Global variables shared between modules */
uint32_t current_file_size = 0;
uint8_t page_buffer[PAGE_SIZE];
uint32_t page_start_offset = 0;
uint16_t page_bytes_received = 0;
uint16_t expected_page_size = 0;
uint8_t distribution_in_progress = 0;
uint8_t ota_target_slot = OTA_SLOT_INVALID;
struct simple_udp_connection udp_conn;

static const uip_ipaddr_t *
line_next_hop(void)
{
  uip_ds6_route_t *r = uip_ds6_route_head();
  return r != NULL ? uip_ds6_route_nexthop(r) : NULL;
}

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

/*
 * Forward an OTA packet along a line topology.
 *
 * The first routing entry's next-hop is the downstream neighbor in the
 * supported line layout. Intermediate nodes apply the same rule, so packets
 * progress hop-by-hop instead of being flooded.
 */
void send_to_all(const fw_packet_t *pkt, uint16_t len) {
  const uip_ipaddr_t *nexthop = line_next_hop();
  if (nexthop != NULL) {
    LOG_INFO("[OTA] Unicasting packet (type %u) to next-hop: ", pkt->type);
    LOG_INFO_6ADDR(nexthop);
    LOG_INFO_("\n");
    simple_udp_sendto(&udp_conn, pkt, len, nexthop);
    return;
  }
  LOG_WARN("[OTA] No downstream route; packet type %u not sent\n",
           pkt->type);
}

uint8_t
ota_downstream_queue_ready(void)
{
  const uip_ipaddr_t *nexthop = line_next_hop();
  const uip_lladdr_t *lladdr;
  struct tsch_neighbor *neighbor;
  int neighbor_pending = 0;
  int global_pending = tsch_queue_global_packet_count();

  if(nexthop == NULL) {
    return global_pending <= OTA_QUEUE_MAX_GLOBAL_PENDING;
  }

  lladdr = uip_ds6_nbr_lladdr_from_ipaddr(nexthop);
  if(lladdr == NULL) {
    return global_pending <= OTA_QUEUE_MAX_GLOBAL_PENDING;
  }

  neighbor = tsch_queue_get_nbr((const linkaddr_t *)lladdr);
  if(neighbor != NULL) {
    neighbor_pending = tsch_queue_nbr_packet_count(neighbor);
  }

  return neighbor_pending <= OTA_QUEUE_MAX_NEXT_HOP_PENDING &&
         global_pending <= OTA_QUEUE_MAX_GLOBAL_PENDING;
}

void
ota_log_downstream_queue_state(void)
{
  const uip_ipaddr_t *nexthop = line_next_hop();
  const uip_lladdr_t *lladdr;
  struct tsch_neighbor *neighbor = NULL;
  int neighbor_pending = 0;
  int global_pending = tsch_queue_global_packet_count();

  if(nexthop != NULL) {
    lladdr = uip_ds6_nbr_lladdr_from_ipaddr(nexthop);
    if(lladdr != NULL) {
      neighbor = tsch_queue_get_nbr((const linkaddr_t *)lladdr);
    }
  }

  if(neighbor != NULL) {
    neighbor_pending = tsch_queue_nbr_packet_count(neighbor);
  }

  LOG_INFO("[OTA] Waiting for queue drain: next-hop %d/%u, global %d/%u\n",
           neighbor_pending, OTA_QUEUE_MAX_NEXT_HOP_PENDING,
           global_pending, OTA_QUEUE_MAX_GLOBAL_PENDING);
}
