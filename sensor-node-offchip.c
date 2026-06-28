#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "lib/random.h"
#include "net/ipv6/uip-ds6-nbr.h"
#include "net/ipv6/uip-ds6-route.h"
#include "ota.h"
#include "sys/log.h"
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#include <ti/common/cc26xx/oad/ext_flash_layout.h>
#include <ti/common/cc26xx/oad/oad_image_header.h>

/* Coordinator IP variables are no longer needed globally as they are derived
 * from RPL root IP */

static uint32_t current_rx_page_offset = 0xFFFFFFFF;
static uint8_t page_bitmap[8];
static uint8_t page_0_erased = 0;
static uint8_t ota_session_accepted = 0;

static uint16_t running_fw_crc = 0;

static struct ctimer report_ctimer;
#if OTA_STAGE_RESET_AFTER_VERIFY
static struct ctimer ota_reset_timer;
#endif
static bitmap_report_t report_to_send;

static const uint8_t expected_img_id[OAD_IMG_ID_LEN] = OAD_IMG_ID_VAL;
static const uint8_t expected_metadata_id[OAD_IMG_ID_LEN] = OAD_EXTFL_ID_VAL;

static uint32_t image_softver_to_u32(const imgFixedHdr_t *fixed_hdr) {
  uint32_t version;
#ifdef BIM_VERIFY_VERSION_IMAGE
  version = fixed_hdr->softVer;
#else
  memcpy(&version, fixed_hdr->softVer, sizeof(version));
#endif
  return version;
}

static int validate_offchip_image(const imgHdr_t *header) {
  if (memcmp(header->fixedHdr.imgID, expected_img_id,
             sizeof(expected_img_id)) != 0) {
    LOG_WARN("[OTA] Rejecting off-chip image: invalid image ID\n");
    return 0;
  }

  if (header->secInfoSeg.secVer <= OTA_RUNNING_SEC_VER) {
    LOG_WARN("[OTA] Rejecting off-chip image: secVer %u <= running secVer %u\n",
             header->secInfoSeg.secVer, OTA_RUNNING_SEC_VER);
    return 0;
  }

  if (header->fixedHdr.techType != OAD_WIRELESS_TECH_TIMAC_2_4G ||
      header->imgPayload.wirelessTech != OAD_WIRELESS_TECH_TIMAC_2_4G) {
    LOG_WARN("[OTA] Rejecting off-chip image: techType 0x%04x payloadTech "
             "0x%04x\n",
             (unsigned int)header->fixedHdr.techType,
             (unsigned int)header->imgPayload.wirelessTech);
    return 0;
  }

  if (header->fixedHdr.imgType != OTA_EXPECTED_IMG_TYPE) {
    LOG_WARN("[OTA] Rejecting off-chip image: imgType %u != expected %u\n",
             header->fixedHdr.imgType, OTA_EXPECTED_IMG_TYPE);
    return 0;
  }

  if (header->fixedHdr.bimVer != BIM_VER ||
      header->fixedHdr.metaVer != META_VER ||
      header->fixedHdr.crcStat == CRC_INVALID) {
    LOG_WARN("[OTA] Rejecting off-chip image: invalid BIM metadata fields "
             "(bim %u meta %u copy 0x%02x crc 0x%02x)\n",
             header->fixedHdr.bimVer, header->fixedHdr.metaVer,
             header->fixedHdr.imgCpStat, header->fixedHdr.crcStat);
    return 0;
  }

  if (header->fixedHdr.hdrLen != sizeof(imgHdr_t)) {
    LOG_WARN("[OTA] Rejecting off-chip image: hdrLen %u != %u\n",
             header->fixedHdr.hdrLen, (unsigned int)sizeof(imgHdr_t));
    return 0;
  }

  if (header->imgPayload.segTypeImg != IMG_PAYLOAD_SEG_ID ||
      header->imgPayload.startAddr != OTA_INTERNAL_IMAGE_START_ADDR) {
    LOG_WARN("[OTA] Rejecting off-chip image: payload segment invalid "
             "(type %u start 0x%08lx)\n",
             header->imgPayload.segTypeImg,
             (unsigned long)header->imgPayload.startAddr);
    return 0;
  }

  if (header->fixedHdr.prgEntry < OTA_INTERNAL_VECTOR_MIN_ADDR ||
      header->fixedHdr.prgEntry > header->fixedHdr.imgEndAddr ||
      header->fixedHdr.imgEndAddr >= OTA_INTERNAL_BIM_ADDR) {
    LOG_WARN("[OTA] Rejecting off-chip image: invalid internal range "
             "entry=0x%08lx end=0x%08lx\n",
             (unsigned long)header->fixedHdr.prgEntry,
             (unsigned long)header->fixedHdr.imgEndAddr);
    return 0;
  }

  if (header->fixedHdr.len == 0 || header->fixedHdr.len > current_file_size) {
    LOG_WARN("[OTA] Rejecting off-chip image: invalid length %lu "
             "(file %lu)\n",
             (unsigned long)header->fixedHdr.len,
             (unsigned long)current_file_size);
    return 0;
  }

  LOG_INFO("[OTA] Candidate off-chip image validated: secVer %u > %u, "
           "size %lu, crc32 0x%08lx\n",
           header->secInfoSeg.secVer, OTA_RUNNING_SEC_VER,
           (unsigned long)header->fixedHdr.len,
           (unsigned long)header->fixedHdr.crc32);
  return 1;
}

static int read_offchip_image_header(imgHdr_t *header) {
  int ok = 0;

  if (!ext_flash_open(NULL)) {
    LOG_WARN("[OTA] Failed to open external flash for OAD header read\n");
    return 0;
  }

  ok = ext_flash_read(NULL, OTA_EXT_IMAGE_ADDR, sizeof(*header),
                      (uint8_t *)header);
  ext_flash_close(NULL);

  if (!ok) {
    LOG_WARN("[OTA] Failed to read OAD header at 0x%05lx\n",
             (unsigned long)OTA_EXT_IMAGE_ADDR);
  }

  return ok;
}

static int write_offchip_bim_metadata(const imgHdr_t *header) {
  ExtImageInfo_t metadata;
  ExtImageInfo_t readback;
  int ok = 0;

  memset(&metadata, 0xff, sizeof(metadata));
  metadata.fixedHdr = header->fixedHdr;
  memcpy(metadata.fixedHdr.imgID, expected_metadata_id,
         sizeof(metadata.fixedHdr.imgID));
  metadata.fixedHdr.imgCpStat = NEED_COPY;
  metadata.fixedHdr.crcStat = CRC_VALID;
  metadata.extFlAddr = OTA_EXT_IMAGE_ADDR;
  metadata.counter = image_softver_to_u32(&header->fixedHdr);

  if (!ext_flash_open(NULL)) {
    LOG_WARN("[OTA] Failed to open external flash for BIM metadata write\n");
    return 0;
  }

  if (!ext_flash_erase(NULL, OTA_EXT_METADATA_ADDR,
                       EXT_FLASH_ERASE_SECTOR_SIZE)) {
    LOG_WARN("[OTA] Failed to erase BIM metadata sector at 0x%05lx\n",
             (unsigned long)OTA_EXT_METADATA_ADDR);
    goto done;
  }

  if (!ext_flash_write(NULL, OTA_EXT_METADATA_ADDR, sizeof(metadata),
                       (const uint8_t *)&metadata)) {
    LOG_WARN("[OTA] Failed to write BIM metadata at 0x%05lx\n",
             (unsigned long)OTA_EXT_METADATA_ADDR);
    goto done;
  }

  if (!ext_flash_read(NULL, OTA_EXT_METADATA_ADDR, sizeof(readback),
                      (uint8_t *)&readback)) {
    LOG_WARN("[OTA] Failed to read back BIM metadata\n");
    goto done;
  }

  if (memcmp(readback.fixedHdr.imgID, expected_metadata_id,
             sizeof(readback.fixedHdr.imgID)) != 0) {
    LOG_WARN("[OTA] BIM metadata readback ID mismatch\n");
    goto done;
  }

  LOG_INFO("[OTA] Off-chip BIM metadata written at 0x%05lx "
           "(payload 0x%05lx, counter %lu)\n",
           (unsigned long)OTA_EXT_METADATA_ADDR,
           (unsigned long)metadata.extFlAddr, (unsigned long)metadata.counter);
  LOG_INFO("[OTA] BIM metadata readback: imgID=OAD NVM1 copy=0x%02x "
           "crc=0x%02x len=%lu extFlAddr=0x%05lx\n",
           readback.fixedHdr.imgCpStat, readback.fixedHdr.crcStat,
           (unsigned long)readback.fixedHdr.len,
           (unsigned long)readback.extFlAddr);
  ok = 1;

done:
  ext_flash_close(NULL);
  return ok;
}

#if OTA_STAGE_RESET_AFTER_VERIFY
static void ota_reset_callback(void *ptr) {
  (void)ptr;
  LOG_INFO("[OTA] Resetting to boot staged image\n");
  SysCtrlSystemReset();
}
#endif

static int stage_offchip_image_for_bim(void) {
  imgHdr_t header;

  if (!read_offchip_image_header(&header)) {
    return 0;
  }

  if (!validate_offchip_image(&header)) {
    return 0;
  }

  if (!write_offchip_bim_metadata(&header)) {
    return 0;
  }

#if OTA_STAGE_RESET_AFTER_VERIFY
  LOG_INFO("[OTA] Reset scheduled in 5 seconds\n");
  ctimer_set(&ota_reset_timer, 5 * CLOCK_SECOND, ota_reset_callback, NULL);
#else
  LOG_INFO("[OTA] Staged image metadata written; reset disabled by "
           "OTA_STAGE_RESET_AFTER_VERIFY=0\n");
#endif

  return 1;
}

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

static void send_start_report(uint8_t status, uint16_t image_sec_ver,
                              uint8_t target_slot) {
  if (node_id == 1) {
    return;
  }

  uip_ipaddr_t root_ip;
  if (NETSTACK_ROUTING.get_root_ipaddr == NULL ||
      !NETSTACK_ROUTING.get_root_ipaddr(&root_ip) ||
      uip_is_addr_unspecified(&root_ip)) {
    return;
  }

  start_report_t report;
  report.type = PKT_TYPE_START_REPORT;
  report.status = status;
  report.image_sec_ver = image_sec_ver;
  report.running_sec_ver = OTA_RUNNING_SEC_VER;
  report.target_slot = target_slot;

  uip_ipaddr_t coordinator_ll_ip;
  uip_ipaddr_copy(&coordinator_ll_ip, &root_ip);
  memset(&coordinator_ll_ip.u8[0], 0, 8);
  coordinator_ll_ip.u8[0] = 0xfe;
  coordinator_ll_ip.u8[1] = 0x80;

  const uip_ipaddr_t *parent_ip = uip_ds6_defrt_choose();
  if (parent_ip != NULL && uip_ipaddr_cmp(parent_ip, &coordinator_ll_ip)) {
    simple_udp_sendto(&udp_conn, &report, sizeof(report), &coordinator_ll_ip);
  } else {
    simple_udp_sendto(&udp_conn, &report, sizeof(report), &root_ip);
  }

  LOG_INFO("[OTA] START report sent: status=%u imageSecVer=%u "
           "runningSecVer=%u\n",
           status, image_sec_ver, OTA_RUNNING_SEC_VER);
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

static void print_page_bitmap(void) {
  if (current_rx_page_offset != 0xFFFFFFFF) {
    LOG_INFO("[OTA] Image page offset 0x%05lx stored at ext-flash 0x%05lx. "
             "Bitmap: ",
             (unsigned long)current_rx_page_offset,
             (unsigned long)(OTA_EXT_IMAGE_ADDR + current_rx_page_offset));
    for (int i = 0; i < sizeof(page_bitmap); i++) {
      printf("%02x", page_bitmap[i]);
    }
    printf("\n");

    /* Send report to coordinator if we are a sensor node */
    if (node_id != 1) {
      report_to_send.type = PKT_TYPE_BITMAP_REPORT;
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
    if (datalen >= sizeof(start_report_t) && data[0] == PKT_TYPE_START_REPORT) {
      const start_report_t *report = (const start_report_t *)data;
      ota_coordinator_handle_start_report(report, sender_addr);
    } else if (datalen >= sizeof(bitmap_report_t)) {
      const bitmap_report_t *report = (const bitmap_report_t *)data;
      if (report->type == PKT_TYPE_BITMAP_REPORT) {
        LOG_INFO("Bitmap report from ");
        LOG_INFO_6ADDR(sender_addr);
        LOG_INFO_(" page 0x%05lx: ", (unsigned long)report->page_offset);
        for (int i = 0; i < 8; i++) {
          printf("%02x", report->bitmap[i]);
        }
        printf("\n");

        if (report->page_offset == page_start_offset) {
          int found = -1;
          for (int i = 0; i < session_nodes_count; i++) {
            if (memcmp(&session_nodes[i].u8[8], &sender_addr->u8[8], 8) == 0) {
              found = i;
              break;
            }
          }
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
    uint16_t image_sec_ver = pkt->length;
    LOG_INFO("[OTA] START received. File size: %lu, imageSecVer: %u\n",
             (unsigned long)pkt->offset, image_sec_ver);
    if (pkt->target_slot != OTA_TARGET_OFFCHIP) {
      LOG_WARN("[OTA] START rejected: invalid off-chip target %u\n",
               pkt->target_slot);
      ota_session_accepted = 0;
      send_start_report(OTA_START_STATUS_REJECTED_TARGET, image_sec_ver,
                        pkt->target_slot);
      return;
    }

    if (image_sec_ver != OTA_START_SEC_VER_UNKNOWN &&
        image_sec_ver <= OTA_RUNNING_SEC_VER) {
      LOG_WARN("[OTA] START rejected: image secVer %u <= running secVer %u\n",
               image_sec_ver, OTA_RUNNING_SEC_VER);
      ota_session_accepted = 0;
      current_rx_page_offset = 0xFFFFFFFF;
      memset(page_bitmap, 0, sizeof(page_bitmap));
      page_0_erased = 0;
      send_start_report(OTA_START_STATUS_REJECTED_VERSION, image_sec_ver,
                        pkt->target_slot);

      if (should_forward) {
        send_to_all(pkt, datalen);
      }
      return;
    }

    ota_session_accepted = 1;
    current_file_size = pkt->offset;
    running_fw_crc = 0;

    /* Reset bitmap variables */
    current_rx_page_offset = 0xFFFFFFFF;
    memset(page_bitmap, 0, sizeof(page_bitmap));
    page_0_erased = 0;

    send_start_report(OTA_START_STATUS_ACCEPTED, image_sec_ver,
                      pkt->target_slot);

    if (ext_flash_open(NULL)) {
      /* Erase the first sector eagerly at START so the flash is ready before
       * distribution begins. The coordinator needs several seconds to buffer
       * the first page from UART, giving the erase time to complete. */
      LOG_INFO("Erasing initial OTA image sector at 0x%05lx...\n",
               (unsigned long)OTA_EXT_IMAGE_ADDR);
      ext_flash_erase(NULL, OTA_EXT_IMAGE_ADDR, EXT_FLASH_ERASE_SECTOR_SIZE);
      page_0_erased = 1;
      ext_flash_close(NULL);
    } else {
      LOG_INFO("[ERROR] Failed to open flash!\n");
    }

    /* Forward START if from parent or coordinator (optimized for line topology)
     */
    if (should_forward) {
      send_to_all(pkt, datalen);
    }


  } else if (pkt->type == PKT_TYPE_DATA) {
    LOG_INFO("[OTA] DATA received: image offset 0x%05lx, ext-flash 0x%05lx, "
             "len %u\n",
             (unsigned long)pkt->offset,
             (unsigned long)(OTA_EXT_IMAGE_ADDR + pkt->offset), pkt->length);

    /* Forward the packet if from parent or coordinator (optimized for line
     * topology). A node that rejected the START may still be a relay for
     * accepted downstream nodes. */
    if (should_forward) {
      send_to_all(pkt, datalen);
    }

    if (!ota_session_accepted) {
      return;
    }

    uint32_t page_offset =
        pkt->offset - (pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE);

    if (current_rx_page_offset == 0xFFFFFFFF) {
      current_rx_page_offset = page_offset;
      memset(page_bitmap, 0, sizeof(page_bitmap));

      if (page_offset == 0) {
        if (!page_0_erased) {
          if (ext_flash_open(NULL)) {
          LOG_WARN("[OTA] Self-healing: Erasing image sector at ext-flash "
                   "0x%05lx...\n",
                   (unsigned long)OTA_EXT_IMAGE_ADDR);
            ext_flash_erase(NULL, OTA_EXT_IMAGE_ADDR,
                            EXT_FLASH_ERASE_SECTOR_SIZE);
            page_0_erased = 1;
            ext_flash_close(NULL);
          } else {
            LOG_INFO("[ERROR] Failed to open flash for self-healing erase!\n");
          }
        }
      } else {
        if (ext_flash_open(NULL)) {
          LOG_INFO("[OTA] Erasing sector for image page offset 0x%05lx "
                   "(ext-flash 0x%05lx)...\n",
                   (unsigned long)page_offset,
                   (unsigned long)(OTA_EXT_IMAGE_ADDR + page_offset));
          ext_flash_erase(NULL, OTA_EXT_IMAGE_ADDR + page_offset,
                          EXT_FLASH_ERASE_SECTOR_SIZE);
          ext_flash_close(NULL);
        } else {
          LOG_INFO("[ERROR] Failed to open flash for page erase!\n");
        }
      }
    } else if (page_offset > current_rx_page_offset) {
      /* Transition to the next page */
      current_rx_page_offset = page_offset;
      memset(page_bitmap, 0, sizeof(page_bitmap));

      if (ext_flash_open(NULL)) {
        LOG_INFO("[OTA] Transitioning to image page offset 0x%05lx "
                 "(ext-flash 0x%05lx): erasing sector...\n",
                 (unsigned long)page_offset,
                 (unsigned long)(OTA_EXT_IMAGE_ADDR + page_offset));
        ext_flash_erase(NULL, OTA_EXT_IMAGE_ADDR + page_offset,
                        EXT_FLASH_ERASE_SECTOR_SIZE);
        ext_flash_close(NULL);
      } else {
        LOG_INFO("[ERROR] Failed to open flash for page transition erase!\n");
      }
    } else if (page_offset < current_rx_page_offset) {
      /* Ignore data for an older page */
      LOG_WARN("[OTA] Ignored DATA for older image page: offset 0x%05lx "
               "(current image page: 0x%05lx)\n",
               (unsigned long)page_offset,
               (unsigned long)current_rx_page_offset);
      return;
    }

    /* Check if this chunk is already received to avoid duplicate write to flash
     */
    uint8_t chunk_already_received = 0;
    uint16_t chunk_idx = (pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE) / 64;
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

      if (ext_flash_open(NULL)) {
        ext_flash_write(NULL, OTA_EXT_IMAGE_ADDR + pkt->offset, pkt->length,
                        pkt->data);
        ext_flash_close(NULL);
      }
    }
  } else if (pkt->type == PKT_TYPE_PAGE_END) {
    LOG_INFO("[OTA] PAGE_END received for image offset 0x%05lx "
             "(ext-flash 0x%05lx)\n",
             (unsigned long)pkt->offset,
             (unsigned long)(OTA_EXT_IMAGE_ADDR + pkt->offset));

    /* Forward PAGE_END if from parent or coordinator (optimized for line
     * topology). */
    if (should_forward) {
      send_to_all(pkt, datalen);
    }

    if (!ota_session_accepted) {
      return;
    }

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
    int flash_ok = 0;
    if (ext_flash_open(NULL)) {
      flash_ok = 1;
      uint8_t buf[64];
      for (uint32_t i = 0; i < page_len; i += sizeof(buf)) {
        uint16_t chunk =
            (page_len - i > sizeof(buf)) ? sizeof(buf) : (page_len - i);
        ext_flash_read(NULL, OTA_EXT_IMAGE_ADDR + current_rx_page_offset + i,
                       chunk, buf);
        page_crc = crc16_data(buf, chunk, page_crc);
        temp_fw_crc = crc16_data(buf, chunk, temp_fw_crc);
      }
      ext_flash_close(NULL);
    }

    if (flash_ok && page_crc == pkt->length) {
      LOG_INFO("[OTA PAGE SUCCESS] CRC matches for image page offset 0x%05lx "
               "(ext-flash 0x%05lx, crc 0x%04x)\n",
               (unsigned long)current_rx_page_offset,
               (unsigned long)(OTA_EXT_IMAGE_ADDR + current_rx_page_offset),
               page_crc);
      running_fw_crc = temp_fw_crc; /* Commit running CRC update */
    } else {
      LOG_WARN("[OTA PAGE FAILED] CRC mismatch/Flash error for image page "
               "offset 0x%05lx (ext-flash 0x%05lx)! Expected 0x%04x, "
               "got 0x%04x\n",
               (unsigned long)current_rx_page_offset,
               (unsigned long)(OTA_EXT_IMAGE_ADDR + current_rx_page_offset),
               pkt->length, page_crc);
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

    if (!ota_session_accepted) {
      return;
    }

    /* Print final page bitmap (if not already printed by PAGE_END) */
    print_page_bitmap();
    current_rx_page_offset = 0xFFFFFFFF;

    /* Verify final cumulative CRC in memory without blocking the CPU or reading flash */
    if (running_fw_crc == (uint16_t)pkt->offset) {
      LOG_INFO("[OTA VERIFY SUCCESS] Cumulative CRC matches! (0x%04x)\n", running_fw_crc);
      if (!stage_offchip_image_for_bim()) {
        LOG_WARN("[OTA] Downloaded image was not staged for boot\n");
      }
    } else {
      LOG_INFO("[OTA VERIFY FAILED] Cumulative CRC mismatch! Expected 0x%04x, got 0x%04x\n",
               (uint16_t)pkt->offset, running_fw_crc);
      LOG_WARN("[OTA] Skipping BIM metadata write because transfer CRC failed\n");
    }
  }
}
