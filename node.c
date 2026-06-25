/*
 * 6TiSCH dual-role node:
 *   - node_id == 1  → Coordinator (Receives OTA over UART, sends over UDP)
 *   - node_id != 1  → Sensor-node (Receives OTA over UDP, writes to Flash)
 */

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
#include "sys/log.h"
#include "dev/serial-line.h"
#include "ota.h"

PROCESS(node_process, "6TiSCH Node");
AUTOSTART_PROCESSES(&node_process);

/* Main node initialization and bootstrapper process */
PROCESS_THREAD(node_process, ev, data) {
  PROCESS_BEGIN();

  /* Allocate process event */
  event_bitmap_received = process_alloc_event();

  /* Simple Energest Monitoring */
  simple_energest_init();


  if (node_id == 1) {
    LOG_INFO("This device is COORDINATOR (node_id=1).\n");
    tsch_set_coordinator(1);
    NETSTACK_ROUTING.root_start();
  } else {
    LOG_INFO(
        "This device is SENSOR-NODE (node_id=%u). Waiting for coordinator...\n",
        node_id);
  }

  /* Start TSCH MAC layer */
  NETSTACK_MAC.on();

  /* Register UDP socket with sensor-node UDP callback */
  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_rx_callback);

  if (node_id == 1) {
    LOG_INFO("Waiting for TSCH association and first sensor-node route...\n");

    static struct etimer route_timer;
    etimer_set(&route_timer, CLOCK_SECOND);
    while (!ota_coordinator_has_routes()) {
      PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER && data == &route_timer);
      etimer_reset(&route_timer);
    }

    LOG_INFO("Routes detected. Ready to receive firmware via UART.\n");

    while (1) {
      PROCESS_WAIT_EVENT();
      if (ev == serial_line_event_message && data != NULL) {
        char *str = (char *)data;

        if (strncmp(str, "FW:", 3) == 0) {
          /* Delegate UART commands to the coordinator module */
          ota_coordinator_handle_uart(str);
        } else {
          /* If it's a regular shell command and the shell was stopped (e.g.
           * from an aborted update), restart it */
          extern struct process serial_shell_process;
          if (!process_is_running(&serial_shell_process)) {
            process_start(&serial_shell_process, NULL);
            /* Post the event back to the shell so it processes the current
             * command */
            process_post(&serial_shell_process, serial_line_event_message,
                         data);
          }
        }
      }
    }
  }

  /* Sensor-node: keep the process alive */
  while (1) {
    PROCESS_WAIT_EVENT();
  }

  PROCESS_END();
}
