# 6TiSCH Sensor-Node to Hardware (sn-to-hw)

A Contiki-NG example demonstrating a dual-role 6TiSCH network running on the
**Texas Instruments CC1352R1** LaunchPad (SimpleLink platform). One device acts
as the TSCH coordinator and RPL DAG root; every other device is a sensor node
that periodically sends a UDP message to the coordinator once the network is
ready.

---

## Table of Contents

- [Overview](#overview)
- [Network Architecture](#network-architecture)
- [How It Works](#how-it-works)
- [Hardware Requirements](#hardware-requirements)
- [Prerequisites](#prerequisites)
- [Project Structure](#project-structure)
- [Key Configuration Parameters](#key-configuration-parameters)
- [Building](#building)
  - [Coordinator (node\_id = 1)](#coordinator-node_id--1)
  - [Sensor Node (node\_id ≥ 2)](#sensor-node-node_id--2)
- [Flashing](#flashing)
- [Simulation with Cooja](#simulation-with-cooja)
- [Expected Log Output](#expected-log-output)
- [Timing Behaviour](#timing-behaviour)
- [Optional Build Flags](#optional-build-flags)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Overview

| Role | node\_id | Stack role | UDP role |
|------|----------|------------|----------|
| **Coordinator** | `1` | TSCH coordinator + RPL DAG root | UDP server (receives messages) |
| **Sensor Node** | `≥ 2` | TSCH node + RPL leaf | UDP client (sends messages) |

The project is self-contained: **a single firmware binary** determines its own
role at runtime using `node_id`. No separate coordinator firmware is needed.

---

## Network Architecture

```
┌──────────────────────────────────────────────────┐
│               6TiSCH Network (PANID 0x81a5)      │
│                                                  │
│   ┌─────────────┐        ┌─────────────────┐     │
│   │  Sensor     │──UDP──▶│  Coordinator    │     │
│   │  Node #2    │        │  (node_id = 1)  │     │
│   └─────────────┘        │  RPL DAG Root   │     │
│                           │  UDP Server     │     │
│   ┌─────────────┐        └─────────────────┘     │
│   │  Sensor     │──UDP──▶                        │
│   │  Node #3    │                                │
│   └─────────────┘                                │
│            ...                                   │
└──────────────────────────────────────────────────┘
```

- **MAC layer**: IEEE 802.15.4e TSCH (Time-Slotted Channel Hopping)
- **Routing**: RPL (non-storing mode, default)
- **Transport**: UDP over IPv6 / 6LoWPAN
- **RF band**: 2.4 GHz, Channel 26
- **Hopping sequence**: single channel (`TSCH_HOPPING_SEQUENCE_1_1`) — suitable
  for demos and Cooja simulation

---

## How It Works

### Coordinator (node\_id = 1)
1. Calls `NETSTACK_ROUTING.root_start()` to become the RPL DAG root.
2. Starts the TSCH MAC layer (`NETSTACK_MAC.on()`).
3. Registers a UDP socket on port **8765** and listens for incoming messages.
4. Logs every received "Hello World!" payload along with the sender's IPv6
   address.

### Sensor Node (node\_id ≥ 2)
1. Starts the TSCH MAC layer and waits for TSCH association (beacon from the
   coordinator).
2. Polls `network_ready_to_send_to_root()` in a tight loop (using
   `PROCESS_PAUSE()`) until:
   - TSCH is associated (`tsch_is_associated == true`), **and**
   - The RPL DODAG is reachable (`node_is_reachable()`), **and**
   - The root's IPv6 address is known (`get_root_ipaddr()`).
3. Once network-ready, starts a **10-second periodic timer** and sends
   `"Hello World!"` to the coordinator on every tick.
4. If connectivity is lost between ticks, the send is deferred and a warning is
   logged.

---

## Hardware Requirements

| Item | Details |
|------|---------|
| Development board | Texas Instruments **CC1352R1** LaunchPad or LPSTK SensorTag |
| Platform in Contiki-NG | `simplelink` |
| Board identifier | `sensortag/cc1352r1` |
| RF frequency | **2.4 GHz** (IEEE 802.15.4, channels 11–26) |
| Minimum devices | 2 (1 coordinator + 1 sensor node) |

> **Note:** The `sky`, `native`, and `z1` platforms are explicitly excluded
> from this project. Only the SimpleLink family is supported.

---

## Prerequisites

- **Contiki-NG** repository cloned (this example lives inside it at
  `examples/6tisch/sn-to-hw/`)
- **ARM GCC toolchain** (`arm-none-eabi-gcc`) installed and on `$PATH`
- **TI SimpleLink CC13xx/CC26xx SDK** installed; SDK root path exported:
  ```bash
  export SIMPLELINK_CC13XX_CC26XX_SDK_INSTALL_DIR=/path/to/simplelink_cc13xx_cc26xx_sdk_x_xx_xx_xx
  ```
- **Uniflash** or `cc2538-bsl` for flashing (optional — only needed for
  physical hardware)
- **Cooja** (bundled with Contiki-NG) for simulation

---

## Project Structure

```
sn-to-hw/
├── node.c              # Main application — dual-role logic
├── project-conf.h      # Project-level compile-time configuration
├── Makefile            # Build system; convenience targets for each role
├── rpl-tsch-cooja.csc  # Cooja simulation scenario (2 nodes pre-configured)
└── README.md           # This file
```

---

## Key Configuration Parameters

All tunable parameters live in [`project-conf.h`](project-conf.h).

### RF / PHY

| Macro | Value | Description |
|-------|-------|-------------|
| `RF_CONF_MODE` | `RF_MODE_2_4_GHZ` | Force 2.4 GHz IEEE mode on CC1352R1 |
| `DOT_15_4G_CONF_FREQ_BAND_ID` | `DOT_15_4G_FREQ_BAND_2450` | 2.4 GHz band |
| `IEEE802154_CONF_DEFAULT_CHANNEL` | `26` | Default RF channel (11–26) |

### TSCH

| Macro | Value | Description |
|-------|-------|-------------|
| `TSCH_CONF_DEFAULT_TIMESLOT_TIMING` | `tsch_timeslot_timing_us_10000` | Standard 10 ms timeslot |
| `TSCH_CONF_ARCH_HDR_PATH` | `"net/mac/tsch/tsch.h"` | Suppresses platform 50 kbps timing override |
| `IEEE802154_CONF_PANID` | `0x81a5` | Personal Area Network ID |
| `TSCH_CONF_AUTOSTART` | `0` | TSCH is started manually via `NETSTACK_MAC.on()` |
| `TSCH_SCHEDULE_CONF_DEFAULT_LENGTH` | `3` | Minimal 6TiSCH schedule slots |
| `TSCH_CONF_DEFAULT_HOPPING_SEQUENCE` | `TSCH_HOPPING_SEQUENCE_1_1` | Single-channel hopping (ch 20) |
| `TSCH_CONF_EB_PERIOD` | `2 * CLOCK_SECOND` | Enhanced Beacon interval — fast sync |
| `TSCH_CONF_MAX_EB_PERIOD` | `4 * CLOCK_SECOND` | Maximum EB back-off period |

### Application (node.c)

| Macro | Value | Description |
|-------|-------|-------------|
| `UDP_PORT` | `8765` | UDP port for coordinator ↔ sensor communication |
| `SEND_INTERVAL` | `10 * CLOCK_SECOND` | How often the sensor node sends a message |
| `MSG_PAYLOAD` | `"Hello World!"` | Payload content of each UDP packet |

### Logging

| Macro | Level | Note |
|-------|-------|------|
| `LOG_CONF_LEVEL_RPL` | `WARN` | Suppress verbose RPL output |
| `LOG_CONF_LEVEL_MAC` | `INFO` | Show TSCH association events |
| `TSCH_LOG_CONF_PER_SLOT` | `1` | Enable per-slot TSCH logging |
| `LOG_CONF_LEVEL_FRAMER` | *(not set)* | Intentionally omitted — printing from interrupt context causes hard faults on SimpleLink |

---

## Building

The default `TARGET` is `simplelink` and the default `BOARD` is
`sensortag/cc1352r1`. Override these on the command line if needed.

### Coordinator (node\_id = 1)

```bash
make coordinator
# Produces: coordinator.bin
```

This runs `distclean` first, then builds with `NODEID=1` and copies the
resulting ELF to `coordinator.bin`.

### Sensor Node (node\_id ≥ 2)

```bash
make sensor-node
# Produces: sensor-node.bin
```

This runs `distclean` first, then builds with `NODEID=2` and copies the
resulting ELF to `sensor-node.bin`.

### Generic build (choose node ID manually)

```bash
make NODEID=3 TARGET=simplelink BOARD=sensortag/cc1352r1
```

### Build with optional features

```bash
# Enable link-layer security (802.15.4 AES-128 CCM*)
make coordinator MAKE_WITH_SECURITY=1

# Enable Orchestra adaptive TSCH scheduling
make coordinator MAKE_WITH_ORCHESTRA=1
```

---

## Flashing

Flash coordinator firmware to the first board and sensor-node firmware to all
remaining boards using **TI Uniflash** or the `cc2538-bsl` script:

```bash
# Example using cc2538-bsl (adjust serial port as needed)
python3 cc2538-bsl.py -e -w -v -p /dev/tty.usbmodem* coordinator.bin
python3 cc2538-bsl.py -e -w -v -p /dev/tty.usbmodem* sensor-node.bin
```

---

## Simulation with Cooja

A pre-configured Cooja scenario is included:

```bash
# From the Contiki-NG root:
cd tools/cooja
./gradlew run --args="--contiki=../.. \
  --logdir=/tmp/cooja-logs \
  ../../examples/6tisch/sn-to-hw/rpl-tsch-cooja.csc"
```

The scenario contains 2 motes:
- Mote 1 → Coordinator (`node_id = 1`)
- Mote 2 → Sensor Node (`node_id = 2`)

---

## Expected Log Output

### Coordinator

```
[INFO: App      ] This device is COORDINATOR (node_id=1).
[INFO: MAC      ] association done (PAN ID 0x81a5)
[INFO: App      ] Message received [12 bytes]: Hello World!
[INFO: App      ] Hello World! received
[INFO: App      ] Sender: fe80::202:2:2:2
```

### Sensor Node

```
[INFO: App      ] This device is SENSOR-NODE (node_id=2). Connecting to coordinator...
[INFO: App      ] Waiting for TSCH synchronization and RPL DODAG...
[INFO: MAC      ] association done (PAN ID 0x81a5)
[INFO: App      ] Coordinator root address found: fd00::201:1:1:1
[INFO: App      ] Sending to coordinator -> fd00::201:1:1:1 : "Hello World!" [1]
[INFO: App      ] Sending to coordinator -> fd00::201:1:1:1 : "Hello World!" [2]
```

---

## Timing Behaviour

Understanding the startup sequence is important for debugging:

```
t=0s   Node boots, TSCH MAC starts
t≈2s   Enhanced Beacon received from coordinator → TSCH association
t≈4s   RPL DODAG discovered, root IPv6 address known
         → "Coordinator root address found" logged
t≈4s   10-second send timer is armed (etimer_set)
t≈14s  First "Hello World!" sent to coordinator
t≈24s  Second "Hello World!" sent ...  (every 10 s thereafter)
```

> **Why the 10-second gap before the first message?**
>
> The timer is set immediately after the root address is discovered, so the
> first message is sent exactly one `SEND_INTERVAL` (10 s) later. This is by
> design: it gives the RPL routes time to fully stabilise before application
> traffic starts. To send immediately on first discovery, add a
> `simple_udp_sendto()` call before `etimer_set()`.

---

## Optional Build Flags

| Flag | Default | Description |
|------|---------|-------------|
| `MAKE_WITH_ORCHESTRA` | `0` | Enable Orchestra adaptive scheduling |
| `MAKE_WITH_SECURITY` | `0` | Enable IEEE 802.15.4 link-layer security |
| `MAKE_WITH_STORING_ROUTING` | `0` | Switch RPL to storing mode |
| `MAKE_WITH_LINK_BASED_ORCHESTRA` | `0` | Use link-based Orchestra rule (requires storing mode) |
| `MAKE_WITH_ORCHESTRA_ROOT_RULE` | `0` | Add a dedicated root scheduling rule |
| `MAKE_WITH_PERIODIC_ROUTES_PRINT` | `0` | Periodically print routing table (regression tests) |

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Sensor node never associates | RF channel mismatch | Ensure both boards use the same `IEEE802154_CONF_DEFAULT_CHANNEL` |
| Build fails with `tsch-arch.h` errors | Platform timing header conflict | Verify `TSCH_CONF_ARCH_HDR_PATH` is set to `"net/mac/tsch/tsch.h"` |
| Hard fault / crash on coordinator | `LOG_CONF_LEVEL_FRAMER` defined | Remove `FRAMER` log level — it triggers interrupt-context printing on SimpleLink |
| No messages received by coordinator | RPL not converged | Wait longer; check `LOG_CONF_LEVEL_RPL` is at least `WARN` to see routing events |
| `Binary not found` after `make coordinator` | Build error silently failed | Run plain `make` first to see full compiler output |

---

## License

This project is derived from the Contiki-NG 6TiSCH example by
[Simon Duquennoy](mailto:simonduq@sics.se) (SICS Swedish ICT) and is
distributed under the **3-Clause BSD License**. See the file headers for the
full license text.
