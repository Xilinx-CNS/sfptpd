# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2022 Xilinx, Inc.

# Variables for installation and package building

# Requires the following input variables:
#   PACKAGE_NAME
#   PACKAGE_VERSION
# These are overriden by anything supplied by the packaging system

ifndef prefix
prefix = /usr/local
else
ifeq ("$(prefix)","/usr")
INST_CONFDIR = $(DESTDIR)/etc
else
ifeq ("$(prefix)","/")
prefix =
endif
endif
endif

SBINDIR ?= sbin

# Defaults from OS detection

ifneq (,$(wildcard /etc/debian_version))
DEFAULT_DEFAULTSDIR := default
DEFAULT_UNITPREFIX :=
else
DEFAULT_DEFAULTSDIR := sysconfig
DEFAULT_UNITPREFIX := $(prefix)
endif

# Installation variables
INST_SBINDIR ?= $(DESTDIR)$(prefix)/$(SBINDIR)
INST_UNITDIR ?= $(DESTDIR)$(DEFAULT_UNITPREFIX)/lib/systemd/system
INST_CONFDIR ?= $(DESTDIR)$(prefix)/etc
INST_DOCDIR ?= $(DESTDIR)$(prefix)/share/doc
INST_MANDIR ?= $(DESTDIR)$(prefix)/share/man
INST_PKGDOCDIR ?= $(INST_DOCDIR)/$(PACKAGE_NAME)
INST_PKGLICENSEDIR ?= $(INST_PKGDOCDIR)
INST_DEFAULTSDIR ?= $(INST_CONFDIR)/$(DEFAULT_DEFAULTSDIR)
INST_PKGLIBEXECDIR ?= $(DESTDIR)$(prefix)/libexec/$(PACKAGE_NAME)

# Installation customisation

INST_OMIT ?=
INST_INITS ?= systemd

# fin
