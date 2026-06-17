/*
 * Copyright (c) 2015, SICS Swedish ICT.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/**
 * \file
 *   6TiSCH dual-role node:
 *     - node_id == 1  → TSCH coordinator + RPL DAG Root + UDP server
 *                        (It receives the "Hello World!" message from the
 * sensor nodes)
 *     - node_id != 1  → 6TiSCH sensor-node + UDP client
 *                        (It sends the "Hello World!" message to the
 * coordinator when TSCH and RPL are ready)
 *
 * \author Simon Duquennoy <simonduq@sics.se>
 *         (Periodic UDP messaging added)
 */

#include "contiki.h"
#include "net/ipv6/simple-udp.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-ds6.h"
#include "net/mac/tsch/tsch.h"
#include "net/netstack.h"
#include "net/routing/routing.h"
#include "sys/log.h"
#include "sys/node-id.h"
#include <string.h>

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

/* UDP port for communication */
#define UDP_PORT 8765

/* Sending interval (seconds) */
#define SEND_INTERVAL (10 * CLOCK_SECOND)

/* Message to send */
#define MSG_PAYLOAD "Hello World!"

/*---------------------------------------------------------------------------*/
PROCESS(node_process, "6TiSCH Node");
AUTOSTART_PROCESSES(&node_process);

/*---------------------------------------------------------------------------*/
/* UDP socket — common for both server and client. */
static struct simple_udp_connection udp_conn;

/*---------------------------------------------------------------------------*/
/*
 * UDP receive callback — works on the coordinator side.
 * It logs the received message and the sender's address.
 */
static void udp_rx_callback(struct simple_udp_connection *c,
                            const uip_ipaddr_t *sender_addr,
                            uint16_t sender_port,
                            const uip_ipaddr_t *receiver_addr,
                            uint16_t receiver_port, const uint8_t *data,
                            uint16_t datalen) {
  LOG_INFO("Message received [%u bytes]: %.*s\n", datalen, datalen,
           (char *)data);
  if (datalen == strlen(MSG_PAYLOAD) &&
      memcmp(data, MSG_PAYLOAD, datalen) == 0) {
    LOG_INFO("Hello World! received\n");
  }
  LOG_INFO("Sender: ");
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");
}

/*---------------------------------------------------------------------------*/
/*
 * Two conditions are required for the sensor-node side to send application
 * data: TSCH association, then RPL DODAG/root address.
 */
static int network_ready_to_send_to_root(uip_ipaddr_t *root_addr) {
  return tsch_is_associated && NETSTACK_ROUTING.node_is_reachable() &&
         NETSTACK_ROUTING.get_root_ipaddr(root_addr);
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(node_process, ev, data) {
  static struct etimer send_timer;
  static uint32_t seq = 0;
  static char buf[64];
  uip_ipaddr_t dest_ipaddr;

  PROCESS_BEGIN();

  /* ------------------------------------------------------------------
   * Device role determination:
   *   node_id == 1  →  Coordinator (TSCH coordinator + RPL root + UDP server)
   *   node_id != 1  →  Normal 6TiSCH sensor-node (UDP client)
   *
   * ------------------------------------------------------------------ */
  if (node_id == 1) {
    LOG_INFO("This device is COORDINATOR (node_id=1).\n");
    /* Tell TSCH that this device is the coordinator — before MAC starts */
    NETSTACK_ROUTING.root_start();
  } else {
    LOG_INFO("This device is SENSOR-NODE (node_id=%u). Connecting to "
             "coordinator...\n",
             node_id);
  }

  /* Start TSCH MAC layer */
  NETSTACK_MAC.on();

  /* Register UDP socket */
  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_rx_callback);

  /* ------------------------------------------------------------------
   * The sensor-node periodically sends a message to the coordinator.
   * The coordinator logs only the message received via UDP receive callback.
   * ------------------------------------------------------------------ */
  if (node_id != 1) {
    LOG_INFO("Waiting for TSCH synchronization and RPL DODAG...\n");
    while (!network_ready_to_send_to_root(&dest_ipaddr)) {
      PROCESS_PAUSE();
    }

    LOG_INFO("Coordinator root address found: ");
    LOG_INFO_6ADDR(&dest_ipaddr);
    LOG_INFO_("\n");

    etimer_set(&send_timer, SEND_INTERVAL);

    while (1) {
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));
      etimer_reset(&send_timer);

      if (network_ready_to_send_to_root(&dest_ipaddr)) {

        seq++;
        snprintf(buf, sizeof(buf), "%s", MSG_PAYLOAD);

        LOG_INFO("Sending to coordinator -> ");
        LOG_INFO_6ADDR(&dest_ipaddr);
        LOG_INFO_(" : \"%s\" [%lu]\n", buf, (unsigned long)seq);

        simple_udp_sendto(&udp_conn, buf, strlen(buf), &dest_ipaddr);

      } else {
        LOG_INFO("TSCH/RPL not ready, sending delayed...\n");
      }
    }
  }

  while (1) {
    PROCESS_WAIT_EVENT();
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
