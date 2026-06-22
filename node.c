/*
 * 6TiSCH dual-role node:
 *   - node_id == 1  → Coordinator (Receives OTA over UART, sends over UDP)
 *   - node_id != 1  → Sensor-node (Receives OTA over UDP, writes to Flash)
 */

#include "contiki.h"
#include "net/ipv6/simple-udp.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-ds6.h"
#include "net/mac/tsch/tsch.h"
#include "net/netstack.h"
#include "net/routing/routing.h"
#include "services/simple-energest/simple-energest.h"
#include "sys/log.h"
#include "sys/node-id.h"
#include "dev/serial-line.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "net/mac/tsch/tsch-schedule.h"
#include "services/orchestra/orchestra.h"

#include "ext-flash.h"
#include "Board.h"
#include <ti/drivers/PIN.h>

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_PORT 8765

/* OTA Packet Definitions */
#define PKT_TYPE_START  0
#define PKT_TYPE_DATA   1
#define PKT_TYPE_VERIFY 2

typedef struct __attribute__((packed)) {
  uint8_t type;
  uint32_t offset;
  uint16_t length;
  uint8_t data[64];
} fw_packet_t;

/* Global variables for flash tracking */
static uint32_t current_file_size = 0;

#define PAGE_SIZE 4096
static uint8_t page_buffer[PAGE_SIZE];
static uint32_t page_start_offset = 0;
static uint16_t page_bytes_received = 0;
static uint16_t expected_page_size = 0;
static uint8_t distribution_in_progress = 0;

/*---------------------------------------------------------------------------*/
PROCESS(node_process, "6TiSCH Node");
PROCESS(distribute_process, "OTA Distribution Process");
AUTOSTART_PROCESSES(&node_process);

/*---------------------------------------------------------------------------*/
static struct simple_udp_connection udp_conn;

static void set_shared_period(uint16_t new_size);
static void reset_ota_timer(void);

/*---------------------------------------------------------------------------*/
/* Fast hex char to byte converter */
static uint8_t hex2val(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

/* Page chunk delivery tracking bitmap */
static uint32_t current_rx_page_offset = 0xFFFFFFFF;
static uint8_t page_bitmap[8];

static void print_page_bitmap(void) {
  if(current_rx_page_offset != 0xFFFFFFFF) {
    LOG_INFO("[OTA] Page 0x%05lx Rx complete. Bitmap: ", (unsigned long)current_rx_page_offset);
    for(int i = 0; i < sizeof(page_bitmap); i++) {
      printf("%02x", page_bitmap[i]);
    }
    printf("\n");
  }
}

/*---------------------------------------------------------------------------*/
/* UDP receive callback for Sensor Node */
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  if(datalen < 7) {
    return; /* Too small to be fw_packet_t header */
  }

  const fw_packet_t *pkt = (const fw_packet_t *)data;

  if(pkt->type == PKT_TYPE_START) {
    set_shared_period(11);
    LOG_INFO("[OTA] START received. File size: %lu\n", (unsigned long)pkt->offset);
    current_file_size = pkt->offset;
    
    /* Reset bitmap variables */
    current_rx_page_offset = 0xFFFFFFFF;
    memset(page_bitmap, 0, sizeof(page_bitmap));
    
    if(ext_flash_open(NULL)) {
      /* Erase the first sector */
      LOG_INFO("Erasing initial sector at 0x00000...\n");
      ext_flash_erase(NULL, 0x00000, EXT_FLASH_ERASE_SECTOR_SIZE);
      ext_flash_close(NULL);
    } else {
      LOG_INFO("[ERROR] Failed to open flash!\n");
    }
  } 
  else if(pkt->type == PKT_TYPE_DATA) {
    set_shared_period(11); /* Reset timeout timer and ensure fast period */
    LOG_INFO("[OTA] DATA received: offset 0x%05lx, len %u\n", (unsigned long)pkt->offset, pkt->length);
    
    uint32_t page_offset = pkt->offset - (pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE);
    if(current_rx_page_offset == 0xFFFFFFFF) {
      current_rx_page_offset = page_offset;
      memset(page_bitmap, 0, sizeof(page_bitmap));
    } else if(page_offset != current_rx_page_offset) {
      /* Page boundary crossed, print previous page bitmap */
      print_page_bitmap();
      current_rx_page_offset = page_offset;
      memset(page_bitmap, 0, sizeof(page_bitmap));
    }

    if(pkt->length > 0) {
      uint16_t chunk_idx = (pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE) / pkt->length;
      if(chunk_idx < 64) {
        page_bitmap[chunk_idx / 8] |= (1 << (chunk_idx % 8));
      }
    }

    if(ext_flash_open(NULL)) {
      /* If crossing a sector boundary, erase the new sector */
      if(pkt->offset > 0 && (pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE) == 0) {
        LOG_INFO("Erasing sector at 0x%05lx...\n", (unsigned long)pkt->offset);
        ext_flash_erase(NULL, pkt->offset, EXT_FLASH_ERASE_SECTOR_SIZE);
      } else if((pkt->offset % EXT_FLASH_ERASE_SECTOR_SIZE) + pkt->length > EXT_FLASH_ERASE_SECTOR_SIZE) {
        /* If a single write crosses a boundary, erase next sector */
        uint32_t next_sector = (pkt->offset / EXT_FLASH_ERASE_SECTOR_SIZE + 1) * EXT_FLASH_ERASE_SECTOR_SIZE;
        LOG_INFO("Erasing sector at 0x%05lx...\n", (unsigned long)next_sector);
        ext_flash_erase(NULL, next_sector, EXT_FLASH_ERASE_SECTOR_SIZE);
      }
      
      ext_flash_write(NULL, pkt->offset, pkt->length, pkt->data);
      ext_flash_close(NULL);
    }
  }
  else if(pkt->type == PKT_TYPE_VERIFY) {
    LOG_INFO("[OTA] VERIFY request received! Expected CRC: 0x%08lx\n", (unsigned long)pkt->offset);
    
    /* Print final page bitmap */
    print_page_bitmap();
    current_rx_page_offset = 0xFFFFFFFF;
    
    uint32_t checksum = 0;
    if(ext_flash_open(NULL)) {
       uint8_t buf[64];
       for(uint32_t i = 0; i < current_file_size; i += sizeof(buf)) {
         uint16_t chunk = (current_file_size - i > sizeof(buf)) ? sizeof(buf) : (current_file_size - i);
         ext_flash_read(NULL, i, chunk, buf);
         for(int j=0; j<chunk; j++) {
           checksum += buf[j];
         }
       }
       ext_flash_close(NULL);
    }
    checksum &= 0xFFFFFFFF;
    if(checksum == pkt->offset) {
      LOG_INFO("[OTA VERIFY SUCCESS] Checksum matches! (0x%08lx)\n", (unsigned long)checksum);
    } else {
      LOG_INFO("[OTA VERIFY FAILED] Checksum mismatch! Expected 0x%08lx, got 0x%08lx\n", (unsigned long)pkt->offset, (unsigned long)checksum);
    }
    set_shared_period(61);
  }
}

/*---------------------------------------------------------------------------*/
static int
coordinator_has_routes(void)
{
  return tsch_is_associated &&
         (uip_ds6_route_head() != NULL);
}

static uint8_t shared_period_is_fast = 0;
static struct etimer ota_timeout_timer;
#define OTA_TIMEOUT_DURATION (60 * CLOCK_SECOND)

static void set_shared_period(uint16_t new_size) {
  struct tsch_slotframe *sf = tsch_schedule_slotframe_head();
  while(sf != NULL) {
    if(sf->size.val == 61 || sf->size.val == 11) {
      if(sf->size.val != new_size) {
        LOG_INFO("[OTA] Changing shared slotframe size from %u to %u\n", sf->size.val, new_size);
        TSCH_ASN_DIVISOR_INIT(sf->size, new_size);
      }
      if(new_size == 11) {
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

static void reset_ota_timer(void) {
  if(shared_period_is_fast) {
    etimer_set(&ota_timeout_timer, OTA_TIMEOUT_DURATION);
  }
}

/*---------------------------------------------------------------------------*/
static void send_to_all(fw_packet_t *pkt, uint16_t len) {
  uip_ipaddr_t dest_ip;
  uip_create_linklocal_allnodes_mcast(&dest_ip);
  simple_udp_sendto(&udp_conn, pkt, len, &dest_ip);
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(distribute_process, ev, data)
{
  static struct etimer dist_timer;
  static uint16_t dist_chunk_idx;
  static uint16_t total_chunks;

  PROCESS_BEGIN();

  total_chunks = (expected_page_size + 63) / 64;
  LOG_INFO("Distributing page at offset 0x%05lx, total chunks: %u\n", (unsigned long)page_start_offset, total_chunks);

  for(dist_chunk_idx = 0; dist_chunk_idx < total_chunks; dist_chunk_idx++) {
    uint32_t chunk_abs_offset = page_start_offset + dist_chunk_idx * 64;
    uint16_t chunk_len = 64;
    if(chunk_abs_offset + chunk_len > page_start_offset + expected_page_size) {
      chunk_len = (page_start_offset + expected_page_size) - chunk_abs_offset;
    }

    fw_packet_t pkt;
    pkt.type = PKT_TYPE_DATA;
    pkt.offset = chunk_abs_offset;
    pkt.length = chunk_len;
    memcpy(pkt.data, &page_buffer[dist_chunk_idx * 64], chunk_len);

    send_to_all(&pkt, 7 + chunk_len);
    LOG_INFO("Distributed chunk %u/%u: offset 0x%05lx, len %u\n", 
             dist_chunk_idx + 1, total_chunks, (unsigned long)chunk_abs_offset, chunk_len);

    /* Wait for 150 ms to avoid network congestion and queue overflows */
    etimer_set(&dist_timer, CLOCK_SECOND * 15 / 100);
    PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER && data == &dist_timer);
  }

  LOG_INFO("Page distribution complete for offset 0x%05lx\n", (unsigned long)page_start_offset);

  /* Advance to next page */
  page_start_offset += expected_page_size;
  page_bytes_received = 0;
  if(page_start_offset < current_file_size) {
    expected_page_size = (current_file_size - page_start_offset > PAGE_SIZE) ? PAGE_SIZE : (current_file_size - page_start_offset);
  } else {
    expected_page_size = 0;
  }
  distribution_in_progress = 0;

  /* Print FW:PAGE_OK to serial line to notify python script */
  printf("FW:PAGE_OK\n");

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(node_process, ev, data)
{
  PROCESS_BEGIN();

  /* Simple Energest Monitoring */
  simple_energest_init();

  if(node_id == 1) {
    LOG_INFO("This device is COORDINATOR (node_id=1).\n");
    tsch_set_coordinator(1);
    NETSTACK_ROUTING.root_start();
  } else {
    LOG_INFO("This device is SENSOR-NODE (node_id=%u). Waiting for coordinator...\n", node_id);
  }

  /* Start TSCH MAC layer */
  NETSTACK_MAC.on();

  /* Register UDP socket */
  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_rx_callback);

  if(node_id == 1) {
    LOG_INFO("Waiting for TSCH association and first sensor-node route...\n");

    while(!coordinator_has_routes()) {
      PROCESS_PAUSE();
    }

    LOG_INFO("Routes detected. Ready to receive firmware via UART.\n");
    
    while(1) {
      PROCESS_WAIT_EVENT();
      if(ev == serial_line_event_message && data != NULL) {
        char *str = (char *)data;
        
        extern struct process serial_shell_process;
        if(strncmp(str, "FW:", 3) == 0) {
          uint32_t offset;
          unsigned int length;
          static char hexdata[130];
          
          memset(hexdata, 0, sizeof(hexdata));
          
          if(str[3] == 'S') {
            /* Stop the serial shell to prevent 'Command not found' outputs during transmission */
            if(process_is_running(&serial_shell_process)) {
              process_exit(&serial_shell_process);
            }
            if(sscanf(str, "FW:S:%lx", &offset) == 1) {
              fw_packet_t pkt;
              pkt.type = PKT_TYPE_START;
              pkt.offset = offset;
              pkt.length = 0;
              send_to_all(&pkt, 7); /* 7 is offsetof(fw_packet_t, data) */
              LOG_INFO("Sent START, size: %lu\n", (unsigned long)offset);

              /* Initialize page variables */
              current_file_size = offset;
              page_start_offset = 0;
              page_bytes_received = 0;
              expected_page_size = (current_file_size > PAGE_SIZE) ? PAGE_SIZE : current_file_size;
              distribution_in_progress = 0;
              
              /* Acknowledge START command */
              printf("FW:ACK\n");
            }
          }
          else if(str[3] == 'D') {
            if(sscanf(str, "FW:D:%lx:%x:%128s", &offset, &length, hexdata) >= 3) {
              set_shared_period(11); /* Switch to fast period on data transmission */
              
              if(distribution_in_progress) {
                LOG_INFO("[ERROR] Page distribution in progress, ignoring UART chunk!\n");
              } else if(offset >= page_start_offset && offset < page_start_offset + expected_page_size) {
                uint32_t rel_offset = offset - page_start_offset;
                if(rel_offset + length <= expected_page_size) {
                  for(int i = 0; i < length; i++) {
                    page_buffer[rel_offset + i] = (hex2val(hexdata[i*2]) << 4) | hex2val(hexdata[i*2+1]);
                  }
                  page_bytes_received += length;
                  LOG_INFO("Buffered chunk offset 0x%05lx, len %u. Progress: %u/%u\n",
                           (unsigned long)offset, length, page_bytes_received, expected_page_size);

                  /* Acknowledge successful buffering of this chunk */
                  printf("FW:ACK\n");

                  if(page_bytes_received == expected_page_size) {
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
          }
          else if(str[3] == 'V') {
            if(sscanf(str, "FW:V:%lx", &offset) == 1) {
              fw_packet_t pkt;
              pkt.type = PKT_TYPE_VERIFY;
              pkt.offset = offset;
              pkt.length = 0;
              send_to_all(&pkt, 7);
              LOG_INFO("Sent VERIFY, expected checksum: 0x%08lx\n", (unsigned long)offset);
              
              /* Acknowledge VERIFY command */
              printf("FW:ACK\n");

              reset_ota_timer(); /* Let the timeout timer return us to normal 61 period */
              
              /* Restart the serial shell now that transmission is complete */
              if(!process_is_running(&serial_shell_process)) {
                process_start(&serial_shell_process, NULL);
              }
            }
          }
        } else {
          /* If it's a regular shell command and the shell was stopped (e.g. from an aborted update), restart it */
          if(!process_is_running(&serial_shell_process)) {
            process_start(&serial_shell_process, NULL);
            /* Post the event back to the shell so it processes the current command */
            process_post(&serial_shell_process, serial_line_event_message, data);
          }
        }
      } else if(ev == PROCESS_EVENT_TIMER && data == &ota_timeout_timer) {
        LOG_INFO("[OTA] Timeout reached on coordinator, dropping back to low-power\n");
        set_shared_period(61);
      }
    }
  }

  /* Sensor-node: keep the process alive and handle timeout */
  while(1) {
    PROCESS_WAIT_EVENT();
    if(ev == PROCESS_EVENT_TIMER && data == &ota_timeout_timer) {
      LOG_INFO("[OTA] Timeout reached on sensor-node, dropping back to low-power\n");
      set_shared_period(61);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
