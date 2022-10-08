# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2022 Xilinx, Inc.

# Variables for installation and package building

# Requires the following input variables:
#   PACKAGE_NAME
#   PACKAGE_VERSION
# These are overriden by anything supplied by the packaging system

# Default to /usr/local if not driven by a packaging tool

ifndef DESTDIR
ifndef INST_PREFIX
INST_PREFIX = /usr/local
INST_CONFDIR = $(INST_PREFIX)/etc
endif
endif

# Installation variables

INST_PREFIX ?= $(DESTDIR)/usr
INST_SBINDIR ?= $(INST_PREFIX)/sbin
INST_UNITDIR ?= $(INST_PREFIX)/lib/systemd/system
INST_CONFDIR ?= $(DESTDIR)/etc
INST_DOCDIR ?= $(INST_PREFIX)/share/doc
INST_MANDIR ?= $(INST_PREFIX)/share/man
INST_PKGDOCDIR ?= $(INST_DOCDIR)/$(PACKAGE_NAME)
INST_PKGLICENSEDIR ?= $(INST_PKGDOCDIR)

# Installation customisation

INST_OMIT ?=
INST_INITS ?= systemd

# fin
