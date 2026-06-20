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

/*---------------------------------------------------------------------------*/
PROCESS(node_process, "6TiSCH Node");
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
            }
          }
          else if(str[3] == 'D') {
            if(sscanf(str, "FW:D:%lx:%x:%128s", &offset, &length, hexdata) >= 3) {
              set_shared_period(11); /* Switch to fast period on data transmission */
              fw_packet_t pkt;
              pkt.type = PKT_TYPE_DATA;
              pkt.offset = offset;
              pkt.length = length;
              
              for(int i = 0; i < length; i++) {
                pkt.data[i] = (hex2val(hexdata[i*2]) << 4) | hex2val(hexdata[i*2+1]);
              }
              
              send_to_all(&pkt, 7 + length);
              LOG_INFO("Sent DATA offset 0x%05lx len %u\n", (unsigned long)offset, length);
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
