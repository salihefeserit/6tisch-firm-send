# 6TiSCH OTA Firmware Sender

This project implements OTA firmware distribution over a Contiki-NG 6TiSCH/RPL
network. A coordinator receives the firmware image over UART, distributes it to
sensor nodes over UDP, and runs per-page recovery using bitmap reports from the
nodes.

The project supports two BIM/OAD application layouts:

- **dual-onchip BIM**: the new image is written to the inactive internal flash
  slot. The target is selected as slot `A` or slot `B`.
- **offchip BIM**: the new image is written to external flash. The target is
  selected as `offchip`.

The coordinator is architecture-neutral. The UART sender declares the intended
target; the coordinator forwards that target in START/DATA/PAGE_END/VERIFY
packets. Image compatibility, secVer policy, and target-slot admission are
decided by each sensor node during START admission.

---

## Contents

- [Architecture](#architecture)
- [OTA Flow](#ota-flow)
- [Project Layout](#project-layout)
- [Requirements](#requirements)
- [Building](#building)
- [Initial Flashing](#initial-flashing)
- [Sending OTA Images over UART](#sending-ota-images-over-uart)
- [Protocol Summary](#protocol-summary)
- [Admission and Rejection Rules](#admission-and-rejection-rules)
- [Important Configuration](#important-configuration)
- [Logs and Verification](#logs-and-verification)
- [Troubleshooting](#troubleshooting)

---

## Architecture

The network has two runtime roles:

| Role | Selection | Responsibility |
|------|-----------|----------------|
| Coordinator | Built with `NODEID=1` | Starts the RPL DAG root, receives firmware over UART, distributes it to sensor nodes |
| Sensor node | Built without `NODEID` | Uses its hardware EUI-64 address, receives OTA packets, writes the image to the selected flash backend |

The codebase is split along these boundaries:

| Layer | Files | Responsibility |
|-------|-------|----------------|
| Application bootstrap | `node.c` | Starts the coordinator or sensor-node role and registers the UDP socket |
| Shared protocol | `ota.h` | Packet types, target constants, status codes, public API |
| Shared transfer state | `ota-common.c` | Page buffer, file offsets, UDP send helper |
| Common coordinator | `ota-coordinator.c` | Handles UART commands, distributes pages, retransmits missing chunks |
| Coordinator session/admission | `ota-coordinator-session.c` | Builds sessions from routes and collects START reports |
| Common sensor-node OTA flow | `ota-sensor.c` | START/DATA/PAGE_END/VERIFY state machine, forwarding, bitmap reports |
| Sensor backend API | `ota-sensor-backend.h` | Storage, validation, and staging interface used by `ota-sensor.c` |
| Dual-onchip sensor backend | `sensor-node-dual-onchip.c` | Internal flash slot writes, metadata staging, stable boot confirmation |
| Offchip sensor backend | `sensor-node-offchip.c` | External flash writes, off-chip OAD header validation, reset staging |
| Dual-onchip flash helpers | `ota-flash.c/.h`, `ota-metadata.c/.h` | Internal flash and BIM metadata operations |
| OAD header integration | `oad_image_header_app.c/.h` | TI OAD image header symbols |

The old `coordinator-dual-onchip.c` and `coordinator-offchip.c` split has been
removed. Both BIM layouts now use the same coordinator implementation. The
architecture-specific code lives in the sensor-node backend and linker scripts.

---

## OTA Flow

1. The coordinator boots with `NODEID=1`, becomes the TSCH coordinator, and
   starts the RPL root.
2. Sensor nodes join the TSCH network and become visible in the coordinator's
   RPL routing table.
3. `uart_sender.py` sends an `FW:S` START command to the coordinator serial
   port.
4. The coordinator sends a START packet to the routed nodes.
5. Each sensor node checks the target and image secVer:
   - if acceptable, it replies with an accepted `PKT_TYPE_START_REPORT`;
   - otherwise, it replies with a rejection status.
6. The coordinator keeps only accepted nodes in the active OTA session.
7. The script splits the firmware into 4096-byte pages and sends each page to
   the coordinator as 32-byte UART chunks.
8. The coordinator distributes each page as 64-byte UDP DATA packets.
9. At the end of each page, sensor nodes send bitmap reports. If chunks are
   missing, the coordinator retransmits only the missing chunks.
10. After all pages are sent, the script sends `FW:V:<crc>` to trigger image
    verification.
11. Sensor nodes validate the image and perform the architecture-specific
    staging/reset operation.

---

## Project Layout

```text
6tisch-firm-send/
|-- node.c                       # Role bootstrap and Contiki process wiring
|-- ota.h                        # OTA protocol, constants, and public API
|-- ota-common.c                 # Shared transfer state and send_to_all()
|-- ota-coordinator.c            # Architecture-neutral coordinator
|-- ota-coordinator-session.c    # Coordinator session/admission helpers
|-- ota-sensor.c                 # Common sensor-node OTA state machine
|-- ota-sensor-backend.h         # Sensor backend interface
|-- sensor-node-dual-onchip.c    # Dual-onchip sensor-node OTA backend
|-- sensor-node-offchip.c        # Offchip sensor-node OTA backend
|-- sensor-node-legacy.c         # Legacy non-BIM sensor path
|-- coordinator-legacy.c         # Legacy non-BIM coordinator path
|-- ota-flash.c/.h               # Dual-onchip internal flash operations
|-- ota-metadata.c/.h            # Dual-onchip BIM metadata operations
|-- oad_image_header_app.c/.h    # TI OAD image header integration
|-- slot-a.ld                    # Dual-onchip Slot A linker script
|-- slot-b.ld                    # Dual-onchip Slot B linker script
|-- offchip-oad.ld               # Offchip linker script
|-- uart_sender.py               # UART OTA sender
|-- project-conf.h               # 6TiSCH, RPL, TSCH, and platform settings
|-- Makefile                     # Build targets
`-- rpl-tsch-cooja.csc           # Cooja scenario
```

---

## Requirements

- Contiki-NG source tree with this example directory
- `arm-none-eabi-gcc` toolchain
- GNU Make (`gmake`)
- TI SimpleLink CC13xx/CC26xx SDK
- TI OAD image tool and private key:
  - default SDK path: `$(HOME)/ti/simplelink_cc13xx_cc26xx_sdk_8_32_00_07`
  - override with `TI_FULL_SDK_DIR=/path/to/sdk`
- Python 3
- `pyserial` for the UART sender
- TI UniFlash, `cc2538-bsl`, or your preferred flashing tool

Default platform:

```make
TARGET ?= simplelink
BOARD  ?= sensortag/cc1352r1
```

---

## Building

Use the convenience targets below. They pass the required `MAKE_WITH_BIM_*`,
linker script, and `NODEID` parameters internally, so normal builds do not need
those flags on the command line.

### Dual-onchip Sensor-Node Images

```sh
gmake sensor-node-slot-a APP_SEC_VER=1
gmake sensor-node-slot-b APP_SEC_VER=2
```

Outputs:

- `sensor-node-slot-a.elf`
- `sensor-node-slot-a-raw.bin`
- `sensor-node-slot-a.hex`
- `sensor-node-slot-a.bin`
- `sensor-node-slot-b.elf`
- `sensor-node-slot-b-raw.bin`
- `sensor-node-slot-b.hex`
- `sensor-node-slot-b.bin`

The Slot A build also creates `sensor-node.bin` and `sensor-node.hex` copies for
initial flashing convenience.

### Offchip Sensor-Node Image

```sh
gmake sensor-node-offchip APP_SEC_VER=3
```

Outputs:

- `sensor-node-offchip.elf`
- `sensor-node-offchip-raw.bin`
- `sensor-node-offchip.hex`
- `sensor-node-offchip.bin`

### Dual-onchip Coordinator Images

```sh
gmake coordinator-slot-a APP_SEC_VER=4
gmake coordinator-slot-b APP_SEC_VER=5
```

Use these when the coordinator itself is running under the dual-onchip BIM
layout. The coordinator still does not interpret the target architecture of the
image it forwards.

### Offchip Coordinator Image

```sh
gmake coordinator-offchip APP_SEC_VER=6
```

Outputs:

- `coordinator-offchip.elf`
- `coordinator-offchip-raw.bin`
- `coordinator-offchip.hex`
- `coordinator-offchip.bin`

### Legacy Non-BIM Targets

The BIM targets above are the primary OTA build path. The old non-BIM demo path
is kept for compatibility:

```sh
gmake coordinator
gmake sensor-node
```

---

## Initial Flashing

Initial flashing depends on the BIM layout.

### Dual-onchip

1. Flash the dual-onchip BIM.
2. Build and flash the Slot A coordinator application:

   ```sh
   gmake coordinator-slot-a APP_SEC_VER=1
   ```

3. Build and flash the Slot A sensor-node application:

   ```sh
   gmake sensor-node-slot-a APP_SEC_VER=1
   ```

4. For later OTA updates, target the inactive slot. A typical first update for
   a node currently running Slot A is a Slot B image.

### Offchip

1. Flash the offchip BIM.
2. Build and flash the offchip coordinator application:

   ```sh
   gmake coordinator-offchip APP_SEC_VER=1
   ```

3. Build and flash the offchip sensor-node application:

   ```sh
   gmake sensor-node-offchip APP_SEC_VER=1
   ```

In the offchip flow, the application runs from internal flash, the OTA
candidate is written to external flash, and BIM evaluates the candidate after
reset.

---

## Sending OTA Images over UART

Usage:

```sh
python3 uart_sender.py <serial_port> <firmware.bin> <image_sec_ver> [target]
```

Parameters:

| Parameter | Description |
|-----------|-------------|
| `<serial_port>` | Coordinator serial port |
| `<firmware.bin>` | OAD binary to send |
| `<image_sec_ver>` | Security version of the image being sent |
| `[target]` | `A`, `B`, or `offchip`. Defaults to `offchip` if omitted |

Examples:

```sh
python3 uart_sender.py /dev/tty.usbmodem00000001 sensor-node-slot-b.bin 2 B
python3 uart_sender.py /dev/tty.usbmodem00000001 sensor-node-slot-a.bin 3 A
python3 uart_sender.py /dev/tty.usbmodem00000001 sensor-node-offchip.bin 4 offchip
```

`image_sec_ver` is required for all architectures. It is also required for
dual-onchip images so sensor nodes can reject lower or equal secVer images
during START admission, before DATA transfer begins.

The script clears `coordinator.log` at the start of each session and appends the
serial logs it reads from the coordinator.

---

## Protocol Summary

UART commands between `uart_sender.py` and the coordinator:

| Command | Direction | Meaning |
|---------|-----------|---------|
| `FW:S:<size>:<secVer>:<target>` | Script -> Coordinator | Start a new OTA session |
| `FW:D:<offset>:<len>:<hex>` | Script -> Coordinator | Write a firmware chunk into the coordinator page buffer |
| `FW:V:<crc16>` | Script -> Coordinator | Verify the full image |
| `FW:ACK` | Coordinator -> Script | Command accepted, or START admission completed |
| `FW:NO_TARGETS` | Coordinator -> Script | No eligible target nodes remain |
| `FW:PAGE_OK` | Coordinator -> Script | Current page was distributed to all active nodes |

UDP packet types are defined in `ota.h`:

| Packet | Value | Purpose |
|--------|-------|---------|
| `PKT_TYPE_START` | `0` | Starts sensor-node admission checks |
| `PKT_TYPE_DATA` | `1` | Carries firmware data |
| `PKT_TYPE_VERIFY` | `2` | Triggers full-image CRC validation and staging |
| `PKT_TYPE_BITMAP_REPORT` | `3` | Sensor-node page bitmap report |
| `PKT_TYPE_PAGE_END` | `4` | Coordinator page-end report request |
| `PKT_TYPE_START_REPORT` | `5` | Sensor-node START accept/reject report |

Target constants:

| Target | Value |
|--------|-------|
| `OTA_SLOT_A` | `0` |
| `OTA_SLOT_B` | `1` |
| `OTA_TARGET_OFFCHIP` | `0x80` |

---

## Admission and Rejection Rules

The coordinator does not filter images by architecture. This is intentional:
the coordinator can forward `A`, `B`, or `offchip` targets regardless of its own
BIM layout.

Expected sensor-node behavior:

| Sensor-node backend | Accepted targets | Rejected targets |
|---------------------|------------------|------------------|
| `sensor-node-dual-onchip.c` | `A`, `B` | `offchip`, invalid target, active slot, lower/equal secVer |
| `sensor-node-offchip.c` | `offchip` | `A`, `B`, invalid target, lower/equal secVer |

START status codes:

| Status | Meaning |
|--------|---------|
| `OTA_START_STATUS_ACCEPTED` | Sensor node is ready to receive the image |
| `OTA_START_STATUS_REJECTED_VERSION` | Image secVer policy failed |
| `OTA_START_STATUS_REJECTED_TARGET` | Target is not valid for this sensor-node backend |
| `OTA_START_STATUS_REJECTED_SAME_SLOT` | Dual-onchip node refused to write to its active slot |

At the end of START admission, the coordinator removes rejected and silent nodes
from the active session. If no nodes remain, it returns `FW:NO_TARGETS` to the
UART script.

---

## Important Configuration

Main settings in `project-conf.h`:

| Setting | Value | Note |
|---------|-------|------|
| `RF_CONF_MODE` | `RF_MODE_2_4_GHZ` | 2.4 GHz IEEE mode for CC1352R1 |
| `IEEE802154_CONF_DEFAULT_CHANNEL` | `26` | Default 802.15.4 channel |
| `IEEE802154_CONF_PANID` | `0x81a5` | TSCH PAN ID |
| `TSCH_CONF_AUTOSTART` | `0` | TSCH is started with `NETSTACK_MAC.on()` |
| `TSCH_CONF_EB_PERIOD` | `2 * CLOCK_SECOND` | Faster association |
| `TSCH_CONF_MAX_EB_PERIOD` | `4 * CLOCK_SECOND` | EB backoff upper bound |
| `ORCHESTRA_CONF_UNICAST_PERIOD` | `7` | Faster unicast slots for OTA transfer |
| `TSCH_QUEUE_CONF_NUM_PER_NEIGHBOR` | `16` | Larger neighbor queue for OTA traffic |
| `QUEUEBUF_CONF_NUM` | `16` | Queuebuf capacity |

Important Makefile variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `APP_SEC_VER` | `1` | OAD image security version |
| `APP_IMG_NO` | `0` | OAD image number |
| `APP_V_MAJOR/MINOR/PATCH/BUILD` | `1.0.0.0` | OAD software version fields |
| `TI_FULL_SDK_DIR` | `$(HOME)/ti/simplelink_cc13xx_cc26xx_sdk_8_32_00_07` | TI SDK path |
| `OAD_IMAGE_TOOL` | SDK OAD tool path | TI OAD binary generation |
| `OAD_PRIVATE_KEY` | SDK `private.pem` path | OAD signing key |
| `OTA_EXPECTED_IMG_TYPE` | `OAD_IMG_TYPE_APP` | Offchip image type validation |
| `OTA_STAGE_RESET_AFTER_VERIFY` | `1` | Offchip reset staging after VERIFY |

---

## Logs and Verification

Typical coordinator readiness log:

```text
[INFO: App] This device is COORDINATOR (node_id=1).
[INFO: App] Waiting for TSCH association and first sensor-node route...
[INFO: App] Routes detected. Ready to receive firmware via UART.
```

START admission:

```text
[INFO: App] Session admission started for 1 routed nodes:
[INFO: App] Sent START, size: ..., secVer: ..., target: B
[INFO: App] START report from ...: status=0 imageSecVer=2 runningSecVer=1
[INFO: App] START admission complete: accepted=1 rejected=0 silent=0
```

Page distribution:

```text
[INFO: App] Distributing page at offset 0x00000, total chunks: ...
[INFO: App] Sent PAGE_END for offset 0x00000, waiting for bitmap...
[INFO: App] Page 0x00000 successfully completed on all active nodes!
FW:PAGE_OK
```

Rejection example:

```text
[INFO: App] START report from ...: status=2 imageSecVer=... runningSecVer=...
[INFO: App] START admission dropped rejected node: ...
FW:NO_TARGETS
```

Build verification commands:

```sh
python3 -m py_compile uart_sender.py
gmake sensor-node-slot-a APP_SEC_VER=1
gmake sensor-node-slot-b APP_SEC_VER=2
gmake sensor-node-offchip APP_SEC_VER=3
gmake coordinator-slot-a APP_SEC_VER=4
gmake coordinator-slot-b APP_SEC_VER=5
gmake coordinator-offchip APP_SEC_VER=6
```

---

## Troubleshooting

| Symptom | Likely cause | Check / fix |
|---------|--------------|-------------|
| Script receives `FW:NO_TARGETS` | No routed nodes, or all nodes rejected START | Check coordinator route logs and START status codes |
| Dual-onchip node appears to accept `offchip` | Old firmware is running, or wrong binary was flashed | Rebuild/flash `sensor-node-slot-a` or `sensor-node-slot-b` and check backend logs |
| Offchip node appears to accept `A`/`B` | Old offchip firmware without target checks may be running | Rebuild/flash `sensor-node-offchip` |
| START admission times out | Nodes are routed but do not reply | Check TSCH association, RPL routes, and `coordinator.log` |
| DATA chunk is rejected | DATA arrived before START admission finished, or no active targets remain | Ensure the script waits for `FW:ACK` after START |
| Page timeout | Bitmap reports are missing or packet loss is high | Check RSSI/distance, TSCH queues, and route stability |
| `secVer <= running secVer` appears in logs | The sent image is not newer | Increase `APP_SEC_VER` and rebuild |
| Same-slot rejection | Dual-onchip node refused to write to its active slot | If running Slot A, target `B`; if running Slot B, target `A` |
| Build fails around OAD image tool | SDK path or private key path is wrong | Check `TI_FULL_SDK_DIR`, `OAD_IMAGE_TOOL`, and `OAD_PRIVATE_KEY` |
| SimpleLink hard fault | Interrupt-context logging or platform log level issue | Keep `LOG_CONF_LEVEL_FRAMER` undefined |

---

## Notes

- Sensor-node builds intentionally do not pass `NODEID`. The same binary can be
  flashed to multiple physical nodes; each device uses its hardware EUI-64
  address.
- Coordinator builds pass `NODEID=1`. The RPL root startup decision in `node.c`
  depends on this value.
- `MAKE_WITH_BIM_DUAL_ONCHIP` and `MAKE_WITH_BIM_OFFCHIP` are mutually
  exclusive. The Makefile stops with an error if both are enabled.
- The primary OTA source files are `ota-coordinator.c`,
  `ota-coordinator-session.c`, `ota-sensor.c`,
  `sensor-node-dual-onchip.c`, and `sensor-node-offchip.c`.
  `coordinator-legacy.c` and `sensor-node-legacy.c` are kept only for the
  legacy non-BIM path.
