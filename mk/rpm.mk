# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2024 Advanced Micro Devices, Inc.

# Rules to assist RPM build
#
# Apart from substituting the derived product version number into the spec
# file there is nothing product-specific in these make helper rules;
# the challenge is simply to put the files in the places expected by
# rpmbuild.
#
# The rules also output some information in ENV:name=value format
# for the convenience of any further wrapper scripts.

HOST_DISTTAG := $(shell rpm --eval '%{?dist}' 2>/dev/null | cut -c2- || true)
RPM_TOPDIR ?= $(shell rpm --eval '%{?_topdir}' 2>/dev/null || true)

RPM_OSVER ?= $(HOST_DISTTAG)
RPM_DISTTAG ?= .$(RPM_OSVER)
ifeq ($(strip $(RPM_DISTTAG)),none)
USE_DISTTAG :=
else
USE_DISTTAG := $(RPM_DISTTAG)
endif

RPM_SPEC_INDIR = scripts/rpm/$(RPM_OSVER)
RPM_SUBDIRS = SOURCES SPECS
RPM_ALLDIRS = $(addprefix $(RPM_TOPDIR)/,$(RPM_SUBDIRS))
RPM_SPECFILE = sfptpd.spec
RPM_SOURCES = $(RPM_TOPDIR)/SOURCES
RPM_SPECS = $(RPM_TOPDIR)/SPECS
RPM_SRPMS = $(RPM_TOPDIR)/SRPMS
RPM_RPMS = $(RPM_TOPDIR)/RPMS
RPM_SPECPATH = $(RPM_SPECS)/$(RPM_SPECFILE)
SRPM_MANIFEST = $(BUILD_DIR)/srpm.manifest

VERSION_QUIRKS = $(if $(findstring $(RPM_OSVER),el6 el7),--no-post-revision-modifier)
ifneq ($(RPM_DISTTAG),.$(RPM_OSVER))
FORCE_DISTTAG = 1
FORCE_DISTTAG_TO := $(USE_DISTTAG)
RPMBUILD_OPTS += --undefine "dist"
else
ifneq ($(RPM_OSVER),$(HOST_DISTTAG))
FORCE_DISTTAG_TO := .$(RPM_OSVER)
RPMBUILD_OPTS += --define "dist $(FORCE_DISTTAG_TO)"
endif
endif

$(RPM_ALLDIRS):
	mkdir -p $@

.PHONY: rpm_build_tree
rpm_build_tree: $(RPM_ALLDIRS)
	echo ENV:SFPTPD_OSDIST=$(RPM_OSVER)

.PHONY: rpm_prep

ifdef RPM_VER
rpm_prep: SFPTPD_RPM_VER := $(RPM_VER)
else
rpm_prep: SFPTPD_RPM_VER := $(shell scripts/sfptpd_versioning $(VERSION_QUIRKS) derive)
endif
rpm_prep: rpm_build_tree
	cp -a $(RPM_SPEC_INDIR)/* $(RPM_SOURCES)/
	mv $(RPM_SOURCES)/$(RPM_SPECFILE) $(RPM_SPECS)/
	tar cz --exclude=$(BUILD_DIR) --exclude=$(RPM_TOPDIR) -f $(RPM_SOURCES)/sfptpd-$(SFPTPD_RPM_VER).tgz --transform=s,^\.,sfptpd-$(SFPTPD_RPM_VER),g .
	sed -i "s/^\(Version: \).*/\1 $(SFPTPD_RPM_VER)/g" $(RPM_SPECPATH)
ifdef FORCE_DISTTAG
	sed -i "s/^\(Release: .*\)%{.*dist}/\1$(FORCE_DISTTAG_TO)/g" $(RPM_SPECPATH)
endif
	echo ENV:SFPTPD_VERSION=$(SFPTPD_RPM_VER)

.PHONY: build_srpm
build_srpm: rpm_prep
	$(MKDIR) $(BUILD_DIR)
	rpmbuild $(RPMBUILD_OPTS) --define "_topdir $(RPM_TOPDIR)" -bs $(RPM_SPECPATH) > $(SRPM_MANIFEST) && \
	echo ENV:SFPTPD_SRPMS=$$(sed -n 's,^Wrote: \(.*rpm\).*,\1 ,p' < $(SRPM_MANIFEST))

# fin
