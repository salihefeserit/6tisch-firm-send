#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "sys/log.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-ds6.h"
#include "ota.h"
#include <errno.h>

#define START_ADMISSION_TIMEOUT (CLOCK_SECOND * 8)

uip_ipaddr_t session_nodes[MAX_NODES];
uint8_t session_nodes_count = 0;
uint8_t node_replied[MAX_NODES];
uint8_t session_bitmaps[MAX_NODES][8];
uint8_t retry_count = 0;

process_event_t event_bitmap_received;

static struct ctimer start_admission_timer;
static uint8_t start_admission_in_progress = 0;
static uint8_t session_start_accepted[MAX_NODES];

static void
finish_start_admission(void *ptr)
{
  int write_idx = 0;
  uint8_t accepted_count = 0;
  uint8_t rejected_count = 0;
  uint8_t silent_count = 0;

  (void)ptr;
  start_admission_in_progress = 0;

  for(int i = 0; i < session_nodes_count; i++) {
    if(node_replied[i] && session_start_accepted[i]) {
      if(write_idx != i) {
        uip_ipaddr_copy(&session_nodes[write_idx], &session_nodes[i]);
        memcpy(session_bitmaps[write_idx], session_bitmaps[i],
               sizeof(session_bitmaps[write_idx]));
      }
      write_idx++;
      accepted_count++;
    } else {
      if(node_replied[i]) {
        rejected_count++;
        LOG_INFO("START admission dropped rejected node: ");
      } else {
        silent_count++;
        LOG_INFO("START admission dropped silent node: ");
      }
      LOG_INFO_6ADDR(&session_nodes[i]);
      LOG_INFO_("\n");
    }
  }

  session_nodes_count = write_idx;

  LOG_INFO("START admission complete: accepted=%u rejected=%u silent=%u\n",
           accepted_count, rejected_count, silent_count);

  if(session_nodes_count == 0) {
    ota_reset_transfer_state(0);
    ota_notify_uart_no_targets();
    return;
  }

  LOG_INFO("Session continues with %u active nodes:\n", session_nodes_count);
  for(int i = 0; i < session_nodes_count; i++) {
    LOG_INFO("  - ");
    LOG_INFO_6ADDR(&session_nodes[i]);
    LOG_INFO_("\n");
  }

  printf("FW:ACK\n");
}

void
ota_notify_uart_no_targets(void)
{
  printf("FW:NO_TARGETS\n");
}

int
ota_coordinator_has_routes(void)
{
  return tsch_is_associated && (uip_ds6_route_head() != NULL);
}

void
ota_reset_transfer_state(uint32_t file_size)
{
  current_file_size = file_size;
  page_start_offset = 0;
  page_bytes_received = 0;
  expected_page_size = (current_file_size > PAGE_SIZE)
                         ? PAGE_SIZE
                         : current_file_size;
  distribution_in_progress = 0;
}

void
ota_populate_session_nodes_from_routes(void)
{
  session_nodes_count = 0;
  uip_ds6_route_t *route;
  for(route = uip_ds6_route_head(); route != NULL;
      route = uip_ds6_route_next(route)) {
    if(session_nodes_count < MAX_NODES) {
      uip_ipaddr_copy(&session_nodes[session_nodes_count], &route->ipaddr);
      session_nodes_count++;
    } else {
      LOG_WARN("Too many routing entries! Truncating to MAX_NODES (%d)\n",
               MAX_NODES);
      break;
    }
  }
}

uint8_t
ota_parse_target(const char *target)
{
  char *endptr;
  unsigned long parsed;

  if(target == NULL || target[0] == '\0' ||
     strcmp(target, "offchip") == 0 || strcmp(target, "OFFCHIP") == 0) {
    return OTA_TARGET_OFFCHIP;
  }

  if(strcmp(target, "A") == 0 || strcmp(target, "a") == 0) {
    return OTA_SLOT_A;
  }
  if(strcmp(target, "B") == 0 || strcmp(target, "b") == 0) {
    return OTA_SLOT_B;
  }

  errno = 0;
  parsed = strtoul(target, &endptr, 0);
  if(errno == 0 && endptr != target && *endptr == '\0' &&
     parsed <= OTA_SLOT_B) {
    return (uint8_t)parsed;
  }

  return OTA_TARGET_INVALID;
}

const char *
ota_target_name(uint8_t target)
{
  if(target == OTA_SLOT_A) {
    return "A";
  }
  if(target == OTA_SLOT_B) {
    return "B";
  }
  if(target == OTA_TARGET_OFFCHIP) {
    return "offchip";
  }
  return "invalid";
}

int
find_session_node_by_addr(const uip_ipaddr_t *addr)
{
  for(int i = 0; i < session_nodes_count; i++) {
    if(memcmp(&session_nodes[i].u8[8], &addr->u8[8], 8) == 0) {
      return i;
    }
  }
  return -1;
}

void
remove_session_node(int index)
{
  if(index < 0 || index >= session_nodes_count) {
    return;
  }

  for(int i = index + 1; i < session_nodes_count; i++) {
    uip_ipaddr_copy(&session_nodes[i - 1], &session_nodes[i]);
    node_replied[i - 1] = node_replied[i];
    memcpy(session_bitmaps[i - 1], session_bitmaps[i], 8);
  }
  session_nodes_count--;
  if(session_nodes_count == 0) {
    ota_notify_uart_no_targets();
  }
}

uint8_t
ota_page_is_complete(uint16_t total_chunks)
{
  if(session_nodes_count == 0) {
    return 1;
  }
  for(int i = 0; i < session_nodes_count; i++) {
    for(int chunk_idx = 0; chunk_idx < total_chunks; chunk_idx++) {
      uint8_t byte_idx = chunk_idx / 8;
      uint8_t bit_idx = chunk_idx % 8;
      if((session_bitmaps[i][byte_idx] & (1 << bit_idx)) == 0) {
        return 0;
      }
    }
  }
  return 1;
}

uint8_t
ota_start_admission_is_in_progress(void)
{
  return start_admission_in_progress;
}

void
ota_start_admission_cancel(void)
{
  if(start_admission_in_progress) {
    ctimer_stop(&start_admission_timer);
    start_admission_in_progress = 0;
  }
}

void
ota_start_admission_begin(void)
{
  memset(node_replied, 0, sizeof(node_replied));
  memset(session_start_accepted, 0, sizeof(session_start_accepted));
  start_admission_in_progress = 1;
  ctimer_set(&start_admission_timer, START_ADMISSION_TIMEOUT,
             finish_start_admission, NULL);
}

void
ota_coordinator_handle_start_report(const start_report_t *report,
                                    const uip_ipaddr_t *sender_addr)
{
  if(!start_admission_in_progress) {
    LOG_INFO("Ignoring START report outside admission window from ");
    LOG_INFO_6ADDR(sender_addr);
    LOG_INFO_("\n");
    return;
  }

  int found = find_session_node_by_addr(sender_addr);
  if(found < 0) {
    LOG_WARN("START report from unexpected node: ");
    LOG_WARN_6ADDR(sender_addr);
    LOG_WARN_("\n");
    return;
  }

  if(node_replied[found]) {
    LOG_INFO("Duplicate START report from ");
    LOG_INFO_6ADDR(sender_addr);
    LOG_INFO_(" ignored.\n");
    return;
  }

  node_replied[found] = 1;
  session_start_accepted[found] =
      (report->status == OTA_START_STATUS_ACCEPTED);

  LOG_INFO("START report from ");
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_(": status=%u imageSecVer=%u runningSecVer=%u\n",
            report->status, report->image_sec_ver, report->running_sec_ver);

  uint8_t all_replied = 1;
  for(int i = 0; i < session_nodes_count; i++) {
    if(!node_replied[i]) {
      all_replied = 0;
      break;
    }
  }

  if(all_replied) {
    ctimer_stop(&start_admission_timer);
    finish_start_admission(NULL);
  }
}
