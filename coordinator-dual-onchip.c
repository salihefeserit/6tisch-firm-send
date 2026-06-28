#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "sys/log.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-ds6.h"
#include "ota.h"

uip_ipaddr_t session_nodes[MAX_NODES];
uint8_t session_nodes_count = 0;
uint8_t node_replied[MAX_NODES];
uint8_t session_bitmaps[MAX_NODES][8];
uint8_t retry_count = 0;

process_event_t event_bitmap_received;

PROCESS(distribute_process, "OTA Distribution Process");

static void notify_uart_no_targets(void) {
  printf("FW:NO_TARGETS\n");
}

int find_session_node_by_addr(const uip_ipaddr_t *addr) {
  for (int i = 0; i < session_nodes_count; i++) {
    if (memcmp(&session_nodes[i].u8[8], &addr->u8[8], 8) == 0) {
      return i;
    }
  }
  return -1;
}

void remove_session_node(int index) {
  if (index < 0 || index >= session_nodes_count) {
    return;
  }

  for (int i = index + 1; i < session_nodes_count; i++) {
    uip_ipaddr_copy(&session_nodes[i - 1], &session_nodes[i]);
    node_replied[i - 1] = node_replied[i];
    memcpy(session_bitmaps[i - 1], session_bitmaps[i], 8);
  }
  session_nodes_count--;
  if (session_nodes_count == 0) {
    notify_uart_no_targets();
  }
}

int ota_coordinator_has_routes(void) {
  return tsch_is_associated && (uip_ds6_route_head() != NULL);
}

/* Check if all active session nodes have received all chunks of the current page */
static uint8_t page_is_complete(uint16_t total_chunks) {
  if (session_nodes_count == 0) {
    return 1;
  }
  for (int i = 0; i < session_nodes_count; i++) {
    for (int chunk_idx = 0; chunk_idx < total_chunks; chunk_idx++) {
      uint8_t byte_idx = chunk_idx / 8;
      uint8_t bit_idx = chunk_idx % 8;
      if ((session_bitmaps[i][byte_idx] & (1 << bit_idx)) == 0) {
        return 0; /* A node is missing this chunk */
      }
    }
  }
  return 1;
}

/* Process thread for distributing a page to all nodes */
PROCESS_THREAD(distribute_process, ev, data) {
  static struct etimer dist_timer;
  static uint16_t dist_chunk_idx;
  static uint16_t total_chunks;
  static fw_packet_t end_pkt;
  static uint8_t recovery_round;
  static uint16_t page_crc;

  PROCESS_BEGIN();

  total_chunks = (expected_page_size + 63) / 64;
  LOG_INFO("DEBUG crc16_add('1', 0) = 0x%04x\n", crc16_add('1', 0));
  LOG_INFO("DEBUG crc16_add(0xef, 0) = 0x%04x\n", crc16_add(0xef, 0));
  LOG_INFO("Page Buffer Header: ");
  for(int i = 0; i < 16 && i < expected_page_size; i++) {
    printf("%02x", page_buffer[i]);
  }
  printf("\n");
  page_crc = crc16_data(page_buffer, expected_page_size, 0);
  LOG_INFO("Distributing page at offset 0x%05lx, total chunks: %u, CRC-16: 0x%04x\n",
           (unsigned long)page_start_offset, total_chunks, page_crc);

  /* Reset session bitmaps to 0 for the new page */
  memset(session_bitmaps, 0, sizeof(session_bitmaps));

  if (session_nodes_count == 0) {
    LOG_INFO("No active target nodes. Skipping page transmission.\n");
    distribution_in_progress = 0;
    notify_uart_no_targets();
    PROCESS_EXIT();
  }

  for (dist_chunk_idx = 0; dist_chunk_idx < total_chunks; dist_chunk_idx++) {
    uint32_t chunk_abs_offset = page_start_offset + dist_chunk_idx * 64;
    uint16_t chunk_len = 64;
    if (chunk_abs_offset + chunk_len > page_start_offset + expected_page_size) {
      chunk_len = (page_start_offset + expected_page_size) - chunk_abs_offset;
    }

    fw_packet_t pkt;
    pkt.type = PKT_TYPE_DATA;
    pkt.offset = chunk_abs_offset;
    pkt.length = chunk_len;
    pkt.target_slot = ota_target_slot;
    memcpy(pkt.data, &page_buffer[dist_chunk_idx * 64], chunk_len);

    send_to_all(&pkt, FW_PACKET_HEADER_LEN + chunk_len);
    LOG_INFO("Distributed chunk %u/%u: offset 0x%05lx, len %u\n",
             dist_chunk_idx + 1, total_chunks, (unsigned long)chunk_abs_offset,
             chunk_len);

    /* Wait for 80 ms to avoid network congestion and queue overflows (optimized for line topology) */
    etimer_set(&dist_timer, CLOCK_SECOND * 8 / 100);
    PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER && data == &dist_timer);
  }

  LOG_INFO("Page distribution complete for offset 0x%05lx\n",
           (unsigned long)page_start_offset);

  recovery_round = 0;

  /* Loop until all active nodes report 100% completion of the page */
  while (1) {
    if (session_nodes_count == 0) {
      LOG_INFO("No active nodes tracked in this session. Skipping wait for bitmap reports.\n");
      break;
    }

    /* Check if the page is complete on all nodes before doing a recovery round */
    if (page_is_complete(total_chunks)) {
      LOG_INFO("Page 0x%05lx successfully completed on all active nodes!\n",
               (unsigned long)page_start_offset);
      break;
    }

    if (recovery_round >= 10) {
      LOG_WARN("Max page recovery rounds (10) reached! Moving to next page anyway.\n");
      break;
    }
    recovery_round++;

    LOG_INFO("Expecting reports from %u nodes (Recovery Round %u):\n",
             session_nodes_count, recovery_round);
    for (int i = 0; i < session_nodes_count; i++) {
      LOG_INFO("  - ");
      LOG_INFO_6ADDR(&session_nodes[i]);
      LOG_INFO_("\n");
    }

    /* Reset reply tracking for this round */
    memset(node_replied, 0, sizeof(node_replied));
    retry_count = 0;

    /* Broadcast PAGE_END packet to prompt reports */
    end_pkt.type = PKT_TYPE_PAGE_END;
    end_pkt.offset = page_start_offset;
    end_pkt.length = page_crc;
    end_pkt.target_slot = ota_target_slot;
    send_to_all(&end_pkt, FW_PACKET_HEADER_LEN);
    LOG_INFO("Sent PAGE_END for offset 0x%05lx, waiting for bitmap...\n",
             (unsigned long)page_start_offset);

    /* Wait for bitmap reports (or timeout of 15.0 seconds) */
    etimer_set(&dist_timer, CLOCK_SECOND * 15);
    while (1) {
      PROCESS_WAIT_EVENT();
      if (ev == PROCESS_EVENT_TIMER && data == &dist_timer) {
        retry_count++;
        
        /* Print pending nodes */
        LOG_INFO("Timeout (%d/3) waiting for reports. Pending nodes:\n", retry_count);
        for (int i = 0; i < session_nodes_count; i++) {
          if (!node_replied[i]) {
            LOG_INFO("  - ");
            LOG_INFO_6ADDR(&session_nodes[i]);
            LOG_INFO_("\n");
          }
        }

        if (retry_count >= 3) {
          /* Check if at least one node replied in this round */
          uint8_t replied_count = 0;
          for (int i = 0; i < session_nodes_count; i++) {
            if (node_replied[i]) {
              replied_count++;
            }
          }

          if (replied_count > 0) {
            LOG_INFO("Max retries reached. Dropping unresponsive nodes:\n");
            /* Drop unresponsive nodes by compacting session_nodes array */
            int write_idx = 0;
            for (int i = 0; i < session_nodes_count; i++) {
              if (node_replied[i]) {
                if (write_idx != i) {
                  uip_ipaddr_copy(&session_nodes[write_idx], &session_nodes[i]);
                  memcpy(session_bitmaps[write_idx], session_bitmaps[i], 8);
                }
                write_idx++;
              } else {
                LOG_INFO("  - Dropped: ");
                LOG_INFO_6ADDR(&session_nodes[i]);
                LOG_INFO_("\n");
              }
            }
            session_nodes_count = write_idx;
            break; /* Break the wait loop to evaluate completeness and trigger retries */
          } else {
            LOG_WARN("Max retries reached but 0 nodes replied in this round. Resetting retry count and continuing to wait...\n");
            retry_count = 0;
          }
        }
        
        LOG_INFO("Sending PAGE_END again...\n");
        send_to_all(&end_pkt, FW_PACKET_HEADER_LEN);
        etimer_set(&dist_timer, CLOCK_SECOND * 15);
      } else if (ev == event_bitmap_received) {
        /* Double check if all nodes have actually replied in this round to ignore stale events */
        uint8_t all_replied = 1;
        for (int i = 0; i < session_nodes_count; i++) {
          if (!node_replied[i]) {
            all_replied = 0;
            break;
          }
        }
        if (all_replied) {
          LOG_INFO("Bitmap reports received from all active nodes!\n");
          break;
        } else {
          LOG_INFO("Ignored stale event_bitmap_received event.\n");
        }
      }
    }

    /* Check completeness again after wait loop exits */
    if (page_is_complete(total_chunks)) {
      LOG_INFO("Page 0x%05lx successfully completed on all active nodes!\n",
               (unsigned long)page_start_offset);
      break;
    }

    /* We have missing chunks! Iterate over chunks and count misses */
    LOG_INFO("Analyzing missing chunks for page 0x%05lx...\n", (unsigned long)page_start_offset);
    for (dist_chunk_idx = 0; dist_chunk_idx < total_chunks; dist_chunk_idx++) {
      uint8_t byte_idx = dist_chunk_idx / 8;
      uint8_t bit_idx = dist_chunk_idx % 8;
      uint8_t missed_count = 0;

      for (int i = 0; i < session_nodes_count; i++) {
        if ((session_bitmaps[i][byte_idx] & (1 << bit_idx)) == 0) {
          missed_count++;
        }
      }

      if (missed_count > 0) {
        uint32_t chunk_abs_offset = page_start_offset + dist_chunk_idx * 64;
        uint16_t chunk_len = 64;
        if (chunk_abs_offset + chunk_len > page_start_offset + expected_page_size) {
          chunk_len = (page_start_offset + expected_page_size) - chunk_abs_offset;
        }

        fw_packet_t pkt;
        pkt.type = PKT_TYPE_DATA;
        pkt.offset = chunk_abs_offset;
        pkt.length = chunk_len;
        pkt.target_slot = ota_target_slot;
        memcpy(pkt.data, &page_buffer[dist_chunk_idx * 64], chunk_len);

        LOG_INFO("Retransmitting chunk %u (offset 0x%05lx, missed by %u nodes)\n",
                 dist_chunk_idx, (unsigned long)chunk_abs_offset, missed_count);
        send_to_all(&pkt, FW_PACKET_HEADER_LEN + chunk_len);

        /* Wait for 80 ms pacing delay between retransmissions (optimized for line topology) */
        etimer_set(&dist_timer, CLOCK_SECOND * 8 / 100);
        PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER && data == &dist_timer);
      }
    }
  }

  /* Advance to next page */
  page_start_offset += expected_page_size;
  page_bytes_received = 0;
  if (page_start_offset < current_file_size) {
    expected_page_size = (current_file_size - page_start_offset > PAGE_SIZE)
                             ? PAGE_SIZE
                             : (current_file_size - page_start_offset);
  } else {
    expected_page_size = 0;
  }
  distribution_in_progress = 0;

  /* Print FW:PAGE_OK to serial line to notify python script */
  printf("FW:PAGE_OK\n");

  PROCESS_END();
}

/* Parse and process incoming firmware commands over UART */
void ota_coordinator_handle_uart(const char *str) {
  extern struct process serial_shell_process;
  uint32_t offset;
  unsigned int length;
  unsigned int target_slot;
  static char hexdata[130];

  memset(hexdata, 0, sizeof(hexdata));

  if (str[3] == 'S') {
    /* Stop the serial shell to prevent 'Command not found' outputs
     * during transmission */
    if (process_is_running(&serial_shell_process)) {
      process_exit(&serial_shell_process);
    }
    if (sscanf(str, "FW:S:%lx:%u", &offset, &target_slot) == 2 &&
        target_slot <= OTA_SLOT_B) {
      fw_packet_t pkt;
      pkt.type = PKT_TYPE_START;
      pkt.offset = offset;
      pkt.length = 0;
      pkt.target_slot = (uint8_t)target_slot;
      ota_target_slot = (uint8_t)target_slot;
      send_to_all(&pkt, FW_PACKET_HEADER_LEN);
      LOG_INFO("Sent START, size: %lu, target slot: %c\n",
               (unsigned long)offset,
               ota_target_slot == OTA_SLOT_A ? 'A' : 'B');

      /* Populate session nodes from RPL routing table */
      session_nodes_count = 0;
      uip_ds6_route_t *route;
      for(route = uip_ds6_route_head(); route != NULL; route = uip_ds6_route_next(route)) {
        if(session_nodes_count < MAX_NODES) {
          uip_ipaddr_copy(&session_nodes[session_nodes_count], &route->ipaddr);
          session_nodes_count++;
        } else {
          LOG_WARN("Too many routing entries! Truncating to MAX_NODES (%d)\n", MAX_NODES);
          break;
        }
      }
      if(session_nodes_count == 0) {
        LOG_WARN("No active routes at session start!\n");
      } else {
        LOG_INFO("Session started with %u active nodes:\n", session_nodes_count);
        for(int i = 0; i < session_nodes_count; i++) {
          LOG_INFO("  - ");
          LOG_INFO_6ADDR(&session_nodes[i]);
          LOG_INFO_("\n");
        }
      }

      /* Initialize page variables */
      current_file_size = offset;
      ota_target_slot = (uint8_t)target_slot;
      page_start_offset = 0;
      page_bytes_received = 0;
      expected_page_size = (current_file_size > PAGE_SIZE)
                               ? PAGE_SIZE
                               : current_file_size;
      distribution_in_progress = 0;

      /* Acknowledge START command */
      printf("FW:ACK\n");
      if (session_nodes_count == 0) {
        notify_uart_no_targets();
      }
    } else {
      LOG_WARN("[ERROR] START command must be FW:S:<size>:<slot>, slot 0=A 1=B\n");
    }
  } else if (str[3] == 'D') {
    if (sscanf(str, "FW:D:%lx:%x:%128s", &offset, &length, hexdata) >= 3) {
      if (session_nodes_count == 0) {
        LOG_INFO("[OTA] No active target nodes, rejecting UART DATA chunk.\n");
        notify_uart_no_targets();
      } else if (distribution_in_progress) {
        LOG_INFO("[ERROR] Page distribution in progress, ignoring UART chunk!\n");
      } else if (offset >= page_start_offset &&
                 offset < page_start_offset + expected_page_size) {
        uint32_t rel_offset = offset - page_start_offset;
        if (rel_offset + length <= expected_page_size) {
          if (strlen(hexdata) != length * 2) {
            LOG_WARN("[ERROR] Hex data length mismatch! Expected %u, got %u\n",
                     length * 2, (unsigned int)strlen(hexdata));
            return;
          }
          for (int i = 0; i < length; i++) {
            page_buffer[rel_offset + i] =
                (hex2val(hexdata[i * 2]) << 4) |
                hex2val(hexdata[i * 2 + 1]);
          }
          page_bytes_received += length;
          LOG_INFO("Buffered chunk offset 0x%05lx, len %u. Progress: %u/%u\n",
                   (unsigned long)offset, length, page_bytes_received,
                   expected_page_size);

          /* Acknowledge successful buffering of this chunk */
          printf("FW:ACK\n");

          if (page_bytes_received == expected_page_size) {
            distribution_in_progress = 1;
            process_start(&distribute_process, NULL);
          }
        } else {
          LOG_INFO("[ERROR] Chunk write exceeds page boundary!\n");
        }
      } else {
        LOG_INFO("[ERROR] Chunk offset 0x%05lx out of bounds for page starting at 0x%05lx\n",
                 (unsigned long)offset, (unsigned long)page_start_offset);
      }
    }
  } else if (str[3] == 'V') {
    if (sscanf(str, "FW:V:%lx", &offset) == 1) {
      if (session_nodes_count == 0) {
        LOG_INFO("[OTA] No active target nodes, ignoring VERIFY.\n");
        notify_uart_no_targets();
        return;
      }

      fw_packet_t pkt;
      pkt.type = PKT_TYPE_VERIFY;
      pkt.offset = offset;
      pkt.length = 0;
      pkt.target_slot = ota_target_slot;
      send_to_all(&pkt, FW_PACKET_HEADER_LEN);
      LOG_INFO("Sent VERIFY, expected checksum: 0x%08lx\n",
               (unsigned long)offset);

      /* Acknowledge VERIFY command */
      printf("FW:ACK\n");

      /* Restart the serial shell now that transmission is complete */
      if (!process_is_running(&serial_shell_process)) {
        process_start(&serial_shell_process, NULL);
      }
    }
  }
}
