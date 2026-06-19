CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

# Default target: SimpleLink CC13xx/CC26xx
TARGET ?= simplelink
# Default board: CC1352R1 SensorTag (LPSTK)
BOARD ?= sensortag/cc1352r1

PLATFORMS_EXCLUDE = sky native z1

# Simple Energest
MODULES += $(CONTIKI_NG_SERVICES_DIR)/simple-energest

CONTIKI=../../..

# ---------------------------------------------------------------
# Convenience targets:
#   make coordinator   -> node_id=1 binary (coordinator.bin)
#   make sensor-node   -> sensor-node binary WITHOUT fixed NODEID
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
	  cp "$$BUILT" coordinator.bin; \
	  echo ">>> Coordinator binary ready: coordinator.bin"; \
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

.PHONY: coordinator sensor-node

# Orchestra: adds dedicated per-neighbor TX/RX slots (avoids queue overflow
# caused by the single shared slot in the minimal TSCH schedule)
MAKE_WITH_ORCHESTRA ?= 1
# force Security from command line
MAKE_WITH_SECURITY ?= 0
 # print #routes periodically, used for regression tests
MAKE_WITH_PERIODIC_ROUTES_PRINT ?= 0
# RPL storing mode — required for coordinator-to-node downlink
# (coordinator needs a populated routing table to reach sensor nodes)
MAKE_WITH_STORING_ROUTING ?= 1
# Orchestra link-based rule? (Works only if Orchestra & storing mode routing is enabled)
MAKE_WITH_LINK_BASED_ORCHESTRA ?= 0
# Use the Orchestra root rule?
MAKE_WITH_ORCHESTRA_ROOT_RULE ?= 0

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

include $(CONTIKI)/Makefile.include
