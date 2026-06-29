#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "sys/log.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-ds6.h"
#include "ota.h"

/*
 * Architecture-neutral OTA coordinator.
 *
 * The coordinator only forwards the target selected by UART. Dual-onchip vs
 * offchip compatibility is decided by each sensor node during START admission.
 */
PROCESS(distribute_process, "OTA Distribution Process");

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
  page_crc = crc16_data(page_buffer, expected_page_size, 0);
  LOG_INFO("Distributing page at offset 0x%05lx, total chunks: %u, CRC-16: 0x%04x\n",
           (unsigned long)page_start_offset, total_chunks, page_crc);

  /* Reset session bitmaps to 0 for the new page */
  memset(session_bitmaps, 0, sizeof(session_bitmaps));

  if (session_nodes_count == 0) {
    LOG_INFO("No active target nodes. Skipping page transmission.\n");
    distribution_in_progress = 0;
    ota_notify_uart_no_targets();
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
    if (ota_page_is_complete(total_chunks)) {
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
    if (ota_page_is_complete(total_chunks)) {
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
  char start_arg1[16];
  char start_arg2[16];
  static char hexdata[130];

  memset(start_arg1, 0, sizeof(start_arg1));
  memset(start_arg2, 0, sizeof(start_arg2));
  memset(hexdata, 0, sizeof(hexdata));

  if (str[3] == 'S') {
    /* Stop the serial shell to prevent 'Command not found' outputs
     * during transmission */
    if (process_is_running(&serial_shell_process)) {
      process_exit(&serial_shell_process);
    }
    int start_fields =
        sscanf(str, "FW:S:%lx:%15[^:]:%15s", &offset, start_arg1,
               start_arg2);
    if (start_fields >= 2) {
      uint16_t image_sec_ver = OTA_START_SEC_VER_UNKNOWN;
      uint8_t target_slot;
      const char *target_arg = start_arg1;

      if (start_fields >= 3) {
        image_sec_ver = (uint16_t)strtoul(start_arg1, NULL, 0);
        target_arg = start_arg2;
      }

      target_slot = ota_parse_target(target_arg);
      if (target_slot == OTA_TARGET_INVALID) {
        LOG_WARN("[ERROR] Invalid OTA target: %s\n", target_arg);
        ota_reset_transfer_state(0);
        ota_notify_uart_no_targets();
        return;
      }

      ota_start_admission_cancel();

      ota_populate_session_nodes_from_routes();
      if(session_nodes_count == 0) {
        LOG_WARN("No active routes at session start!\n");
        ota_reset_transfer_state(0);
        ota_notify_uart_no_targets();
        return;
      }

      LOG_INFO("Session admission started for %u routed nodes:\n",
               session_nodes_count);
      for(int i = 0; i < session_nodes_count; i++) {
        LOG_INFO("  - ");
        LOG_INFO_6ADDR(&session_nodes[i]);
        LOG_INFO_("\n");
      }

      fw_packet_t pkt;
      pkt.type = PKT_TYPE_START;
      pkt.offset = offset;
      pkt.length = image_sec_ver;
      pkt.target_slot = target_slot;
      ota_target_slot = target_slot;
      send_to_all(&pkt, FW_PACKET_HEADER_LEN);
      LOG_INFO("Sent START, size: %lu, secVer: %u, target: %s\n",
               (unsigned long)offset, image_sec_ver,
               ota_target_name(ota_target_slot));

      ota_reset_transfer_state(offset);
      ota_start_admission_begin();
    } else {
      LOG_WARN("[ERROR] START command must be FW:S:<size>:<secVer>:<slot>, slot 0=A 1=B\n");
    }
  } else if (str[3] == 'D') {
    if (sscanf(str, "FW:D:%lx:%x:%128s", &offset, &length, hexdata) >= 3) {
      if (ota_start_admission_is_in_progress()) {
        LOG_INFO("[ERROR] START admission in progress, ignoring UART chunk!\n");
      } else if (session_nodes_count == 0) {
        LOG_INFO("[OTA] No active target nodes, rejecting UART DATA chunk.\n");
        ota_notify_uart_no_targets();
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
        ota_notify_uart_no_targets();
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
