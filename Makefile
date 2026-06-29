CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

# Default target: SimpleLink CC13xx/CC26xx
TARGET ?= simplelink
# Default board: CC1352R1 SensorTag (LPSTK)
BOARD ?= sensortag/cc1352r1

PLATFORMS_EXCLUDE = sky native z1
TI_FULL_SDK_DIR ?= $(HOME)/ti/simplelink_cc13xx_cc26xx_sdk_8_32_00_07
OAD_IMAGE_TOOL ?= $(TI_FULL_SDK_DIR)/tools/common/oad/oad_image_tool.py
OAD_PRIVATE_KEY ?= $(TI_FULL_SDK_DIR)/tools/common/oad/private.pem
OAD_BINARY_TYPE ?= 1
OAD_IMG_TYPE ?= OAD_IMG_TYPE_APP
APP_SEC_VER ?= 1
SLOT_A_SEC_VER ?= $(APP_SEC_VER)
SLOT_B_SEC_VER ?= $(APP_SEC_VER)
APP_IMG_NO ?= 0
APP_V_MAJOR ?= 1
APP_V_MINOR ?= 0
APP_V_PATCH ?= 0
APP_V_BUILD ?= 0
OTA_EXPECTED_IMG_TYPE ?= OAD_IMG_TYPE_APP
OTA_STAGE_RESET_AFTER_VERIFY ?= 1

# Simple Energest
MODULES += $(CONTIKI_NG_SERVICES_DIR)/simple-energest
PROJECT_SOURCEFILES += ota-common.c

# OTA/BIM builds share one coordinator and select only the sensor-node backend
# that matches the requested BIM architecture. The coordinator is intentionally
# target-agnostic; START admission on sensor nodes accepts or rejects the image.
ifeq ($(MAKE_WITH_BIM_DUAL_ONCHIP),1)
  ifeq ($(MAKE_WITH_BIM_OFFCHIP),1)
    $(error MAKE_WITH_BIM_DUAL_ONCHIP and MAKE_WITH_BIM_OFFCHIP are mutually exclusive)
  endif
  PROJECT_SOURCEFILES += ota-coordinator-session.c
  PROJECT_SOURCEFILES += ota-coordinator.c ota-sensor.c sensor-node-dual-onchip.c
  PROJECT_SOURCEFILES += oad_image_header_app.c ota-flash.c ota-metadata.c
  CFLAGS += -DOTA_WITH_BIM_DUAL_ONCHIP=1
  CFLAGS += -DSECURITY
  CFLAGS += -DOAD_IMG_TYPE=OAD_IMG_TYPE_APPSTACKLIB
  CFLAGS += -DAPP_IMG_NO=$(APP_IMG_NO)
  CFLAGS += -DAPP_SEC_VER=$(APP_SEC_VER)
  CFLAGS += -DAPP_V_MAJOR=$(APP_V_MAJOR)
  CFLAGS += -DAPP_V_MINOR=$(APP_V_MINOR)
  CFLAGS += -DAPP_V_PATCH=$(APP_V_PATCH)
  CFLAGS += -DAPP_V_BUILD=$(APP_V_BUILD)
  CFLAGS += -I.
  CFLAGS += -idirafter $(TI_FULL_SDK_DIR)/source
else ifeq ($(MAKE_WITH_BIM_OFFCHIP),1)
  MODULES += arch/dev/storage/ext-flash
  PROJECT_SOURCEFILES += ota-coordinator-session.c
  PROJECT_SOURCEFILES += ota-coordinator.c ota-sensor.c sensor-node-offchip.c
  PROJECT_SOURCEFILES += oad_image_header_app.c
  CFLAGS += -DOTA_WITH_BIM_OFFCHIP=1
  CFLAGS += -DOAD_IMG_TYPE=$(OAD_IMG_TYPE)
  CFLAGS += -DAPP_SEC_VER=$(APP_SEC_VER)
  CFLAGS += -DAPP_IMG_NO=$(APP_IMG_NO)
  CFLAGS += -DAPP_V_MAJOR=$(APP_V_MAJOR)
  CFLAGS += -DAPP_V_MINOR=$(APP_V_MINOR)
  CFLAGS += -DAPP_V_PATCH=$(APP_V_PATCH)
  CFLAGS += -DAPP_V_BUILD=$(APP_V_BUILD)
  CFLAGS += -DOTA_RUNNING_SEC_VER=$(APP_SEC_VER)
  CFLAGS += -DOTA_EXPECTED_IMG_TYPE=$(OTA_EXPECTED_IMG_TYPE)
  CFLAGS += -DOTA_STAGE_RESET_AFTER_VERIFY=$(OTA_STAGE_RESET_AFTER_VERIFY)
  CFLAGS += -DSECURITY
  CFLAGS += -I.
  CFLAGS += -idirafter $(TI_FULL_SDK_DIR)/source
else
  # Legacy non-BIM demo path kept for the original single-binary example.
  MODULES += arch/dev/storage/ext-flash
  PROJECT_SOURCEFILES += coordinator-legacy.c sensor-node-legacy.c
endif

ifdef SOURCE_LDSCRIPT
LDGENFLAGS += $(CFLAGS) -x c -P -E
endif

CONTIKI=../../..

# ---------------------------------------------------------------
# Convenience targets:
#   make coordinator              -> node_id=1 legacy binary
#   make sensor-node              -> legacy sensor-node binary
#   make coordinator-slot-a       -> node_id=1 dual-onchip Slot A OAD image
#   make coordinator-slot-b       -> node_id=1 dual-onchip Slot B OAD image
#   make sensor-node-slot-a       -> sensor-node dual-onchip Slot A OAD image
#   make sensor-node-slot-b       -> sensor-node dual-onchip Slot B OAD image
#   make coordinator-offchip      -> node_id=1 off-chip OAD image
#   make sensor-node-offchip      -> sensor-node off-chip OAD image
#
# IMPORTANT: sensor-node is built WITHOUT NODEID so that each physical
# device uses its own factory-programmed IEEE EUI-64 hardware address.
# Flashing the same sensor-node.bin to multiple boards is safe —
# each will have a unique link-layer address and IPv6 address.
# The coordinator is the only device that needs NODEID=1 (to trigger
# root_start() via the node_id == 1 check in node.c).
# ---------------------------------------------------------------

coordinator:
	$(MAKE) distclean 2>/dev/null; $(MAKE) NODEID=1 MAKE_WITH_ORCHESTRA=1 SUPPORTS_PROP_MODE=0
	@BUILT=$$(find build -name "node.simplelink" | head -1); \
	if [ -n "$$BUILT" ]; then \
	  cp "$$BUILT" coordinator.elf; \
	  arm-none-eabi-objcopy -O binary --gap-fill 0xff "$$BUILT" coordinator.bin; \
	  arm-none-eabi-objcopy -O ihex "$$BUILT" coordinator.hex; \
	  echo ">>> Coordinator ELF ready: coordinator.elf"; \
	  echo ">>> Coordinator raw binary ready: coordinator.bin"; \
	  echo ">>> Coordinator Intel HEX ready: coordinator.hex"; \
	else \
	  echo "ERROR: Binary not found!"; exit 1; \
	fi

coordinator-slot-a:
	$(MAKE) distclean 2>/dev/null; $(MAKE) NODEID=1 MAKE_WITH_ORCHESTRA=1 SUPPORTS_PROP_MODE=0 MAKE_WITH_BIM_DUAL_ONCHIP=1 SOURCE_LDSCRIPT=slot-a.ld APP_SEC_VER=$(APP_SEC_VER) OAD_BINARY_TYPE=7
	@BUILT=$$(find build -name "node.simplelink" | head -1); \
	if [ -n "$$BUILT" ]; then \
	  arm-none-eabi-objcopy -O binary --gap-fill 0xff "$$BUILT" coordinator-slot-a-raw.bin; \
	  arm-none-eabi-objcopy -O ihex "$$BUILT" coordinator-slot-a.hex; \
	  cp "$$BUILT" coordinator-slot-a.elf; \
	  python3 "$(OAD_IMAGE_TOOL)" -hex1 coordinator-slot-a.hex -k "$(OAD_PRIVATE_KEY)" -o coordinator-slot-a ccs . "7"; \
	  echo ">>> Coordinator Slot A ELF ready: coordinator-slot-a.elf"; \
	  echo ">>> Coordinator Slot A raw binary ready: coordinator-slot-a-raw.bin"; \
	  echo ">>> Coordinator Slot A OAD binary ready: coordinator-slot-a.bin"; \
	else \
	  echo "ERROR: Binary not found!"; exit 1; \
	fi

coordinator-slot-b:
	$(MAKE) distclean 2>/dev/null; $(MAKE) NODEID=1 MAKE_WITH_ORCHESTRA=1 SUPPORTS_PROP_MODE=0 MAKE_WITH_BIM_DUAL_ONCHIP=1 SOURCE_LDSCRIPT=slot-b.ld APP_SEC_VER=$(APP_SEC_VER) OAD_BINARY_TYPE=7
	@BUILT=$$(find build -name "node.simplelink" | head -1); \
	if [ -n "$$BUILT" ]; then \
	  arm-none-eabi-objcopy -O binary --gap-fill 0xff "$$BUILT" coordinator-slot-b-raw.bin; \
	  arm-none-eabi-objcopy -O ihex "$$BUILT" coordinator-slot-b.hex; \
	  cp "$$BUILT" coordinator-slot-b.elf; \
	  python3 "$(OAD_IMAGE_TOOL)" -hex1 coordinator-slot-b.hex -k "$(OAD_PRIVATE_KEY)" -o coordinator-slot-b ccs . "7"; \
	  echo ">>> Coordinator Slot B ELF ready: coordinator-slot-b.elf"; \
	  echo ">>> Coordinator Slot B raw binary ready: coordinator-slot-b-raw.bin"; \
	  echo ">>> Coordinator Slot B OAD binary ready: coordinator-slot-b.bin"; \
	else \
	  echo "ERROR: Binary not found!"; exit 1; \
	fi

coordinator-dual-onchip: coordinator-slot-a coordinator-slot-b

coordinator-offchip:
	$(MAKE) distclean 2>/dev/null; $(MAKE) NODEID=1 MAKE_WITH_ORCHESTRA=1 SUPPORTS_PROP_MODE=0 MAKE_WITH_BIM_OFFCHIP=1 SOURCE_LDSCRIPT=offchip-oad.ld APP_SEC_VER=$(APP_SEC_VER)
	@BUILT=$$(find build -name "node.simplelink" | head -1); \
	if [ -n "$$BUILT" ]; then \
	  cp "$$BUILT" coordinator-offchip.elf; \
	  arm-none-eabi-objcopy -O binary --gap-fill 0xff "$$BUILT" coordinator-offchip-raw.bin; \
	  arm-none-eabi-objcopy -O ihex "$$BUILT" coordinator-offchip.hex; \
	  python3 "$(OAD_IMAGE_TOOL)" -hex1 coordinator-offchip.hex -hex2 coordinator-offchip.hex -k "$(OAD_PRIVATE_KEY)" -o coordinator-offchip ccs . "$(OAD_BINARY_TYPE)" || exit 1; \
	  echo ">>> Coordinator off-chip BIM ELF ready: coordinator-offchip.elf"; \
	  echo ">>> Coordinator off-chip BIM raw binary ready: coordinator-offchip-raw.bin"; \
	  echo ">>> Coordinator off-chip BIM OAD binary ready: coordinator-offchip.bin"; \
	  echo ">>> Flash coordinator-offchip.bin at internal address 0x00000000 after bim_offchip."; \
	else \
	  echo "ERROR: Binary not found!"; exit 1; \
	fi

sensor-node:
	$(MAKE) distclean 2>/dev/null; $(MAKE) MAKE_WITH_ORCHESTRA=1 SUPPORTS_PROP_MODE=0
	@BUILT=$$(find build -name "node.simplelink" | head -1); \
	if [ -n "$$BUILT" ]; then \
	  cp "$$BUILT" sensor-node.bin; \
	  echo ">>> Sensor-node binary ready: sensor-node.bin"; \
	  echo ">>> (Same binary can be used on each device - hardware MAC address is unique)"; \
	else \
	  echo "ERROR: Binary not found!"; exit 1; \
	fi

sensor-node-slot-a:
	$(MAKE) distclean 2>/dev/null; $(MAKE) MAKE_WITH_ORCHESTRA=1 SUPPORTS_PROP_MODE=0 MAKE_WITH_BIM_DUAL_ONCHIP=1 SOURCE_LDSCRIPT=slot-a.ld APP_SEC_VER=$(APP_SEC_VER) OAD_BINARY_TYPE=7
	@BUILT=$$(find build -name "node.simplelink" | head -1); \
	if [ -n "$$BUILT" ]; then \
	  arm-none-eabi-objcopy -O binary --gap-fill 0xff "$$BUILT" sensor-node-slot-a-raw.bin; \
	  arm-none-eabi-objcopy -O ihex "$$BUILT" sensor-node-slot-a.hex; \
	  cp "$$BUILT" sensor-node-slot-a.elf; \
	  python3 "$(OAD_IMAGE_TOOL)" -hex1 sensor-node-slot-a.hex -k "$(OAD_PRIVATE_KEY)" -o sensor-node-slot-a ccs . "7"; \
	  cp sensor-node-slot-a.bin sensor-node.bin; \
	  cp sensor-node-slot-a.hex sensor-node.hex; \
	  echo ">>> Sensor-node Slot A OAD binary ready: sensor-node-slot-a.bin"; \
	else \
	  echo "ERROR: Binary not found!"; exit 1; \
	fi

sensor-node-slot-b:
	$(MAKE) distclean 2>/dev/null; $(MAKE) MAKE_WITH_ORCHESTRA=1 SUPPORTS_PROP_MODE=0 MAKE_WITH_BIM_DUAL_ONCHIP=1 SOURCE_LDSCRIPT=slot-b.ld APP_SEC_VER=$(APP_SEC_VER) OAD_BINARY_TYPE=7
	@BUILT=$$(find build -name "node.simplelink" | head -1); \
	if [ -n "$$BUILT" ]; then \
	  arm-none-eabi-objcopy -O binary --gap-fill 0xff "$$BUILT" sensor-node-slot-b-raw.bin; \
	  arm-none-eabi-objcopy -O ihex "$$BUILT" sensor-node-slot-b.hex; \
	  cp "$$BUILT" sensor-node-slot-b.elf; \
	  python3 "$(OAD_IMAGE_TOOL)" -hex1 sensor-node-slot-b.hex -k "$(OAD_PRIVATE_KEY)" -o sensor-node-slot-b ccs . "7"; \
	  echo ">>> Sensor-node Slot B OAD binary ready: sensor-node-slot-b.bin"; \
	  echo ">>> sensor-node.bin points to Slot A for initial flashing"; \
	else \
	  echo "ERROR: Binary not found!"; exit 1; \
	fi

sensor-node-dual-onchip: sensor-node-slot-a sensor-node-slot-b

sensor-node-offchip:
	$(MAKE) distclean 2>/dev/null; $(MAKE) MAKE_WITH_ORCHESTRA=1 SUPPORTS_PROP_MODE=0 MAKE_WITH_BIM_OFFCHIP=1 SOURCE_LDSCRIPT=offchip-oad.ld APP_SEC_VER=$(APP_SEC_VER)
	@BUILT=$$(find build -name "node.simplelink" | head -1); \
	if [ -n "$$BUILT" ]; then \
	  cp "$$BUILT" sensor-node-offchip.elf; \
	  arm-none-eabi-objcopy -O binary --gap-fill 0xff "$$BUILT" sensor-node-offchip-raw.bin; \
	  arm-none-eabi-objcopy -O ihex "$$BUILT" sensor-node-offchip.hex; \
	  python3 "$(OAD_IMAGE_TOOL)" -hex1 sensor-node-offchip.hex -hex2 sensor-node-offchip.hex -k "$(OAD_PRIVATE_KEY)" -o sensor-node-offchip ccs . "$(OAD_BINARY_TYPE)" || exit 1; \
	  echo ">>> Sensor-node off-chip BIM ELF ready: sensor-node-offchip.elf"; \
	  echo ">>> Sensor-node off-chip BIM raw binary ready: sensor-node-offchip-raw.bin"; \
	  echo ">>> Sensor-node off-chip BIM OAD binary ready: sensor-node-offchip.bin"; \
	else \
	  echo "ERROR: Binary not found!"; exit 1; \
	fi

.PHONY: coordinator coordinator-slot-a coordinator-slot-b coordinator-dual-onchip coordinator-offchip sensor-node sensor-node-slot-a sensor-node-slot-b sensor-node-dual-onchip sensor-node-offchip

# Orchestra: adds dedicated per-neighbor TX/RX slots (avoids queue overflow
# caused by the single shared slot in the minimal TSCH schedule)
MAKE_WITH_ORCHESTRA ?= 1
# force Security from command line
MAKE_WITH_SECURITY ?= 0
 # print #routes periodically, used for regression tests
MAKE_WITH_PERIODIC_ROUTES_PRINT ?= 1
# RPL storing mode — required for coordinator-to-node downlink
# (coordinator needs a populated routing table to reach sensor nodes)
MAKE_WITH_STORING_ROUTING ?= 1
# Orchestra link-based rule? (Works only if Orchestra & storing mode routing is enabled)
MAKE_WITH_LINK_BASED_ORCHESTRA ?= 0
# Use the Orchestra root rule?
MAKE_WITH_ORCHESTRA_ROOT_RULE ?= 1

MAKE_MAC = MAKE_MAC_TSCH

include $(CONTIKI)/Makefile.dir-variables
include $(CONTIKI)/Makefile.identify-target
ifneq ($(TARGET),z1)
MODULES += $(CONTIKI_NG_SERVICES_DIR)/shell
endif

ORCHESTRA_EXTRA_RULES = &unicast_per_neighbor_rpl_ns

ifeq ($(MAKE_WITH_ORCHESTRA),1)
  MODULES += $(CONTIKI_NG_SERVICES_DIR)/orchestra

  ifeq ($(MAKE_WITH_STORING_ROUTING),1)
    ifeq ($(MAKE_WITH_LINK_BASED_ORCHESTRA),1)
      # enable the `link_based` rule
      ORCHESTRA_EXTRA_RULES = &unicast_per_neighbor_link_based
    else
      # enable the `rpl_storing` rule
      ORCHESTRA_EXTRA_RULES = &unicast_per_neighbor_rpl_storing
    endif

  else
    ifeq ($(MAKE_WITH_LINK_BASED_ORCHESTRA),1)
      $(error "Inconsistent configuration: link-based Orchestra requires routing info")
    endif

  endif

  ifeq ($(MAKE_WITH_ORCHESTRA_ROOT_RULE),1)
    # add the root rule
    ORCHESTRA_EXTRA_RULES +=,&special_for_root
  endif

  # pass the Orchestra rules to the compiler
  CFLAGS += -DORCHESTRA_CONF_RULES="{&eb_per_time_source,$(ORCHESTRA_EXTRA_RULES),&default_common}"
endif

ifeq ($(MAKE_WITH_STORING_ROUTING),1)
  MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC
  CFLAGS += -DRPL_CONF_MOP=RPL_MOP_STORING_NO_MULTICAST
endif

ifeq ($(MAKE_WITH_SECURITY),1)
CFLAGS += -DWITH_SECURITY=1
endif

ifeq ($(MAKE_WITH_PERIODIC_ROUTES_PRINT),1)
CFLAGS += -DWITH_PERIODIC_ROUTES_PRINT=1
endif

CFLAGS += -DEXT_FLASH_PROGRAM_PAGE_SIZE=256
CFLAGS += -DEXT_FLASH_ERASE_SECTOR_SIZE=4096

include $(CONTIKI)/Makefile.include
