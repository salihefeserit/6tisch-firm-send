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
 *     - node_id == 1  → TSCH coordinator + RPL DAG Root + UDP client
 *                        (It periodically sends "Hello World!" to every
 *                         sensor node listed in the RPL routing table)
 *     - node_id != 1  → 6TiSCH sensor-node + UDP server
 *                        (It listens on UDP_PORT and logs every message
 *                         received from the coordinator)
 *
 * NOTE: Requires RPL storing mode (MAKE_WITH_STORING_ROUTING=1) so that
 *       the coordinator populates its routing table with sensor-node routes.
 *
 * \author Simon Duquennoy <simonduq@sics.se>
 *         (Coordinator-to-node periodic UDP messaging)
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
 * UDP receive callback — runs on the sensor-node side.
 * Logs every message received from the coordinator.
 */
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  LOG_INFO("Message received [%u bytes]: %.*s\n", datalen, datalen,
           (char *)data);
  if(datalen == strlen(MSG_PAYLOAD) &&
     memcmp(data, MSG_PAYLOAD, datalen) == 0) {
    LOG_INFO("Hello World! received from coordinator\n");
  }
  LOG_INFO("Sender: ");
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");
}

/*---------------------------------------------------------------------------*/
/*
 * Returns true when the coordinator's TSCH is associated and the RPL
 * routing table contains at least one sensor-node entry.
 * (Requires RPL storing mode so uip_ds6_route_head() is populated.)
 */
static int
coordinator_has_routes(void)
{
  return tsch_is_associated &&
         (uip_ds6_route_head() != NULL);
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(node_process, ev, data)
{
  static struct etimer send_timer;
  static uint32_t seq = 0;
  static char buf[64];
  uip_ds6_route_t *route;

  PROCESS_BEGIN();

  /* Simple Energest Monitoring */
  simple_energest_init();

  /* ------------------------------------------------------------------
   * Device role determination:
   *   node_id == 1  →  Coordinator (TSCH coordinator + RPL root + UDP client)
   *                    Built with NODEID=1 → uses software-assigned address.
   *   node_id != 1  →  Sensor-node (UDP server)
   *                    Built WITHOUT NODEID → each device uses its unique
   *                    factory-programmed IEEE EUI-64 hardware address.
   *                    node_id is derived from the last 2 bytes of the MAC
   *                    address at runtime, so it will never equal 1
   *                    (unless the hardware MAC ends in 0x0001, which is
   *                    extremely unlikely for production devices).
   * ------------------------------------------------------------------ */
  if(node_id == 1) {
    LOG_INFO("This device is COORDINATOR (node_id=1).\n");
    /* Tell TSCH that this device is the coordinator — before MAC starts */
    NETSTACK_ROUTING.root_start();
  } else {
    LOG_INFO("This device is SENSOR-NODE (node_id=%u). "
             "Waiting for coordinator...\n", node_id);
  }

  /* Start TSCH MAC layer */
  NETSTACK_MAC.on();

  /* Register UDP socket (RX callback active on sensor-nodes only) */
  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_rx_callback);

  /* ------------------------------------------------------------------
   * COORDINATOR: periodically send "Hello World!" to every known
   *              sensor-node found in the RPL routing table.
   *
   * SENSOR-NODE: idle — the udp_rx_callback handles incoming messages.
   *
   * NOTE: RPL storing mode must be enabled (MAKE_WITH_STORING_ROUTING=1)
   *       so the coordinator builds a per-node routing table.
   * ------------------------------------------------------------------ */
  if(node_id == 1) {
    LOG_INFO("Waiting for TSCH association and first sensor-node route...\n");

    /* Wait until at least one sensor-node has joined */
    while(!coordinator_has_routes()) {
      PROCESS_PAUSE();
    }

    LOG_INFO("First sensor-node route detected. Starting periodic send.\n");
    etimer_set(&send_timer, SEND_INTERVAL);

    while(1) {
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));
      etimer_reset(&send_timer);

      if(!tsch_is_associated) {
        LOG_INFO("TSCH not associated, send skipped.\n");
        continue;
      }

      seq++;
      snprintf(buf, sizeof(buf), "%s", MSG_PAYLOAD);

      /* Iterate over every route and unicast to each sensor-node */
      for(route = uip_ds6_route_head();
          route != NULL;
          route = uip_ds6_route_next(route)) {

        LOG_INFO("Sending to node -> ");
        LOG_INFO_6ADDR(&route->ipaddr);
        LOG_INFO_(" : \"%s\" [%lu]\n", buf, (unsigned long)seq);

        simple_udp_sendto(&udp_conn, buf, strlen(buf), &route->ipaddr);
      }
    }
  }

  /* Sensor-node: just keep the process alive — udp_rx_callback does the work */
  while(1) {
    PROCESS_WAIT_EVENT();
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
