#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "lib/random.h"
#include "net/ipv6/uip-ds6-nbr.h"
#include "net/ipv6/uip-ds6-route.h"
#include "ota-sensor-backend.h"
#include "ota.h"
#include "sys/log.h"

static uint32_t current_rx_page_offset = 0xFFFFFFFFUL;
static uint8_t page_bitmap[8];
static uint8_t page_0_erased = 0;
static uint8_t ota_session_active = 0;
static uint16_t running_fw_crc = 0;
static uint16_t current_image_sec_ver = OTA_START_SEC_VER_UNKNOWN;

static struct ctimer report_ctimer;
static bitmap_report_t report_to_send;
static struct ctimer leaf_stability_ctimer;
static struct ctimer staged_reboot_ctimer;
static uint16_t staged_image_sec_ver;
static uint8_t staged_target = OTA_TARGET_INVALID;

static uint8_t
is_leaf_node(void)
{
  return uip_ds6_route_head() == NULL;
}

static int
get_coordinator_ll_ip(uip_ipaddr_t *ip)
{
  uip_ipaddr_t root_ip;

  if(NETSTACK_ROUTING.get_root_ipaddr != NULL &&
     NETSTACK_ROUTING.get_root_ipaddr(&root_ip) &&
     !uip_is_addr_unspecified(&root_ip)) {
    uip_ipaddr_copy(ip, &root_ip);
    memset(&ip->u8[0], 0, 8);
    ip->u8[0] = 0xfe;
    ip->u8[1] = 0x80;
    return 1;
  }

  return 0;
}

static void
send_to_coordinator(const void *payload, uint16_t len)
{
  uip_ipaddr_t root_ip;
  uip_ipaddr_t coordinator_ll_ip;
  const uip_ipaddr_t *parent_ip;

  if(node_id == 1 || NETSTACK_ROUTING.get_root_ipaddr == NULL ||
     !NETSTACK_ROUTING.get_root_ipaddr(&root_ip) ||
     uip_is_addr_unspecified(&root_ip)) {
    return;
  }

  uip_ipaddr_copy(&coordinator_ll_ip, &root_ip);
  memset(&coordinator_ll_ip.u8[0], 0, 8);
  coordinator_ll_ip.u8[0] = 0xfe;
  coordinator_ll_ip.u8[1] = 0x80;

  parent_ip = uip_ds6_defrt_choose();
  if(parent_ip != NULL && uip_ipaddr_cmp(parent_ip, &coordinator_ll_ip)) {
    simple_udp_sendto(&udp_conn, payload, len, &coordinator_ll_ip);
  } else {
    simple_udp_sendto(&udp_conn, payload, len, &root_ip);
  }
}

static uint8_t
parent_is_coordinator(const uip_ipaddr_t *parent_ip)
{
  uip_ipaddr_t coordinator_ll_ip;

  return parent_ip != NULL &&
         get_coordinator_ll_ip(&coordinator_ll_ip) &&
         uip_ipaddr_cmp(parent_ip, &coordinator_ll_ip);
}

static void
send_rebooting_soon_to_parent(void)
{
  const uip_ipaddr_t *parent_ip = uip_ds6_defrt_choose();
  stage_report_t report;

  if(parent_ip == NULL) {
    LOG_WARN("[OTA] Cannot send REBOOTING_SOON: no parent route\n");
    return;
  }

  if(parent_is_coordinator(parent_ip)) {
    LOG_INFO("[OTA] Parent is coordinator; REBOOTING_SOON report skipped\n");
    return;
  }

  memset(&report, 0, sizeof(report));
  report.type = PKT_TYPE_STAGE_REPORT;
  report.status = OTA_STAGE_STATUS_REBOOTING_SOON;
  report.target_slot = staged_target;
  report.image_sec_ver = staged_image_sec_ver;
  report.running_sec_ver = ota_sensor_backend_running_sec_ver();
  report.is_leaf = is_leaf_node();

  simple_udp_sendto(&udp_conn, &report, sizeof(report), parent_ip);

  LOG_INFO("[OTA] REBOOTING_SOON report sent to parent: ");
  LOG_INFO_6ADDR(parent_ip);
  LOG_INFO_("\n");
}

static void
staged_reboot_callback(void *ptr)
{
  (void)ptr;
  ota_sensor_backend_reset_to_staged_image();
}

static void
leaf_stability_callback(void *ptr)
{
  (void)ptr;

  if(!is_leaf_node()) {
    LOG_INFO("[OTA] Node is no longer leaf; staged image waits for "
             "coordinated reboot\n");
    return;
  }

#if OTA_STAGE_RESET_AFTER_VERIFY
  send_rebooting_soon_to_parent();
  LOG_INFO("[OTA] Leaf remained stable. Reset scheduled in %u seconds\n",
           OTA_STAGE_REBOOT_DELAY_SECONDS);
  ctimer_set(&staged_reboot_ctimer,
             OTA_STAGE_REBOOT_DELAY_SECONDS * CLOCK_SECOND,
             staged_reboot_callback, NULL);
#else
  LOG_INFO("[OTA] Leaf remained stable; reset disabled by "
           "OTA_STAGE_RESET_AFTER_VERIFY=0\n");
#endif
}

static void
schedule_leaf_reboot_if_safe(uint16_t image_sec_ver, uint8_t target)
{
  staged_image_sec_ver = image_sec_ver;
  staged_target = target;

  if(!is_leaf_node()) {
    LOG_INFO("[OTA] Image staged on non-leaf node; waiting for downstream "
             "reboot policy\n");
    return;
  }

  LOG_INFO("[OTA] Image staged on leaf node. Rechecking leaf state in %u "
           "seconds\n",
           OTA_STAGE_LEAF_STABILITY_SECONDS);
  ctimer_set(&leaf_stability_ctimer,
             OTA_STAGE_LEAF_STABILITY_SECONDS * CLOCK_SECOND,
             leaf_stability_callback, NULL);
}

static void
send_start_report(uint8_t status, uint16_t image_sec_ver, uint8_t target)
{
  start_report_t report;

  report.type = PKT_TYPE_START_REPORT;
  report.status = status;
  report.image_sec_ver = image_sec_ver;
  report.running_sec_ver = ota_sensor_backend_running_sec_ver();
  report.target_slot = target;

  send_to_coordinator(&report, sizeof(report));

  LOG_INFO("[OTA] START report sent: status=%u imageSecVer=%u "
           "runningSecVer=%u target=%s\n",
           status, image_sec_ver, report.running_sec_ver,
           ota_sensor_backend_target_name(target));
}

static void
send_report_callback(void *ptr)
{
  (void)ptr;
  LOG_INFO("[OTA] Sending bitmap report\n");
  send_to_coordinator(&report_to_send, sizeof(report_to_send));
}

static void
schedule_bitmap_send(void)
{
  clock_time_t delay = (random_rand() % 500) * CLOCK_SECOND / 1000;
  LOG_INFO("[OTA] Scheduling bitmap send in %lu ms\n",
           (unsigned long)(delay * 1000 / CLOCK_SECOND));
  ctimer_set(&report_ctimer, delay, send_report_callback, NULL);
}

static void
send_page_report(void)
{
  if(current_rx_page_offset == 0xFFFFFFFFUL) {
    return;
  }

  LOG_INFO("[OTA] Page 0x%05lx Rx complete. Bitmap: ",
           (unsigned long)current_rx_page_offset);
  for(int i = 0; i < sizeof(page_bitmap); i++) {
    printf("%02x", page_bitmap[i]);
  }
  printf("\n");

  if(node_id != 1) {
    memset(&report_to_send, 0, sizeof(report_to_send));
    report_to_send.type = PKT_TYPE_BITMAP_REPORT;
    report_to_send.status = OTA_REPORT_STATUS_OK;
    report_to_send.page_offset = current_rx_page_offset;
    memcpy(report_to_send.bitmap, page_bitmap, sizeof(page_bitmap));
    schedule_bitmap_send();
  }
}

static void
handle_stage_report(const stage_report_t *report,
                    const uip_ipaddr_t *sender_addr)
{
  LOG_INFO("[OTA] Stage report from ");
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_(" status=%u imageSecVer=%u runningSecVer=%u target=%s "
            "isLeaf=%u\n",
            report->status, report->image_sec_ver, report->running_sec_ver,
            ota_sensor_backend_target_name(report->target_slot),
            report->is_leaf);
}

static void
handle_coordinator_report(const uint8_t *data, uint16_t datalen,
                          const uip_ipaddr_t *sender_addr)
{
  if(datalen >= sizeof(start_report_t) && data[0] == PKT_TYPE_START_REPORT) {
    ota_coordinator_handle_start_report((const start_report_t *)data,
                                        sender_addr);
    return;
  }

  if(datalen >= sizeof(stage_report_t) && data[0] == PKT_TYPE_STAGE_REPORT) {
    handle_stage_report((const stage_report_t *)data, sender_addr);
    return;
  }

  if(datalen >= sizeof(bitmap_report_t) &&
     data[0] == PKT_TYPE_BITMAP_REPORT) {
    const bitmap_report_t *report = (const bitmap_report_t *)data;

    LOG_INFO("OTA report from ");
    LOG_INFO_6ADDR(sender_addr);
    LOG_INFO_(" status %u page 0x%05lx: ", report->status,
              (unsigned long)report->page_offset);
    for(int i = 0; i < sizeof(report->bitmap); i++) {
      printf("%02x", report->bitmap[i]);
    }
    printf("\n");

    if(report->page_offset == page_start_offset) {
      int found = find_session_node_by_addr(sender_addr);
      if(found < 0) {
        LOG_WARN("Received bitmap report from unexpected/dropped node: ");
        LOG_WARN_6ADDR(sender_addr);
        LOG_WARN_("\n");
        return;
      }

      if(node_replied[found]) {
        LOG_INFO("Duplicate bitmap report from ");
        LOG_INFO_6ADDR(sender_addr);
        LOG_INFO_(" ignored.\n");
        return;
      }

      node_replied[found] = 1;
      memcpy(session_bitmaps[found], report->bitmap,
             sizeof(session_bitmaps[found]));
      LOG_INFO("Marked node ");
      LOG_INFO_6ADDR(sender_addr);
      LOG_INFO_(" as replied.\n");

      uint8_t all_replied = 1;
      for(int i = 0; i < session_nodes_count; i++) {
        if(!node_replied[i]) {
          all_replied = 0;
          break;
        }
      }
      if(all_replied) {
        LOG_INFO("All expected nodes replied. Notifying distribution process.\n");
        process_post(&distribute_process, event_bitmap_received, NULL);
      } else {
        LOG_INFO("Still waiting for other nodes to reply.\n");
      }
    }
  }
}

static uint8_t
packet_should_forward(const uip_ipaddr_t *sender_addr)
{
  uip_ipaddr_t coordinator_ll_ip;
  const uip_ipaddr_t *parent_ip = uip_ds6_defrt_choose();
  uint8_t is_from_parent =
      (parent_ip != NULL && uip_ipaddr_cmp(sender_addr, parent_ip));
  uint8_t is_from_coordinator =
      (get_coordinator_ll_ip(&coordinator_ll_ip) &&
       uip_ipaddr_cmp(sender_addr, &coordinator_ll_ip));
  uint8_t is_leaf = is_leaf_node();

  return (is_from_parent || is_from_coordinator) && !is_leaf;
}

static void
reset_rx_state(void)
{
  current_rx_page_offset = 0xFFFFFFFFUL;
  memset(page_bitmap, 0, sizeof(page_bitmap));
  page_0_erased = 0;
  running_fw_crc = 0;
}

static void
handle_start(const fw_packet_t *pkt, uint16_t datalen, uint8_t should_forward)
{
  uint16_t image_sec_ver = pkt->length;
  uint8_t reject_status = OTA_START_STATUS_REJECTED_TARGET;

  LOG_INFO("[OTA] START received. File size: %lu, imageSecVer: %u, target: %s\n",
           (unsigned long)pkt->offset, image_sec_ver,
           ota_sensor_backend_target_name(pkt->target_slot));

  if(should_forward) {
    send_to_all(pkt, datalen);
  }

  current_file_size = pkt->offset;
  ota_target_slot = pkt->target_slot;
  current_image_sec_ver = image_sec_ver;
  ota_session_active = 0;
  reset_rx_state();

  if(!ota_sensor_backend_start(pkt->target_slot, current_file_size,
                               image_sec_ver, &reject_status)) {
    LOG_WARN("[OTA] START rejected locally: status=%u\n", reject_status);
    send_start_report(reject_status, image_sec_ver, pkt->target_slot);
    return;
  }

  page_0_erased = 1;
  ota_session_active = 1;
  send_start_report(OTA_START_STATUS_ACCEPTED, image_sec_ver,
                    pkt->target_slot);
}

static void
handle_data(const fw_packet_t *pkt, uint16_t datalen, uint8_t should_forward)
{
  uint32_t page_offset;
  uint32_t sector_offset;
  uint32_t erase_sector_size;
  uint16_t chunk_idx;
  uint8_t chunk_already_received = 0;

  LOG_INFO("[OTA] DATA received: offset 0x%05lx, len %u\n",
           (unsigned long)pkt->offset, pkt->length);

  if(should_forward) {
    send_to_all(pkt, datalen);
  }

  if(!ota_session_active || pkt->target_slot != ota_target_slot ||
     !ota_sensor_backend_bounds_ok(ota_target_slot, pkt->offset,
                                   pkt->length)) {
    LOG_WARN("[OTA] Ignored DATA outside active target/session\n");
    return;
  }

  page_offset = pkt->offset - (pkt->offset % PAGE_SIZE);
  erase_sector_size = ota_sensor_backend_erase_sector_size();
  sector_offset = pkt->offset - (pkt->offset % erase_sector_size);

  if(current_rx_page_offset == 0xFFFFFFFFUL) {
    current_rx_page_offset = page_offset;
    memset(page_bitmap, 0, sizeof(page_bitmap));

    if(page_offset == 0) {
      if(!page_0_erased) {
        page_0_erased =
            ota_sensor_backend_erase_sector(ota_target_slot, 0);
        if(!page_0_erased) {
          LOG_WARN("[OTA] Failed to erase initial sector\n");
          return;
        }
      }
    } else if(!ota_sensor_backend_erase_sector(ota_target_slot,
                                                sector_offset)) {
      LOG_WARN("[OTA] Failed to erase sector 0x%05lx\n",
               (unsigned long)sector_offset);
      return;
    }
  } else if(page_offset > current_rx_page_offset) {
    current_rx_page_offset = page_offset;
    memset(page_bitmap, 0, sizeof(page_bitmap));

    if(!ota_sensor_backend_erase_sector(ota_target_slot, sector_offset)) {
      LOG_WARN("[OTA] Failed to erase sector 0x%05lx\n",
               (unsigned long)sector_offset);
      return;
    }
  } else if(page_offset < current_rx_page_offset) {
    LOG_WARN("[OTA] Ignored DATA for older page: offset 0x%05lx "
             "(current page: 0x%05lx)\n",
             (unsigned long)page_offset,
             (unsigned long)current_rx_page_offset);
    return;
  }

  chunk_idx = (pkt->offset % PAGE_SIZE) / 64;
  if(chunk_idx < 64 &&
     (page_bitmap[chunk_idx / 8] & (1 << (chunk_idx % 8))) != 0) {
    chunk_already_received = 1;
  }

  if(!chunk_already_received) {
    if(pkt->length > 0 && chunk_idx < 64) {
      page_bitmap[chunk_idx / 8] |= (1 << (chunk_idx % 8));
    }

    if(!ota_sensor_backend_write(ota_target_slot, pkt->offset, pkt->data,
                                 pkt->length)) {
      LOG_WARN("[OTA] Flash write failed at 0x%05lx\n",
               (unsigned long)pkt->offset);
      if(chunk_idx < 64) {
        page_bitmap[chunk_idx / 8] &= ~(1 << (chunk_idx % 8));
      }
    }
  }
}

static void
handle_page_end(const fw_packet_t *pkt, uint16_t datalen,
                uint8_t should_forward)
{
  uint16_t page_len;
  uint16_t page_crc = 0;
  uint16_t temp_fw_crc = running_fw_crc;
  uint8_t flash_ok;
  uint8_t buf[64];

  LOG_INFO("[OTA] PAGE_END received for offset 0x%05lx\n",
           (unsigned long)pkt->offset);

  if(should_forward) {
    send_to_all(pkt, datalen);
  }

  if(!ota_session_active || pkt->target_slot != ota_target_slot) {
    LOG_WARN("[OTA] Ignored PAGE_END outside active target/session\n");
    return;
  }

  if(pkt->offset == current_rx_page_offset) {
    /* Current page is already selected. */
  } else if(current_rx_page_offset == 0xFFFFFFFFUL && pkt->offset == 0) {
    current_rx_page_offset = 0;
    memset(page_bitmap, 0, sizeof(page_bitmap));
  } else if(pkt->offset > current_rx_page_offset &&
            current_rx_page_offset != 0xFFFFFFFFUL) {
    current_rx_page_offset = pkt->offset;
    memset(page_bitmap, 0, sizeof(page_bitmap));
  } else {
    return;
  }

  page_len = PAGE_SIZE;
  if(current_rx_page_offset + page_len > current_file_size) {
    page_len = current_file_size - current_rx_page_offset;
  }

  flash_ok = ota_sensor_backend_bounds_ok(ota_target_slot,
                                          current_rx_page_offset, page_len);
  if(flash_ok) {
    for(uint32_t i = 0; i < page_len; i += sizeof(buf)) {
      uint16_t chunk =
          (page_len - i > sizeof(buf)) ? sizeof(buf) : (page_len - i);
      if(!ota_sensor_backend_read(ota_target_slot, current_rx_page_offset + i,
                                  chunk, buf)) {
        flash_ok = 0;
        break;
      }
      page_crc = crc16_data(buf, chunk, page_crc);
      temp_fw_crc = crc16_data(buf, chunk, temp_fw_crc);
    }
  }

  if(flash_ok && page_crc == pkt->length) {
    LOG_INFO("[OTA PAGE SUCCESS] CRC matches for page 0x%05lx (0x%04x)\n",
             (unsigned long)current_rx_page_offset, page_crc);
    running_fw_crc = temp_fw_crc;
  } else {
    LOG_WARN("[OTA PAGE FAILED] CRC mismatch/flash error for page 0x%05lx! "
             "Expected 0x%04x, got 0x%04x\n",
             (unsigned long)current_rx_page_offset, pkt->length, page_crc);
    memset(page_bitmap, 0, sizeof(page_bitmap));
  }

  send_page_report();
}

static void
handle_verify(const fw_packet_t *pkt, uint16_t datalen, uint8_t should_forward)
{
  LOG_INFO("[OTA] VERIFY request received! Expected CRC: 0x%04x\n",
           (uint16_t)pkt->offset);

  if(should_forward) {
    send_to_all(pkt, datalen);
  }

  if(!ota_session_active || pkt->target_slot != ota_target_slot) {
    LOG_WARN("[OTA] Ignored VERIFY outside active target/session\n");
    return;
  }

  send_page_report();
  current_rx_page_offset = 0xFFFFFFFFUL;

  if(running_fw_crc == (uint16_t)pkt->offset) {
    LOG_INFO("[OTA VERIFY SUCCESS] Cumulative CRC matches! (0x%04x)\n",
             running_fw_crc);
    if(!ota_sensor_backend_stage(ota_target_slot)) {
      LOG_WARN("[OTA] Downloaded image was not staged for boot\n");
    } else {
      schedule_leaf_reboot_if_safe(current_image_sec_ver, ota_target_slot);
    }
    ota_session_active = 0;
  } else {
    LOG_WARN("[OTA VERIFY FAILED] Cumulative CRC mismatch! Expected 0x%04x, "
             "got 0x%04x\n",
             (uint16_t)pkt->offset, running_fw_crc);
  }
}

void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                const uint8_t *data, uint16_t datalen)
{
  const fw_packet_t *pkt;
  uint8_t should_forward;

  (void)c;
  (void)sender_port;
  (void)receiver_addr;
  (void)receiver_port;

  if(node_id == 1) {
    handle_coordinator_report(data, datalen, sender_addr);
    return;
  }

  if(datalen >= sizeof(stage_report_t) && data[0] == PKT_TYPE_STAGE_REPORT) {
    handle_stage_report((const stage_report_t *)data, sender_addr);
    return;
  }

  if(datalen < FW_PACKET_HEADER_LEN) {
    return;
  }

  pkt = (const fw_packet_t *)data;
  should_forward = packet_should_forward(sender_addr);

  switch(pkt->type) {
  case PKT_TYPE_START:
    handle_start(pkt, datalen, should_forward);
    break;
  case PKT_TYPE_DATA:
    handle_data(pkt, datalen, should_forward);
    break;
  case PKT_TYPE_PAGE_END:
    handle_page_end(pkt, datalen, should_forward);
    break;
  case PKT_TYPE_VERIFY:
    handle_verify(pkt, datalen, should_forward);
    break;
  default:
    break;
  }
}
