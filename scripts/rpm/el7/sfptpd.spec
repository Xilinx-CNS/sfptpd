# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2014-2024 Advanced Micro Devices, Inc.

Name: sfptpd
Version: %{pkgversion}
Release: 1%{?dist}
Summary: System time sync daemon supporting PTP, NTP and 1PPS
License: BSD-3-Clause AND BSD-2-Clause AND NTP AND ISC
Group: System Environment/Daemons
Source0: sfptpd-%{version}.tgz
URL: https://www.xilinx.com/download/drivers
Vendor: Advanced Micro Devices, Inc.
BuildRequires: sed
BuildRequires: gcc
BuildRequires: make
BuildRequires: systemd
BuildRequires: libmnl-devel
BuildRequires: libcap-devel

%description
Use multiple PTP an PPS sources and sync local clocks together in one
integrated application with high quality timestamp filtering supporting
active-backup and active-active bonds, VLANs, ntpsec/chrony integration, live
stats and long term monitoring. Implements IEEE Std 1588-2019 over UDP with
the default and enterprise profiles.

%package python3
Summary: Python scripts to support sfptpd
BuildArch: noarch
BuildRequires: python3-devel
%global _python_bytecompile_extra 0

%description python3
Provides python3 utilities and scripts to support sfptpd including a monitor
and example reporting console for the IEEE Std 1588-2019 16.11 event monitoring
option, stats collection for collectd, runtime clock control of chronyd
and customisable Python equivalent of the sfptpdctl client.

%prep
%autosetup
scripts/sfptpd_versioning write %{version}

%build
%make_build

%install
export CC='false # no compilation at installation stage #'
export PACKAGE_NAME=%{name}
export INST_SBINDIR=%{buildroot}%{_sbindir}
export INST_DOCDIR=%{buildroot}%{_docdir}
export INST_MANDIR=%{buildroot}%{_mandir}
export INST_CONFDIR=%{buildroot}%{_sysconfdir}
export INST_UNITDIR=%{buildroot}%{_unitdir}
export INST_PKGDOCDIR=%{buildroot}%{_pkgdocdir}
export INST_PKGLIBEXECDIR=%{buildroot}%{_libexecdir}/%{name}
export INST_OMIT="license"
export INST_INITS="systemd"
%make_install
mkdir -p %{buildroot}%{_localstatedir}/lib/%{name}
touch %{buildroot}%{_localstatedir}/lib/%{name}/{config,interfaces,sync-instances,topology,version,ptp-nodes}

%check
make fast_test

%post
%systemd_post %{name}.service

%preun
%systemd_preun %{name}.service

%postun
%systemd_postun_with_restart %{name}.service

%files
%attr(755, root, root) %{_sbindir}/sfptpd
%attr(755, root, root) %{_sbindir}/sfptpdctl
%attr(644, root, root) %{_unitdir}/sfptpd.service
%attr(644, root, root) %config(noreplace) %{_sysconfdir}/sfptpd.conf
%attr(644, root, root) %config(noreplace) %{_sysconfdir}/sysconfig/sfptpd
%license LICENSE PTPD2_COPYRIGHT NTP_COPYRIGHT.html
%doc %{_pkgdocdir}/CHANGELOG.md
%doc %{_pkgdocdir}/config
%doc %{_pkgdocdir}/examples/init.d
%doc %{_pkgdocdir}/examples/systemd
%doc %{_pkgdocdir}/examples/sfptpd.env
%doc %{_pkgdocdir}/examples/sfptpdctl.c
%doc %{_pkgdocdir}/examples/Makefile.sfptpdctl
%doc %{_pkgdocdir}/examples/README.sfptpdctl
%doc %{_pkgdocdir}/examples/sfptpd_json_parse.html
%{_mandir}/man8/sfptpd.8*
%{_mandir}/man8/sfptpdctl.8*
%dir %{_localstatedir}/lib/%{name}
%ghost %{_localstatedir}/lib/%{name}/config
%ghost %{_localstatedir}/lib/%{name}/interfaces
%ghost %{_localstatedir}/lib/%{name}/sync-instances
%ghost %{_localstatedir}/lib/%{name}/topology
%ghost %{_localstatedir}/lib/%{name}/version
%ghost %{_localstatedir}/lib/%{name}/ptp-nodes

%files python3
%{_sbindir}/sfptpmon
%doc %{_pkgdocdir}/examples/monitoring_console.py
%doc %{_pkgdocdir}/examples/sfptpdctl.py
%doc %{_pkgdocdir}/examples/sfptpd_stats_collectd.py
%license LICENSE
%{_mandir}/man8/sfptpmon.8*
%{_libexecdir}/%{name}/chrony_clockcontrol.py
%exclude %{_pkgdocdir}/examples/*.py[co]
%exclude %{_libexecdir}/%{name}/*.py[co]

%changelog
* Mon Jan  8 2024 Andrew Bower <andrew.bower@amd.com> - 3.7.1.1003-1
- see CHANGELOG.md in source archive for changelog
