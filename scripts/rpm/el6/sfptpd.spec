# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2014-2025 Advanced Micro Devices, Inc.

Name: sfptpd
Version: %{pkgversion}
Release: 1%{?dist}
Summary: System time sync daemon supporting PTP, NTP and 1PPS
License: BSD
Group: System Environment/Daemons
Source0: sfptpd-%{version}.tgz
URL: https://www.xilinx.com/download/drivers
Vendor: Advanced Micro Devices, Inc.
BuildRequires: sed
BuildRequires: gcc
BuildRequires: make
BuildRequires: libmnl-devel
BuildRequires: libcap-devel
BuildRequires: devtoolset-7-gcc
BuildRoot: %{_tmppath}/%{name}-%{version}-root

%define _pkgdocdir %{_defaultdocdir}/%{name}-%{version}

%description
Use multiple PTP an PPS sources and sync local clocks together in one
integrated application with high quality timestamp filtering supporting
active-backup and active-active bonds, VLANs, ntp/chrony integration, live
stats and long term monitoring. Implements IEEE Std 1588-2019 over UDP with
the default and enterprise profiles.

%package python
Summary: Python scripts to support sfptpd
BuildArch: noarch
BuildRequires: python
%global _python_bytecompile_extra 0

%description python
Provides python2 utilities and scripts to support sfptpd including an
example reporting console for the IEEE Std 1588-2019 16.11 event monitoring
option, stats collection for collectd, runtime clock control of chronyd
and customisable Python equivalent of the sfptpdctl client.

%prep
%autosetup
scripts/sfptpd_versioning write %{version}
find -iregex '.*\.py' | xargs sed -i -r -e '1s,^(#!).*python3,\1/usr/bin/python,'

%build
export prefix=%{_prefix}
scl enable devtoolset-7 'make %{?_smp_mflags} sfptpd sfptpdctl sfptpd_priv_helper tstool GLIBC_COMPAT=1'

%install
export CC='false # no compilation at installation stage #'
export PACKAGE_NAME=%{name}
export INST_SBINDIR=%{buildroot}%{_sbindir}
export INST_DOCDIR=%{buildroot}%{_defaultdocdir}
export INST_MANDIR=%{buildroot}%{_mandir}
export INST_CONFDIR=%{buildroot}%{_sysconfdir}
export INST_UNITDIR=%{buildroot}%{_unitdir}
export INST_PKGDOCDIR=%{buildroot}%{_pkgdocdir}
export INST_PKGLICENSEDIR=$INST_PKGDOCDIR
export INST_PKGLIBEXECDIR=%{buildroot}%{_libexecdir}/%{name}
export INST_OMIT=""
export INST_INITS=""
rm -rf $RPM_BUILD_ROOT
%make_install
install -m 755 -p -D scripts/rpm/el6/sfptpd.init %{buildroot}/etc/init.d/sfptpd
mkdir -p %{buildroot}%{_localstatedir}/lib/%{name}
touch %{buildroot}%{_localstatedir}/lib/%{name}/{config,interfaces,sync-instances,topology,version,ptp-nodes}

%files
%attr(755, root, root) %{_sbindir}/sfptpd
%attr(755, root, root) %{_sbindir}/sfptpdctl
%attr(755, root, root) %{_sbindir}/tstool
%attr(755, root, root) %{_libexecdir}/%{name}/sfptpd_priv_helper
%attr(755, root, root) %{_sysconfdir}/init.d/sfptpd
%attr(644, root, root) %config(noreplace) %{_sysconfdir}/sfptpd.conf
%attr(644, root, root) %config(noreplace) %{_sysconfdir}/sysconfig/sfptpd
%doc %{_pkgdocdir}/LICENSE
%doc %{_pkgdocdir}/PTPD2_COPYRIGHT
%doc %{_pkgdocdir}/NTP_COPYRIGHT
%doc %{_pkgdocdir}/CHANGELOG.md
%doc %{_pkgdocdir}/config
%doc %{_pkgdocdir}/examples/sfptpdctl.c
%doc %{_pkgdocdir}/examples/Makefile.sfptpdctl
%doc %{_pkgdocdir}/examples/README.sfptpdctl
%doc %{_pkgdocdir}/examples/sfptpd_json_parse.html
%{_mandir}/man8/sfptpd.8*
%{_mandir}/man8/sfptpdctl.8*
%{_mandir}/man8/tstool.8*
%dir %{_localstatedir}/lib/%{name}
%ghost %{_localstatedir}/lib/%{name}/config
%ghost %{_localstatedir}/lib/%{name}/interfaces
%ghost %{_localstatedir}/lib/%{name}/sync-instances
%ghost %{_localstatedir}/lib/%{name}/topology
%ghost %{_localstatedir}/lib/%{name}/version
%ghost %{_localstatedir}/lib/%{name}/ptp-nodes

%files python
%exclude %{_sbindir}/sfptpmon
%doc %{_pkgdocdir}/examples/monitoring_console.py
%doc %{_pkgdocdir}/examples/sfptpdctl.py
%doc %{_pkgdocdir}/examples/sfptpd_stats_collectd.py
%doc %{_pkgdocdir}/LICENSE
%exclude %{_mandir}/man8/sfptpmon.8*
%{_libexecdir}/%{name}/chrony_clockcontrol.py
%exclude %{_pkgdocdir}/examples/*.py[co]
%exclude %{_libexecdir}/%{name}/*.py[co]

%clean
rm -rf $RPM_BUILD_ROOT

%changelog
* Mon Oct 06 2025 AMD NIC Support <support-nic@amd.com> - 3.9.0.1006-1
- see CHANGELOG.md in source archive for changelog
