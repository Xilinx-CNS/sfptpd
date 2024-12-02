# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2024 Xilinx, Inc.

# Top-level makefile for sfptpd

# Scrape the constants file for the version number
SFPTPD_VERSION = $(shell scripts/sfptpd_versioning read)

### Global configuration
PACKAGE_NAME = sfptpd
PACKAGE_VERSION = $(SFPTPD_VERSION)

### Exclude unsupported features by default
# The GPS module is not supported; use 'make NO_GPS=' to enable build
NO_GPS = 1
#NO_ONLOAD = 1

### Base definitions
INCDIRS := $(ONLOAD_INC)
TEST_CC := $(CC) $(INCDIRS) -x c

### Definitions conditional on build environment
if_header_then = printf "\#include <$1>" | $(TEST_CC) -E - > /dev/null 2>&1 && echo $2
if_defn_then = printf "\#include <$1>\nint a=$2;" | $(TEST_CC) $(INCDIRS) -c -S -o - - > /dev/null 2>&1 && echo -DHAVE_$2

### Conditional definitions
CONDITIONAL_DEFS := \
 $(if $(value GLIBC_COMPAT),-DSFPTPD_GLIBC_COMPAT) \
 $(shell $(call if_header_then,sys/capability.h,-DHAVE_CAPS)) \
 $(shell $(call if_header_then,linux/ethtool_netlink.h,-DHAVE_ETHTOOL_NETLINK)) \
 $(shell $(call if_header_then,linux/time_types.h,-DHAVE_TIME_TYPES)) \
 $(shell $(call if_defn_then,linux/if_link.h,IFLA_PERM_ADDRESS)) \
 $(shell $(call if_defn_then,linux/if_link.h,IFLA_PARENT_DEV_NAME))
CONDITIONAL_LIBS := \
 $(shell $(call if_header_then,sys/capability.h,-lcap))

ifndef NO_GPS
CONDITIONAL_DEFS += $(shell $(call if_header_then,gps.h,-DHAVE_GPS))
CONDITIONAL_LIBS += $(shell $(call if_header_then,gps.h,-lgps))
endif

ifndef NO_ONLOAD
CONDITIONAL_DEFS += $(shell $(call if_header_then,onload/extensions.h,-DHAVE_ONLOAD_EXT))
endif

### Unit testing
FAST_TESTS = bic hash stats config link time
TEST_CMD = valgrind --track-origins=yes --error-exitcode=1 build/sfptpd_test

# Include installation and packaging helper
#
include mk/install.mk

### Build flags for all targets
#
CFLAGS += -MMD -MP -Wall -Werror -Wundef -Wstrict-prototypes \
	-Wnested-externs -g -pthread -fPIC -std=c11 \
	-D_GNU_SOURCE \
	$(CONDITIONAL_DEFS) \
	-DINST_PREFIX=$(prefix) \
	-fstack-protector-all -Wstack-protector

# Build flag to enable extra build-time checks e.g. formatting strings
#	 -DSFPTPD_BUILDTIME_CHECKS

ARFLAGS = rcs
CFLAGS += $(CFLAGS_APPEND)
LDFLAGS +=
LDLIBS = -lm -lrt -lpthread -lmnl $(CONDITIONAL_LIBS)
STATIC_LIBRARIES :=
TARGETS :=
MKDIR = mkdir -p

ifdef DEBUG
# Enable symbolic backtraces
LDFLAGS += -rdynamic
endif

# Build directory
BUILD_DIR = build

### Build tools
#

COMPILE         = $(CC) $(CFLAGS) $(INCDIRS) $(EXTRA_CFLAGS) -o $@ -c $<
ARCHIVE         = $(AR) $(ARFLAGS) $@ $^
LINK            = $(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -Wl,--end-group $(LDLIBS)

# Include make rules
include mk/rules.mk

# Include RPM packaging helper
include mk/rpm.mk

# Include the top level makefiles
dir := src
include $(dir)/module.mk
dir := test
include $(dir)/module.mk


# The variables TGT_*, CLEAN and CMD_INST* may be added to by the Makefile
# fragments in the various subdirectories.

.PHONY: targets
targets: $(TARGETS)

.PHONY: clean
clean:
	$(RM) -r $(BUILD_DIR)

.PHONY: test
test:   sfptpd_test
	$(TEST_CMD) all

test_%: build/sfptpd_test
	$< $*

.PHONY: fast_test
fast_test: build/sfptpd_test
	$< $(FAST_TESTS)

# Target to update the version string with divergence from tag in git archive
.PHONY: patch_version
patch_version:
	scripts/sfptpd_versioning patch

$(BUILD_DIR):
	mkdir -p $@

# Patch init paths
$(BUILD_DIR)/%.service: scripts/systemd/%.service $(BUILD_DIR)
	sed s,/etc/sysconfig,$(patsubst $(DESTDIR)%,%,$(INST_DEFAULTSDIR)),g < $< > $@

.PHONY: install
install: sfptpd sfptpdctl sfptpd_priv_helper tstool $(addprefix $(BUILD_DIR)/,sfptpd.service)
	install -d $(INST_PKGDOCDIR)/config
	install -d $(INST_PKGDOCDIR)/examples
	install -d $(INST_PKGLICENSEDIR)
	install -d $(INST_PKGLIBEXECDIR)
	install -d $(INST_DEFAULTSDIR)
	install -d $(INST_MANDIR)/man8
	install -m 755 -p -D $(BUILD_DIR)/sfptpd $(INST_SBINDIR)/sfptpd
	install -m 755 -p -D $(BUILD_DIR)/sfptpdctl $(INST_SBINDIR)/sfptpdctl
	install -m 755 -p -D $(BUILD_DIR)/tstool $(INST_SBINDIR)/tstool
	[ -n "$(filter sfptpmon,$(INST_OMIT))" ] || install -m 755 -p -D scripts/sfptpmon $(INST_SBINDIR)/sfptpmon
	install -m 644 -p -D scripts/sfptpd.env $(INST_DEFAULTSDIR)/sfptpd
	[ -z "$(filter systemd,$(INST_INITS))" ] || install -m 644 -p -D $(BUILD_DIR)/sfptpd.service $(INST_UNITDIR)/sfptpd.service
	[ -n "$(filter license,$(INST_OMIT))" ] || install -m 644 -p -t $(INST_PKGLICENSEDIR) LICENSE PTPD2_COPYRIGHT NTP_COPYRIGHT
	[ -e $(INST_CONFDIR)/sfptpd.conf ] || install -m 644 -p -D config/default.cfg $(INST_CONFDIR)/sfptpd.conf
	install -m 644 -p -t $(INST_PKGDOCDIR)/config config/*.cfg
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples examples/README.sfptpdctl
	[ -n "$(filter c-examples,$(INST_OMIT))" ] || install -m 644 -p -t $(INST_PKGDOCDIR)/examples examples/Makefile.sfptpdctl
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples examples/monitoring_console.py
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples examples/sfptpdctl.py
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples examples/sfptpd_stats_collectd.py
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples $(wildcard examples/*.html)
	[ -n "$(filter c-examples,$(INST_OMIT))" ] || install -m 644 -p -t $(INST_PKGDOCDIR)/examples src/sfptpdctl/sfptpdctl.c
	[ -n "$(filter changelog,$(INST_OMIT))" ] || install -m 644 -p -t $(INST_PKGDOCDIR) CHANGELOG.md
	install -m 755 -p -t $(INST_PKGLIBEXECDIR) examples/chrony_clockcontrol.py
	install -m 755 -p -t $(INST_PKGLIBEXECDIR) $(BUILD_DIR)/sfptpd_priv_helper
	install -m 644 -p -t $(INST_MANDIR)/man8 $(wildcard doc/sfptpd.8)
	install -m 644 -p -t $(INST_MANDIR)/man8 $(wildcard doc/sfptpdctl.8)
	install -m 644 -p -t $(INST_MANDIR)/man8 $(wildcard doc/tstool.8)
	[ -n "$(filter sfptpmon,$(INST_OMIT))" ] || install -m 644 -p -t $(INST_MANDIR)/man8 $(wildcard doc/sfptpmon.8)

.PHONY: uninstall
uninstall:
	rm -f $(INST_SBINDIR)/{sfptpd,sfptpdctl,sfptpmon}
	rm -f $(INST_UNITDIR)/sfptpd.service
	! diff -q config/default.cfg $(INST_CONFDIR)/sfptpd.conf || rm -f $(INST_CONFDIR)/sfptpd.conf
	rm -f $(INST_DEFAULTSDIR)/sfptpd
	rm -f $(INST_MANDIR)/man8/{sfptpd,sfptpdctl,sfptpmon}.8
	rm -f $(DESTDIR)/etc/init/sfptpd
	rm -fr $(INST_PKGDOCDIR)
	rm -fr $(DESTDIR)/var/lib/sfptpd

.PHONY: flat_install
flat_install: SBINDIR=bin
flat_install: prefix=
flat_install: install
	cd $(DESTDIR) && ln -sf $(SBINDIR)/* .

# Prevent make from removing any build targets, including intermediate ones

.SECONDARY: $(CLEAN)


# fin
