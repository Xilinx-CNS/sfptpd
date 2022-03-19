# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2022 Xilinx, Inc.

# Top-level makefile for sfptpd

# Scrape the constants file for the version number
SFPTPD_VERSION = $(shell grep SFPTPD_VERSION_TEXT src/include/sfptpd_version.h | sed -e 's/[^"]*"//' -e 's/".*//')

### Global configuration
PACKAGE_NAME = sfptpd
PACKAGE_VERSION = $(SFPTPD_VERSION)

### Definitions conditional on build environment
if_header_then = echo "\#include <$1>" | $(CC) -E -x c - > /dev/null 2>&1 && echo $2

CONDITIONAL_DEFS := $(shell $(call if_header_then,sys/capability.h,-DHAVE_CAPS))
CONDITIONAL_LIBS := $(shell $(call if_header_then,sys/capability.h,-lcap))

### Unit testing
FAST_TESTS = bic filters hash stats config
TEST_CMD = valgrind --track-origins=yes --error-exitcode=1 build/sfptpd_test

### Build flags for all targets
#
CFLAGS += -MMD -MP -Wall -Werror -Wundef -Wstrict-prototypes \
	 -Wnested-externs -g -pthread -fPIC -std=gnu99 \
	 -D_ISOC99_SOURCE -D_BSD_SOURCE -D_DEFAULT_SOURCE -D_GNU_SOURCE \
	 $(CONDITIONAL_DEFS) \
	 -fstack-protector-all -Wstack-protector

# Build flag to enable extra build-time checks e.g. formatting strings
#	 -DSFPTPD_BUILDTIME_CHECKS

ARFLAGS = rcs
LDFLAGS +=
LDLIBS = -lm -lrt -lpthread $(CONDITIONAL_LIBS)
INCDIRS :=
STATIC_LIBRARIES :=
TARGETS :=
MKDIR = mkdir -p

# Build directory
BUILD_DIR = build

# Include installation and packaging helper
#
include mk/install.mk

### Build tools
#

COMPILE         = $(CC) $(CFLAGS) $(INCDIRS) -o $@ -c $<
ARCHIVE         = $(AR) $(ARFLAGS) $@ $^
LINK            = $(CC) $(LDFLAGS) -o $@ -Wl,--start-group $^ -Wl,--end-group $(LDLIBS)

# Include make rules
include mk/rules.mk


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
fast_test: $(addprefix test_,$(FAST_TESTS))

.PHONY: install
install: sfptpd sfptpdctl
	install -d $(INST_PKGDOCDIR)/config
	install -d $(INST_PKGDOCDIR)/examples
	install -d $(INST_PKGDOCDIR)/examples/init.d
	install -d $(INST_PKGDOCDIR)/examples/systemd
	install -d $(INST_PKGLICENSEDIR)
	install -d $(INST_DEFAULTSDIR)
	install -d $(INST_MANDIR)/man8
	install -m 755 -p -D build/sfptpd $(INST_SBINDIR)/sfptpd
	install -m 755 -p -D build/sfptpdctl $(INST_SBINDIR)/sfptpdctl
	install -m 644 -p -D scripts/sfptpd.env $(INST_DEFAULTSDIR)/sfptpd
	[ -z "$(filter systemd,$(INST_INITS))" ] || install -m 644 -p -D scripts/systemd/sfptpd.service $(INST_UNITDIR)/sfptpd.service
	[ -z "$(filter sysv,   $(INST_INITS))" ] || install -m 755 -p -D scripts/init.d/sfptpd $(INST_CONFDIR)/init.d/sfptpd
	[ -z "$(filter systemd,$(INST_INITS))" ] || install -m 644 -p -D config/default-systemd.cfg $(INST_CONFDIR)/sfptpd.conf
	[ -n "$(filter systemd,$(INST_INITS))" ] || install -m 644 -p -D config/default-sysv.cfg $(INST_CONFDIR)/sfptpd.conf
	[ -n "$(filter license,$(INST_OMIT))" ] || install -m 644 -p -t $(INST_PKGLICENSEDIR) LICENSE PTPD2_COPYRIGHT NTP_COPYRIGHT.html
	install -m 644 -p -t $(INST_PKGDOCDIR)/config config/*.cfg
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples/init.d scripts/init.d/*
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples/systemd scripts/systemd/*
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples scripts/sfptpd.env
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples $(wildcard examples/*.sfptpdctl)
	install -m 755 -p -t $(INST_PKGDOCDIR)/examples $(wildcard examples/*.py)
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples $(wildcard examples/*.html)
	install -m 644 -p -t $(INST_PKGDOCDIR)/examples src/sfptpdctl/sfptpdctl.c
	install -m 644 -p -t $(INST_MANDIR)/man8 $(wildcard doc/sfptpd*.8)

.PHONY: uninstall
uninstall:
	rm -f $(INST_SBINDIR)/sfptpd
	rm -f $(INST_SBINDIR)/sfptpdctl
	rm -f $(INST_UNITDIR)/sfptpd.service
	rm -f $(INST_CONFDIR)/sfptpd.conf
	rm -f $(INST_DEFAULTSDIR)/sfptpd
	rm -f $(INST_MANDIR)/man8/{sfptpd,sfptpdctl}.8
	rm -fr $(INST_PKGDOCDIR)
	rm -fr $(DESTDIR)/var/lib/sfptpd

# Prevent make from removing any build targets, including intermediate ones

.SECONDARY: $(CLEAN)


# fin
