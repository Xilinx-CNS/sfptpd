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


RPM_TOPDIR ?= $(shell rpm --eval '%{?_topdir}')
RPM_OSVER ?= $(shell rpm --eval '%{?dist}' | cut -c2-)

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

$(RPM_ALLDIRS):
	mkdir -p $@

.PHONY: rpm_build_tree
rpm_build_tree: $(RPM_ALLDIRS)
	echo ENV:SFPTPD_OSDIST=$(RPM_OSVER)

.PHONY: rpm_prep

rpm_prep: SFPTPD_RPM_VER := $(shell scripts/sfptpd_versioning $(VERSION_QUIRKS) derive)
rpm_prep: rpm_build_tree
	cp -a $(RPM_SPEC_INDIR)/* $(RPM_SOURCES)/
	mv $(RPM_SOURCES)/$(RPM_SPECFILE) $(RPM_SPECS)/
	tar cz --exclude=$(BUILD_DIR) --exclude=$(RPM_TOPDIR) -f $(RPM_SOURCES)/sfptpd-$(SFPTPD_RPM_VER).tgz --transform=s,^\.,sfptpd-$(SFPTPD_RPM_VER),g .
	sed -i "s/^\(Version: \).*/\1 $(SFPTPD_RPM_VER)/g" $(RPM_SPECPATH)
	echo ENV:SFPTPD_VERSION=$(SFPTPD_RPM_VER)

.PHONY: build_srpm
build_srpm: rpm_prep
	$(MKDIR) $(BUILD_DIR)
	rpmbuild --define "_topdir $(RPM_TOPDIR)" -bs $(RPM_SPECPATH) > $(SRPM_MANIFEST) && \
	echo ENV:SFPTPD_SRPMS=$$(sed -n 's,^Wrote: \(.*rpm\).*,\1 ,p' < $(SRPM_MANIFEST))

# fin
