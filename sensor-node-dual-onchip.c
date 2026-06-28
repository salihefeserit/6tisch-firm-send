#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "lib/random.h"
#include "net/ipv6/uip-ds6-nbr.h"
#include "net/ipv6/uip-ds6-route.h"
#include "ota.h"
#include "sys/log.h"

#if OTA_WITH_BIM_DUAL_ONCHIP
#include "dev/leds.h"
#include "oad_image_header_app.h"
#include "ota-flash.h"
#include "ota-metadata.h"
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#endif

/* Coordinator IP variables are no longer needed globally as they are derived
 * from RPL root IP */

static uint32_t current_rx_page_offset = 0xFFFFFFFF;
static uint8_t page_bitmap[8];
static uint8_t page_0_erased = 0;
static uint8_t ota_session_valid = 0;
static uint8_t ota_rejected_same_slot = 0;

#if OTA_WITH_BIM_DUAL_ONCHIP
#define STABILITY_CHECK_INTERVAL (CLOCK_SECOND * 30)
#define MAX_BOOT_ATTEMPTS 3

static ota_boot_metadata_t current_metadata;
static uint8_t current_slot = OTA_SLOT_INVALID;
static struct ctimer ota_reset_timer;

static char slot_name(uint8_t slot) {
  return slot == OTA_SLOT_A ? 'A' : (slot == OTA_SLOT_B ? 'B' : '?');
}

static uint32_t image_softver_to_u32(const uint8_t soft_ver[4]) {
  uint32_t version;
  memcpy(&version, soft_ver, sizeof(version));
  return version;
}

static void ota_reset_callback(void *ptr) {
  (void)ptr;
  LOG_INFO("[OTA] Resetting to boot staged image\n");
  SysCtrlSystemReset();
}

static int validate_downloaded_image(uint8_t target_slot, imgHdr_t *header,
                                     uint32_t *image_version,
                                     uint32_t *image_size,
                                     uint32_t *image_crc32) {
  uint32_t slot_start = ota_flash_slot_start(target_slot);
  uint32_t slot_end = slot_start + ota_flash_slot_size(target_slot);
  uint8_t current_sec_ver = ota_image_header.secInfoSeg.secVer;
  uint8_t candidate_sec_ver;

  if (!oad_image_header_read(slot_start, header)) {
    LOG_WARN("[OTA] Target slot %c does not contain a valid OAD header\n",
             slot_name(target_slot));
    return 0;
  }

  candidate_sec_ver = header->secInfoSeg.secVer;
  if (candidate_sec_ver <= current_sec_ver) {
    LOG_WARN("[OTA] Rejecting slot %c image: secVer %u <= running secVer %u\n",
             slot_name(target_slot), candidate_sec_ver, current_sec_ver);
    return 0;
  }

  if (header->fixedHdr.imgType != OAD_IMG_TYPE_APPSTACKLIB ||
      header->fixedHdr.techType != OAD_WIRELESS_TECH_TIMAC_2_4G) {
    LOG_WARN("[OTA] Rejecting slot %c image: imgType %u techType 0x%08lx\n",
             slot_name(target_slot), header->fixedHdr.imgType,
             (unsigned long)header->fixedHdr.techType);
    return 0;
  }

  if (header->imgPayload.startAddr != slot_start ||
      header->fixedHdr.prgEntry < slot_start ||
      header->fixedHdr.prgEntry >= slot_end ||
      header->fixedHdr.imgEndAddr < slot_start ||
      header->fixedHdr.imgEndAddr >= slot_end ||
      header->fixedHdr.imgEndAddr < header->imgPayload.startAddr) {
    LOG_WARN("[OTA] Rejecting slot %c image: invalid address range "
             "start=0x%08lx entry=0x%08lx end=0x%08lx\n",
             slot_name(target_slot),
             (unsigned long)header->imgPayload.startAddr,
             (unsigned long)header->fixedHdr.prgEntry,
             (unsigned long)header->fixedHdr.imgEndAddr);
    return 0;
  }

  *image_size = header->fixedHdr.imgEndAddr - slot_start + 1;
  if (*image_size == 0 || *image_size > current_file_size ||
      *image_size > ota_flash_slot_size(target_slot)) {
    LOG_WARN("[OTA] Rejecting slot %c image: invalid image size %lu "
             "(file %lu, slot %lu)\n",
             slot_name(target_slot), (unsigned long)*image_size,
             (unsigned long)current_file_size,
             (unsigned long)ota_flash_slot_size(target_slot));
    return 0;
  }

  *image_version = image_softver_to_u32(header->fixedHdr.softVer);
  *image_crc32 = header->fixedHdr.crc32;
  LOG_INFO("[OTA] Candidate slot %c validated: secVer %u > %u, "
           "size %lu, crc32 0x%08lx\n",
           slot_name(target_slot), candidate_sec_ver, current_sec_ver,
           (unsigned long)*image_size, (unsigned long)*image_crc32);
  return 1;
}

static int stage_downloaded_image(uint8_t target_slot) {
  imgHdr_t target_header;
  ota_boot_metadata_t metadata;
  uint32_t image_version;
  uint32_t image_size;
  uint32_t image_crc32;

  if (!validate_downloaded_image(target_slot, &target_header, &image_version,
                                 &image_size, &image_crc32)) {
    return 0;
  }

  if (!ota_flash_read_metadata(&metadata)) {
    uint32_t running_version =
        image_softver_to_u32(ota_image_header.fixedHdr.softVer);
    uint32_t running_start = (uint32_t)&ota_image_header;
    uint32_t running_size =
        ota_image_header.fixedHdr.imgEndAddr - running_start + 1;
    LOG_WARN("[OTA] Metadata missing/invalid. Reinitializing before staging.\n");
    ota_metadata_init_factory(&metadata, current_slot, running_version,
                              running_size, ota_image_header.fixedHdr.crc32);
  }

  if (!ota_metadata_mark_verified(&metadata, target_slot, image_version,
                                  image_size, image_crc32) ||
      !ota_metadata_stage_verified_image(&metadata, target_slot)) {
    LOG_WARN("[OTA] Failed to stage slot %c in metadata\n",
             slot_name(target_slot));
    return 0;
  }

  if (!ota_flash_write_metadata(&metadata)) {
    LOG_WARN("[OTA] Failed to persist metadata for slot %c\n",
             slot_name(target_slot));
    return 0;
  }

  current_metadata = metadata;
  LOG_INFO("[OTA] Slot %c staged. Reset scheduled in 5 seconds.\n",
           slot_name(target_slot));
  ctimer_set(&ota_reset_timer, 5 * CLOCK_SECOND, ota_reset_callback, NULL);
  return 1;
}

static int start_ota_session(uint8_t target_slot, uint32_t file_size) {
  ota_session_valid = 0;
  ota_rejected_same_slot = 0;
  ota_target_slot = target_slot;
  page_0_erased = 0;
  ota_flash_reset_erase_tracking();

  if (target_slot > OTA_SLOT_B) {
    LOG_WARN("[OTA] Invalid target slot %u\n", target_slot);
    return 0;
  }

  if (file_size == 0 || file_size > ota_flash_slot_size(target_slot)) {
    LOG_WARN("[OTA] Image size %lu does not fit slot %c (max %lu)\n",
             (unsigned long)file_size, slot_name(target_slot),
             (unsigned long)ota_flash_slot_size(target_slot));
    return 0;
  }

  current_slot = ota_flash_current_slot();
  if (target_slot == current_slot) {
    LOG_WARN("[OTA] Rejecting image for current running slot %c\n",
             slot_name(target_slot));
    ota_rejected_same_slot = 1;
    return 0;
  }

  LOG_INFO("[OTA] Current slot %c, target slot %c, size %lu\n",
           slot_name(current_slot), slot_name(target_slot),
           (unsigned long)file_size);

  if (!ota_flash_erase_target_sector(target_slot, 0)) {
    LOG_WARN("[OTA] Failed to erase initial sector in slot %c\n",
             slot_name(target_slot));
    return 0;
  }

  page_0_erased = 1;
  ota_session_valid = 1;
  return 1;
}

void ota_sensor_boot_check(void) {
  uint32_t header_physical_address;
  uint32_t current_version;
  uint32_t physical_crc;
  uint32_t current_size;
  uint32_t saved_crc;
  uint32_t reset_source;
  uint32_t shadow_addr;
  imgHdr_t shadow_header;

  current_slot = ota_flash_current_slot();
  header_physical_address = (uint32_t)&ota_image_header;
  physical_crc = ota_image_header.fixedHdr.crc32;
  memcpy(&current_version, ota_image_header.fixedHdr.softVer,
         sizeof(current_version));
  current_size = ota_image_header.fixedHdr.imgEndAddr -
                 header_physical_address + 1;

  LOG_INFO("[BIM] Running slot %c at 0x%08lx, SecVer %u\n",
           slot_name(current_slot), (unsigned long)header_physical_address,
           ota_image_header.secInfoSeg.secVer);

  if (!ota_flash_read_metadata(&current_metadata)) {
    LOG_INFO("[BIM] Metadata not found. Initializing factory metadata.\n");
    ota_metadata_init_factory(&current_metadata, current_slot, current_version,
                              current_size, physical_crc);
    ota_flash_write_metadata(&current_metadata);
    return;
  }

  saved_crc =
      current_slot == OTA_SLOT_A ? current_metadata.crc_a : current_metadata.crc_b;
  if ((current_slot != current_metadata.active_slot &&
       current_slot != current_metadata.candidate_slot) ||
      physical_crc != saved_crc) {
    LOG_INFO("[BIM] New image detected in slot %c\n", slot_name(current_slot));
    ota_metadata_set_candidate(&current_metadata, current_slot,
                               current_version, current_size, physical_crc);
  } else {
    reset_source = SysCtrlResetSourceGet();
    if (reset_source != RSTSRC_PWR_ON && reset_source != RSTSRC_PIN_RESET &&
        reset_source != RSTSRC_WAKEUP_FROM_SHUTDOWN) {
      ota_metadata_increment_boot_attempts(&current_metadata);
      LOG_WARN("[BIM] Abnormal reset source %lu, boot attempts %lu/%u\n",
               (unsigned long)reset_source,
               (unsigned long)current_metadata.boot_attempts,
               MAX_BOOT_ATTEMPTS);
    }
  }

  shadow_addr = current_slot == OTA_SLOT_A ? OTA_SLOT_B_START : OTA_SLOT_A_START;
  if (oad_image_header_read(shadow_addr, &shadow_header)) {
    uint32_t shadow_phys_crc = shadow_header.fixedHdr.crc32;
    uint32_t shadow_saved_crc =
        current_slot == OTA_SLOT_A ? current_metadata.crc_b : current_metadata.crc_a;
    if (shadow_phys_crc != shadow_saved_crc) {
      uint8_t fallback_slot =
          current_slot == OTA_SLOT_A ? OTA_SLOT_B : OTA_SLOT_A;
      ota_metadata_clear_slot(&current_metadata, fallback_slot);
    }
  }

  ota_flash_write_metadata(&current_metadata);

  if (current_metadata.boot_attempts > MAX_BOOT_ATTEMPTS) {
    uint8_t fallback_slot = current_slot == OTA_SLOT_A ? OTA_SLOT_B : OTA_SLOT_A;
    uint32_t fallback_addr = ota_flash_slot_start(fallback_slot);
    const uint8_t oad_id[8] = {'C', 'C', '1', '3', 'x', '2', 'R', '1'};
    bool fallback_invalid =
        fallback_slot == OTA_SLOT_A
            ? current_metadata.state_a == OTA_IMAGE_STATE_INVALID
            : current_metadata.state_b == OTA_IMAGE_STATE_INVALID;

    LOG_WARN("[BIM] Boot attempt limit exceeded in slot %c\n",
             slot_name(current_slot));

    if (fallback_invalid || memcmp((const void *)fallback_addr, oad_id, 8) != 0) {
      LOG_ERR("[BIM] No valid fallback image. Staying in safe mode.\n");
      leds_on(LEDS_RED);
      while (1) {
      }
    }

    ota_metadata_mark_slot_invalid(&current_metadata, current_slot);
    ota_metadata_confirm_running_image(&current_metadata, fallback_slot);
    ota_flash_write_metadata(&current_metadata);

    if (!ota_flash_invalidate_image_header(header_physical_address)) {
      LOG_ERR("[BIM] Failed to invalidate crashing image header.\n");
      leds_on(LEDS_RED);
      while (1) {
      }
    }

    SysCtrlSystemReset();
  }
}

void ota_sensor_confirm_stable(void) {
  if (current_slot <= OTA_SLOT_B && ota_metadata_crc_is_valid(&current_metadata) &&
      current_metadata.boot_attempts > 0) {
    LOG_INFO("[BIM] Stability window passed. Confirming slot %c\n",
             slot_name(current_slot));
    ota_metadata_confirm_running_image(&current_metadata, current_slot);
    ota_flash_write_metadata(&current_metadata);
  }
}
#else
static char slot_name(uint8_t slot) {
  (void)slot;
  return '?';
}

static int start_ota_session(uint8_t target_slot, uint32_t file_size) {
  (void)target_slot;
  (void)file_size;
  LOG_WARN("[OTA] BIM dual-onchip support is not enabled in this build\n");
  return 0;
}

static int ota_flash_target_bounds_ok(uint8_t slot, uint32_t offset,
                                      uint32_t length) {
  (void)slot;
  (void)offset;
  (void)length;
  return 0;
}

static int ota_flash_erase_target_sector(uint8_t slot, uint32_t sector_offset) {
  (void)slot;
  (void)sector_offset;
  return 0;
}

static int ota_flash_write_target(uint8_t slot, uint32_t offset,
                                  const uint8_t *data, uint16_t length) {
  (void)slot;
  (void)offset;
  (void)data;
  (void)length;
  return 0;
}

static int ota_flash_read_target(uint8_t slot, uint32_t offset,
                                 uint16_t length, uint8_t *buf) {
  (void)slot;
  (void)offset;
  (void)length;
  (void)buf;
  return 0;
}

void ota_sensor_boot_check(void) {
}

void ota_sensor_confirm_stable(void) {
}
#endif

static uint16_t running_fw_crc = 0;

static struct ctimer report_ctimer;
static bitmap_report_t report_to_send;

static int get_coordinator_ll_ip(uip_ipaddr_t *ip) {
  uip_ipaddr_t root_ip;
  if (NETSTACK_ROUTING.get_root_ipaddr != NULL &&
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

static void do_send_bitmap(void) {
  if (node_id != 1) {
    uip_ipaddr_t root_ip;
    if (NETSTACK_ROUTING.get_root_ipaddr != NULL &&
        NETSTACK_ROUTING.get_root_ipaddr(&root_ip) &&
        !uip_is_addr_unspecified(&root_ip)) {

      uip_ipaddr_t coordinator_ll_ip;
      uip_ipaddr_copy(&coordinator_ll_ip, &root_ip);
      memset(&coordinator_ll_ip.u8[0], 0, 8);
      coordinator_ll_ip.u8[0] = 0xfe;
      coordinator_ll_ip.u8[1] = 0x80;

      const uip_ipaddr_t *parent_ip = uip_ds6_defrt_choose();

      /* We can only send directly via link-local if the coordinator is our
       * preferred parent. Under Orchestra unicast_per_neighbor_rpl_storing, we
       * only have dedicated unicast slots for our preferred parent and routing
       * table entries (descendants). If we are 2+ hops away (our parent is
       * another node), sending directly to the coordinator's link-local IP will
       * get permanently stuck in the TSCH queue and cause buffer pool
       * starvation. */
      if (parent_ip != NULL && uip_ipaddr_cmp(parent_ip, &coordinator_ll_ip)) {
        simple_udp_sendto(&udp_conn, &report_to_send, sizeof(report_to_send),
                          &coordinator_ll_ip);
      } else {
        /* If the coordinator is multi-hop, send via the global IP of the root
         * to route it through RPL */
        simple_udp_sendto(&udp_conn, &report_to_send, sizeof(report_to_send),
                          &root_ip);
      }
    }
  }
}

static void send_report_callback(void *ptr) {
  LOG_INFO("[OTA] Sending bitmap report\n");
  do_send_bitmap();
}

static void schedule_bitmap_send(void) {
  clock_time_t delay = (random_rand() % 500) * CLOCK_SECOND / 1000;
  LOG_INFO("[OTA] Scheduling bitmap send in %lu ms\n",
           (unsigned long)(delay * 1000 / CLOCK_SECOND));
  ctimer_set(&report_ctimer, delay, send_report_callback, NULL);
}

static void send_same_slot_reject_report(void) {
  if (node_id != 1) {
    memset(&report_to_send, 0, sizeof(report_to_send));
    report_to_send.type = PKT_TYPE_BITMAP_REPORT;
    report_to_send.status = OTA_REPORT_STATUS_REJECTED_SAME_SLOT;
    report_to_send.page_offset = 0xFFFFFFFFUL;
    LOG_WARN("[OTA] Reporting same-slot OTA rejection to coordinator\n");
    schedule_bitmap_send();
  }
}

static void print_page_bitmap(void) {
  if (current_rx_page_offset != 0xFFFFFFFF) {
    LOG_INFO("[OTA] Page 0x%05lx Rx complete. Bitmap: ",
             (unsigned long)current_rx_page_offset);
    for (int i = 0; i < sizeof(page_bitmap); i++) {
      printf("%02x", page_bitmap[i]);
    }
    printf("\n");

    /* Send report to coordinator if we are a sensor node */
    if (node_id != 1) {
      report_to_send.type = PKT_TYPE_BITMAP_REPORT;
      report_to_send.status = OTA_REPORT_STATUS_OK;
      report_to_send.page_offset = current_rx_page_offset;
      memcpy(report_to_send.bitmap, page_bitmap, sizeof(page_bitmap));
      schedule_bitmap_send();
    }
  }
}



/* UDP receive callback for Sensor Node */
void udp_rx_callback(struct simple_udp_connection *c,
                     const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                     const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                     const uint8_t *data, uint16_t datalen) {
  if (node_id == 1) {
    if (datalen >= sizeof(bitmap_report_t)) {
      const bitmap_report_t *report = (const bitmap_report_t *)data;
      if (report->type == PKT_TYPE_BITMAP_REPORT) {
        LOG_INFO("OTA report from ");
        LOG_INFO_6ADDR(sender_addr);
        LOG_INFO_(" status %u page 0x%05lx: ", report->status,
                  (unsigned long)report->page_offset);
        for (int i = 0; i < 8; i++) {
          printf("%02x", report->bitmap[i]);
        }
        printf("\n");

        if (report->status == OTA_REPORT_STATUS_REJECTED_SAME_SLOT) {
          int found = find_session_node_by_addr(sender_addr);
          if (found != -1) {
            LOG_WARN("Node rejected OTA for its running slot. Removing from "
                     "active target list: ");
            LOG_WARN_6ADDR(sender_addr);
            LOG_WARN_("\n");
            remove_session_node(found);
            process_post(&distribute_process, event_bitmap_received, NULL);
          } else {
            LOG_WARN("Same-slot rejection from unexpected/dropped node: ");
            LOG_WARN_6ADDR(sender_addr);
            LOG_WARN_("\n");
          }
          return;
        }

        if (report->page_offset == page_start_offset) {
          int found = find_session_node_by_addr(sender_addr);
          if (found != -1) {
            if (node_replied[found]) {
              LOG_INFO("Duplicate bitmap report from ");
              LOG_INFO_6ADDR(sender_addr);
              LOG_INFO_(" ignored.\n");
            } else {
              node_replied[found] = 1;
              memcpy(session_bitmaps[found], report->bitmap, 8);
              LOG_INFO("Marked node ");
              LOG_INFO_6ADDR(sender_addr);
              LOG_INFO_(" as replied.\n");

              /* Check if all session nodes have replied */
              uint8_t all_replied = 1;
              for (int i = 0; i < session_nodes_count; i++) {
                if (!node_replied[i]) {
                  all_replied = 0;
                  break;
                }
              }
              if (all_replied) {
                LOG_INFO("All expected nodes replied. Notifying distribution "
                         "process.\n");
                process_post(&distribute_process, event_bitmap_received, NULL);
              } else {
                LOG_INFO("Still waiting for other nodes to reply.\n");
              }
            }
          } else {
            LOG_WARN("Received bitmap report from unexpected/dropped node: ");
            LOG_WARN_6ADDR(sender_addr);
            LOG_WARN_("\n");
          }
        }
      }
    }
    return;
  }

  if (datalen > 0) {
    LOG_INFO("[DEBUG] Rx UDP: len %u, type %u\n", datalen, data[0]);
  }

  if (datalen < FW_PACKET_HEADER_LEN) {
    return; /* Too small to be fw_packet_t header */
  }

  const fw_packet_t *pkt = (const fw_packet_t *)data;

  uip_ipaddr_t coordinator_ll_ip;
  int has_coord_ll = get_coordinator_ll_ip(&coordinator_ll_ip);

  /* Determine if this packet should be forwarded based on RPL parent.
   * A node only forwards downward traffic originating from the root
   * (coordinator) or its preferred parent, which naturally prevents routing
   * loops and handles retransmissions. Additionally, leaf nodes (nodes with no
   * downstream routes) do not need to forward. */
  const uip_ipaddr_t *parent_ip = uip_ds6_defrt_choose();
  uint8_t is_from_parent =
      (parent_ip != NULL && uip_ipaddr_cmp(sender_addr, parent_ip));
  uint8_t is_from_coordinator =
      (has_coord_ll && uip_ipaddr_cmp(sender_addr, &coordinator_ll_ip));

  /* Determine if we are a leaf node by checking the routing table.
   * Under RPL storing mode, a node is a leaf if it has no downstream routes. */
  uint8_t is_leaf = (uip_ds6_route_head() == NULL);

  uint8_t should_forward = (is_from_parent || is_from_coordinator) && !is_leaf;

  if (pkt->type == PKT_TYPE_START) {
    LOG_INFO("[OTA] START received. File size: %lu, target slot: %c\n",
             (unsigned long)pkt->offset, slot_name(pkt->target_slot));

    if (should_forward) {
      send_to_all(pkt, datalen);
    }

    current_file_size = pkt->offset;
    running_fw_crc = 0;

    /* Reset bitmap variables */
    current_rx_page_offset = 0xFFFFFFFF;
    memset(page_bitmap, 0, sizeof(page_bitmap));
    page_0_erased = 0;

    if (!start_ota_session(pkt->target_slot, current_file_size)) {
      LOG_WARN("[OTA] START rejected locally\n");
      if (ota_rejected_same_slot) {
        send_same_slot_reject_report();
      }
      return;
    }

  } else if (pkt->type == PKT_TYPE_DATA) {
    LOG_INFO("[OTA] DATA received: offset 0x%05lx, len %u\n",
             (unsigned long)pkt->offset, pkt->length);

    if (should_forward) {
      send_to_all(pkt, datalen);
    }

    if (!ota_session_valid || pkt->target_slot != ota_target_slot ||
        !ota_flash_target_bounds_ok(ota_target_slot, pkt->offset, pkt->length)) {
      LOG_WARN("[OTA] Ignored DATA outside active target slot/session\n");
      return;
    }

    uint32_t page_offset = pkt->offset - (pkt->offset % PAGE_SIZE);
    uint32_t sector_offset =
        pkt->offset - (pkt->offset % OTA_FLASH_ERASE_SECTOR_SIZE);

    if (current_rx_page_offset == 0xFFFFFFFF) {
      current_rx_page_offset = page_offset;
      memset(page_bitmap, 0, sizeof(page_bitmap));

      if (page_offset == 0) {
        if (!page_0_erased) {
          page_0_erased = ota_flash_erase_target_sector(ota_target_slot, 0);
          if (!page_0_erased) {
            LOG_WARN("[OTA] Failed to erase initial sector\n");
            return;
          }
        }
      } else {
        if (!ota_flash_erase_target_sector(ota_target_slot, sector_offset)) {
          LOG_WARN("[OTA] Failed to erase sector 0x%05lx\n",
                   (unsigned long)sector_offset);
          return;
        }
      }
    } else if (page_offset > current_rx_page_offset) {
      /* Transition to the next page */
      current_rx_page_offset = page_offset;
      memset(page_bitmap, 0, sizeof(page_bitmap));

      if (!ota_flash_erase_target_sector(ota_target_slot, sector_offset)) {
        LOG_WARN("[OTA] Failed to erase sector 0x%05lx\n",
                 (unsigned long)sector_offset);
        return;
      }
    } else if (page_offset < current_rx_page_offset) {
      /* Ignore data for an older page */
      LOG_WARN("[OTA] Ignored DATA for older page: offset 0x%05lx (current "
               "page: 0x%05lx)\n",
               (unsigned long)page_offset,
               (unsigned long)current_rx_page_offset);
      return;
    }

    /* Check if this chunk is already received to avoid duplicate write to flash
     */
    uint8_t chunk_already_received = 0;
    uint16_t chunk_idx = (pkt->offset % PAGE_SIZE) / 64;
    if (chunk_idx < 64) {
      if ((page_bitmap[chunk_idx / 8] & (1 << (chunk_idx % 8))) != 0) {
        chunk_already_received = 1;
      }
    }

    /* Process the data packet only if it is a new chunk */
    if (!chunk_already_received) {
      if (pkt->length > 0) {
        if (chunk_idx < 64) {
          page_bitmap[chunk_idx / 8] |= (1 << (chunk_idx % 8));
        }
      }

      if (!ota_flash_write_target(ota_target_slot, pkt->offset, pkt->data,
                                  pkt->length)) {
        LOG_WARN("[OTA] Flash write failed at 0x%05lx\n",
                 (unsigned long)pkt->offset);
        if (chunk_idx < 64) {
          page_bitmap[chunk_idx / 8] &= ~(1 << (chunk_idx % 8));
        }
      }
    }
  } else if (pkt->type == PKT_TYPE_PAGE_END) {
    LOG_INFO("[OTA] PAGE_END received for offset 0x%05lx\n",
             (unsigned long)pkt->offset);

    /* Debug print routes */
    uip_ds6_route_t *r;
    LOG_INFO("[DEBUG] Routing table dump:\n");
    int route_cnt = 0;
    for (r = uip_ds6_route_head(); r != NULL; r = uip_ds6_route_next(r)) {
      LOG_INFO("  - Route to: ");
      LOG_INFO_6ADDR(&r->ipaddr);
      LOG_INFO_("\n");
      route_cnt++;
    }
    LOG_INFO("[DEBUG] Total routes: %d\n", route_cnt);

    /* Forward PAGE_END if from parent or coordinator (optimized for line
     * topology) */
    if (should_forward) {
      send_to_all(pkt, datalen);
    }

    if (!ota_session_valid || pkt->target_slot != ota_target_slot) {
      LOG_WARN("[OTA] Ignored PAGE_END outside active target slot/session\n");
      return;
    }

    if (pkt->offset == current_rx_page_offset) {
      /* Proceed directly */
    } else if (current_rx_page_offset == 0xFFFFFFFF && pkt->offset == 0) {
      current_rx_page_offset = 0;
      memset(page_bitmap, 0, sizeof(page_bitmap));
    } else if (pkt->offset > current_rx_page_offset &&
               current_rx_page_offset != 0xFFFFFFFF) {
      current_rx_page_offset = pkt->offset;
      memset(page_bitmap, 0, sizeof(page_bitmap));
    } else {
      return;
    }

    /* Verify CRC-16 of the received page and update cumulative running CRC */
    uint16_t page_len = PAGE_SIZE;
    if (current_rx_page_offset + page_len > current_file_size) {
      page_len = current_file_size - current_rx_page_offset;
    }

    uint16_t page_crc = 0;
    uint16_t temp_fw_crc = running_fw_crc;
    int flash_ok = ota_flash_target_bounds_ok(ota_target_slot,
                                              current_rx_page_offset, page_len);
    uint8_t buf[64];
    if (flash_ok) {
      for (uint32_t i = 0; i < page_len; i += sizeof(buf)) {
        uint16_t chunk =
            (page_len - i > sizeof(buf)) ? sizeof(buf) : (page_len - i);
        if (!ota_flash_read_target(ota_target_slot, current_rx_page_offset + i,
                                   chunk, buf)) {
          flash_ok = 0;
          break;
        }
        page_crc = crc16_data(buf, chunk, page_crc);
        temp_fw_crc = crc16_data(buf, chunk, temp_fw_crc);
      }
    }

    if (flash_ok && page_crc == pkt->length) {
      LOG_INFO("[OTA PAGE SUCCESS] CRC matches for page 0x%05lx (0x%04x)\n",
               (unsigned long)current_rx_page_offset, page_crc);
      running_fw_crc = temp_fw_crc; /* Commit running CRC update */
    } else {
      LOG_WARN("[OTA PAGE FAILED] CRC mismatch/Flash error for page 0x%05lx! "
               "Expected 0x%04x, got 0x%04x\n",
               (unsigned long)current_rx_page_offset, pkt->length, page_crc);
      /* Clear page bitmap to trigger retransmission of all chunks in this page */
      memset(page_bitmap, 0, sizeof(page_bitmap));
    }

    /* Print and send the report */
    print_page_bitmap();
  } else if (pkt->type == PKT_TYPE_VERIFY) {
    LOG_INFO("[OTA] VERIFY request received! Expected CRC: 0x%04x\n",
             (uint16_t)pkt->offset);

    /* Forward VERIFY if from parent or coordinator (optimized for line topology) */
    if (should_forward) {
      send_to_all(pkt, datalen);
    }

    if (!ota_session_valid || pkt->target_slot != ota_target_slot) {
      LOG_WARN("[OTA] Ignored VERIFY outside active target slot/session\n");
      return;
    }

    /* Print final page bitmap (if not already printed by PAGE_END) */
    print_page_bitmap();
    current_rx_page_offset = 0xFFFFFFFF;

    /* Verify final cumulative CRC in memory without blocking the CPU or reading flash */
    if (running_fw_crc == (uint16_t)pkt->offset) {
      LOG_INFO("[OTA VERIFY SUCCESS] Cumulative CRC matches! (0x%04x)\n", running_fw_crc);
#if OTA_WITH_BIM_DUAL_ONCHIP
      if (!stage_downloaded_image(ota_target_slot)) {
        LOG_WARN("[OTA] Downloaded image was not staged for boot\n");
      }
#else
      LOG_INFO("[OTA] BIM dual-onchip disabled; image will not be staged for boot\n");
#endif
      ota_session_valid = 0;
    } else {
      LOG_INFO("[OTA VERIFY FAILED] Cumulative CRC mismatch! Expected 0x%04x, got 0x%04x\n",
               (uint16_t)pkt->offset, running_fw_crc);
    }
  }
}
