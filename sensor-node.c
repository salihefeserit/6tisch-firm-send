#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "sys/log.h"
#include "lib/random.h"
#include "ota.h"
#include "net/ipv6/uip-ds6-route.h"

static uip_ipaddr_t coordinator_ip;
static uint8_t has_coordinator_ip = 0;

static uint32_t current_rx_page_offset = 0xFFFFFFFF;
static uint8_t page_bitmap[8];

static uint32_t last_rx_page_offset = 0xFFFFFFFF;
static uint8_t last_page_bitmap[8];

static struct ctimer report_ctimer;
static bitmap_report_t report_to_send;

static struct ctimer start_delay_ctimer;

static void start_delay_callback(void *ptr) {
  LOG_INFO("[OTA] Start delay complete, switching to period 11\n");
  set_shared_period(11);
}

/* Forwarding queue for controlled flooding */
#define FW_QUEUE_SIZE 8

typedef struct {
  fw_packet_t pkt;
  uint16_t len;
} fw_queue_entry_t;

static fw_queue_entry_t fw_queue[FW_QUEUE_SIZE];
static uint8_t fw_queue_head = 0;
static uint8_t fw_queue_tail = 0;
static uint8_t fw_queue_count = 0;

static struct ctimer forward_ctimer;

static void do_forward(void *ptr) {
  if (fw_queue_count == 0) {
    return;
  }
  
  fw_queue_entry_t *entry = &fw_queue[fw_queue_head];
  
  LOG_INFO("[OTA] Forwarding packet: type %u, offset 0x%05lx, len %u (queue: %u)\n",
           entry->pkt.type, (unsigned long)entry->pkt.offset, entry->pkt.length, fw_queue_count);
  send_to_all(&entry->pkt, entry->len);
  
  fw_queue_head = (fw_queue_head + 1) % FW_QUEUE_SIZE;
  fw_queue_count--;
  
  if (fw_queue_count > 0) {
    clock_time_t delay = (random_rand() % 30 + 10) * CLOCK_SECOND / 1000;
    ctimer_set(&forward_ctimer, delay, do_forward, NULL);
  }
}

static void schedule_forward(const fw_packet_t *pkt, uint16_t len) {
  if (fw_queue_count >= FW_QUEUE_SIZE) {
    LOG_WARN("[OTA] Forwarding queue full! Dropping packet (type %u, offset 0x%05lx)\n",
             pkt->type, (unsigned long)pkt->offset);
    return;
  }
  
  memcpy(&fw_queue[fw_queue_tail].pkt, pkt, len);
  fw_queue[fw_queue_tail].len = len;
  fw_queue_tail = (fw_queue_tail + 1) % FW_QUEUE_SIZE;
  fw_queue_count++;
  
  if (fw_queue_count == 1) {
    clock_time_t delay = (random_rand() % 30 + 10) * CLOCK_SECOND / 1000;
    ctimer_set(&forward_ctimer, delay, do_forward, NULL);
  }
}

static void do_send_bitmap(void) {
  if (node_id != 1 && has_coordinator_ip) {
    uip_ipaddr_t dest;
    
    /* Always send to the learned link-local coordinator_ip first. For 1-hop nodes,
     * this bypasses RPL routing tables entirely and is extremely robust under heavy traffic. */
    simple_udp_sendto(&udp_conn, &report_to_send, sizeof(report_to_send), &coordinator_ip);

    /* Also send to the global IP of the root for multi-hop forwarding if available. */
    if (NETSTACK_ROUTING.get_root_ipaddr != NULL &&
        NETSTACK_ROUTING.get_root_ipaddr(&dest) &&
        !uip_is_addr_unspecified(&dest) &&
        !uip_is_addr_linklocal(&dest)) {
      if (memcmp(&dest, &coordinator_ip, sizeof(uip_ipaddr_t)) != 0) {
        simple_udp_sendto(&udp_conn, &report_to_send, sizeof(report_to_send), &dest);
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

static void print_page_bitmap(void) {
  if (current_rx_page_offset != 0xFFFFFFFF) {
    LOG_INFO("[OTA] Page 0x%05lx Rx complete. Bitmap: ",
             (unsigned long)current_rx_page_offset);
    for (int i = 0; i < sizeof(page_bitmap); i++) {
      printf("%02x", page_bitmap[i]);
    }
    printf("\n");

    /* Send report to coordinator if IP is known and we are a sensor node */
    if (node_id != 1 && has_coordinator_ip) {
      report_to_send.type = PKT_TYPE_BITMAP_REPORT;
      report_to_send.page_offset = current_rx_page_offset;
      memcpy(report_to_send.bitmap, page_bitmap, sizeof(page_bitmap));
      schedule_bitmap_send();
    }
  }
}

/* UDP receive callback for Sensor Node */
void udp_rx_callback(struct simple_udp_connection *c,
                     const uip_ipaddr_t *sender_addr,
                     uint16_t sender_port,
                     const uip_ipaddr_t *receiver_addr,
                     uint16_t receiver_port, const uint8_t *data,
                     uint16_t datalen) {
  if (node_id == 1) {
    if (datalen >= sizeof(bitmap_report_t)) {
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
            node_replied[found] = 1;
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
              LOG_INFO("All expected nodes replied. Notifying distribution process.\n");
              process_post(&distribute_process, event_bitmap_received, NULL);
            } else {
              LOG_INFO("Still waiting for other nodes to reply.\n");
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

  /* Sensor node: learn coordinator IP from the first incoming packet.
   * This is used as a fallback; the actual send destination is resolved
   * at send time via RPL (see send_report_callback). */
  if (!has_coordinator_ip) {
    uip_ipaddr_copy(&coordinator_ip, sender_addr);
    has_coordinator_ip = 1;
    LOG_INFO("Learned coordinator IP from sender: ");
    LOG_INFO_6ADDR(&coordinator_ip);
    LOG_INFO_("\n");
  }

  if (datalen < 7) {
    return; /* Too small to be fw_packet_t header */
  }

  const fw_packet_t *pkt = (const fw_packet_t *)data;

  /* Determine if this packet should be forwarded based on RPL parent.
   * A node only forwards downward traffic originating from the root (coordinator)
   * or its preferred parent, which naturally prevents routing loops and handles retransmissions.
   * Additionally, leaf nodes (nodes with no downstream routes) do not need to forward. */
  const uip_ipaddr_t *parent_ip = uip_ds6_defrt_choose();
  uint8_t is_from_parent = (parent_ip != NULL && uip_ipaddr_cmp(sender_addr, parent_ip));
  uint8_t is_from_coordinator = (has_coordinator_ip && uip_ipaddr_cmp(sender_addr, &coordinator_ip));
  uint8_t is_leaf = (uip_ds6_route_head() == NULL);
  uint8_t should_forward = (is_from_parent || is_from_coordinator) && !is_leaf;

  if (pkt->type == PKT_TYPE_START) {
    LOG_INFO("[OTA] START received. File size: %lu\n",
             (unsigned long)pkt->offset);
    current_file_size = pkt->offset;

    /* Reset bitmap variables */
    current_rx_page_offset = 0xFFFFFFFF;
    memset(page_bitmap, 0, sizeof(page_bitmap));
    last_rx_page_offset = 0xFFFFFFFF;
    memset(last_page_bitmap, 0, sizeof(last_page_bitmap));

    if (ext_flash_open(NULL)) {
      /* Erase the first sector eagerly at START so the flash is ready before
       * distribution begins. The coordinator needs several seconds to buffer
       * the first page from UART, giving the erase time to complete. */
      LOG_INFO("Erasing initial sector at 0x00000...\n");
      ext_flash_erase(NULL, 0x00000, EXT_FLASH_ERASE_SECTOR_SIZE);
      ext_flash_close(NULL);
    } else {
      LOG_INFO("[ERROR] Failed to open flash!\n");
    }

    /* Controlled flooding: Forward START if from parent or coordinator */
    if (should_forward) {
      schedule_forward(pkt, datalen);
    }

    /* Set a 5-second ctimer to switch to period 11. This allows the node
     * to finish the flash erase and fully resynchronize with the coordinator
     * on period 61 before transitioning to period 11. */
    ctimer_set(&start_delay_ctimer, CLOCK_SECOND * 5, start_delay_callback, NULL);
  } else if (pkt->type == PKT_TYPE_DATA) {
    set_shared_period(11); /* Reset timeout timer and ensure fast period */
    LOG_INFO("[OTA] DATA received: offset 0x%05lx, len %u\n",
             (unsigned long)pkt->offset, pkt->length);

    uint32_t page_offset =
        pkt->offset - (pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE);
        
    /* If receiving the next page's data, clear the last completed page backup */
    if (page_offset != last_rx_page_offset) {
      last_rx_page_offset = 0xFFFFFFFF;
      memset(last_page_bitmap, 0, sizeof(last_page_bitmap));
    }

    if (current_rx_page_offset == 0xFFFFFFFF) {
      current_rx_page_offset = page_offset;
      memset(page_bitmap, 0, sizeof(page_bitmap));
    }

    /* Check if this chunk is already received to avoid duplicate write to flash */
    uint8_t chunk_already_received = 0;
    uint16_t chunk_idx = (pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE) / 64;
    if (chunk_idx < 64) {
      if (current_rx_page_offset == page_offset) {
        if ((page_bitmap[chunk_idx / 8] & (1 << (chunk_idx % 8))) != 0) {
          chunk_already_received = 1;
        }
      } else if (last_rx_page_offset == page_offset) {
        if ((last_page_bitmap[chunk_idx / 8] & (1 << (chunk_idx % 8))) != 0) {
          chunk_already_received = 1;
        }
      }
    }

    /* Controlled flooding: Forward the packet if from parent or coordinator */
    if (should_forward) {
      schedule_forward(pkt, datalen);
    }

    /* Process the data packet only if it is a new chunk */
    if (!chunk_already_received) {
      if (pkt->length > 0) {
        if (chunk_idx < 64) {
          page_bitmap[chunk_idx / 8] |= (1 << (chunk_idx % 8));
        }
      }

      if (ext_flash_open(NULL)) {
        /* Erase the sector when crossing a boundary.
         * Sector 0 is already erased eagerly in the START handler so the
         * `offset > 0` guard prevents a redundant (and blocking) re-erase. */
        if (pkt->offset > 0 && (pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE) == 0) {
          LOG_INFO("Erasing sector at 0x%05lx...\n", (unsigned long)pkt->offset);
          ext_flash_erase(NULL, pkt->offset, EXT_FLASH_ERASE_SECTOR_SIZE);
        } else if ((pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE) + pkt->length >
                   EXT_FLASH_ERASE_SECTOR_SIZE) {
          /* If a single write crosses a boundary, erase next sector */
          uint32_t next_sector = (pkt->offset / EXT_FLASH_ERASE_SECTOR_SIZE + 1) *
                                 EXT_FLASH_ERASE_SECTOR_SIZE;
          LOG_INFO("Erasing sector at 0x%05lx...\n", (unsigned long)next_sector);
          ext_flash_erase(NULL, next_sector, EXT_FLASH_ERASE_SECTOR_SIZE);
        }

        ext_flash_write(NULL, pkt->offset, pkt->length, pkt->data);
        ext_flash_close(NULL);
      }
    }
  } else if (pkt->type == PKT_TYPE_PAGE_END) {
    LOG_INFO("[OTA] PAGE_END received for offset 0x%05lx\n", (unsigned long)pkt->offset);
    
    /* Controlled flooding: Forward PAGE_END if from parent or coordinator */
    if (should_forward) {
      schedule_forward(pkt, datalen);
    }

    if (pkt->offset == current_rx_page_offset) {
      /* Save to last completed page backup variables */
      last_rx_page_offset = current_rx_page_offset;
      memcpy(last_page_bitmap, page_bitmap, sizeof(page_bitmap));
      
      /* Print and send the report */
      print_page_bitmap();
      
      /* Reset current page state to be ready for the next page */
      current_rx_page_offset = 0xFFFFFFFF;
    } else if (pkt->offset == last_rx_page_offset) {
      /* Retransmission request from coordinator. Send the last backup report */
      LOG_INFO("[OTA] Retransmitting bitmap report for page 0x%05lx\n", (unsigned long)last_rx_page_offset);
      if (has_coordinator_ip) {
        report_to_send.type = PKT_TYPE_BITMAP_REPORT;
        report_to_send.page_offset = last_rx_page_offset;
        memcpy(report_to_send.bitmap, last_page_bitmap, sizeof(last_page_bitmap));
        schedule_bitmap_send();
      }
    }
  } else if (pkt->type == PKT_TYPE_VERIFY) {
    LOG_INFO("[OTA] VERIFY request received! Expected CRC: 0x%08lx\n",
             (unsigned long)pkt->offset);

    /* Controlled flooding: Forward VERIFY if from parent or coordinator */
    if (should_forward) {
      schedule_forward(pkt, datalen);
    }

    /* Print final page bitmap (if not already printed by PAGE_END) */
    print_page_bitmap();
    current_rx_page_offset = 0xFFFFFFFF;

    uint32_t checksum = 0;
    if (ext_flash_open(NULL)) {
      uint8_t buf[64];
      for (uint32_t i = 0; i < current_file_size; i += sizeof(buf)) {
        uint16_t chunk = (current_file_size - i > sizeof(buf))
                             ? sizeof(buf)
                             : (current_file_size - i);
        ext_flash_read(NULL, i, chunk, buf);
        for (int j = 0; j < chunk; j++) {
          checksum += buf[j];
        }
      }
      ext_flash_close(NULL);
    }
    checksum &= 0xFFFFFFFF;
    if (checksum == pkt->offset) {
      LOG_INFO("[OTA VERIFY SUCCESS] Checksum matches! (0x%08lx)\n",
               (unsigned long)checksum);
    } else {
      LOG_INFO("[OTA VERIFY FAILED] Checksum mismatch! Expected 0x%08lx, got 0x%08lx\n",
               (unsigned long)pkt->offset, (unsigned long)checksum);
    }
    set_shared_period(61);
    ota_sensor_node_reset_forwarding();
  }
}

void ota_sensor_node_reset_forwarding(void) {
  ctimer_stop(&forward_ctimer);
  fw_queue_head = 0;
  fw_queue_tail = 0;
  fw_queue_count = 0;
  LOG_INFO("[OTA] Reset sensor node forwarding queue\n");
}
