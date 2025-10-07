# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2014-2025 Advanced Micro Devices, Inc.

Name: sfptpd
Version: %{pkgversion}
Release: 1
Summary: System time sync daemon supporting PTP, NTP and 1PPS
License: BSD-3-Clause AND BSD-2-Clause AND NTP AND ISC
Group: System Environment/Daemons
Source0: sfptpd-%{version}.tgz
Source1: sfptpd.sysusers
URL: https://www.xilinx.com/download/drivers
Vendor: Advanced Micro Devices, Inc.
Recommends: %{name}-python3
Suggests: ntpsec
BuildRequires: sed
BuildRequires: gcc
BuildRequires: make
BuildRequires: systemd-rpm-macros
BuildRequires: libmnl-devel
BuildRequires: libcap-devel
%{?sysusers_requires_compat}

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

%description python3
Provides python3 utilities and scripts to support sfptpd including a monitor
and example reporting console for the IEEE Std 1588-2019 16.11 event monitoring
option, stats collection for collectd, runtime clock control of chronyd
and customisable Python equivalent of the sfptpdctl client.

%prep
%autosetup
scripts/sfptpd_versioning write %{version}
sed -i 's,-u_sfptpd,-u sfptpd,g' scripts/systemd/sfptpd.service

%build
# Not normally required but ensures the CFLAGS etc. get set to platform
# hardened defaults when launched from containerised github workflow
%set_build_flags
export prefix=%{_prefix}
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
install -m 644 -p -D %{SOURCE1} %{buildroot}%{_sysusersdir}/%{name}.conf
mkdir -p %{buildroot}%{_localstatedir}/lib/%{name}
touch %{buildroot}%{_localstatedir}/lib/%{name}/{config,interfaces,sync-instances,topology,version,ptp-nodes}

%check
make fast_test

%pre
%sysusers_create_compat %{SOURCE1}

%post
%systemd_post %{name}.service

%preun
%systemd_preun %{name}.service

%postun
%systemd_postun_with_restart %{name}.service

%files
%{_sbindir}/sfptpd
%{_sbindir}/sfptpdctl
%{_sbindir}/tstool
%{_libexecdir}/%{name}/sfptpd_priv_helper
%{_unitdir}/sfptpd.service
%config(noreplace) %{_sysconfdir}/sfptpd.conf
%config(noreplace) %{_sysconfdir}/sysconfig/sfptpd
%{_sysusersdir}/%{name}.conf
%license LICENSE PTPD2_COPYRIGHT NTP_COPYRIGHT
%doc %{_pkgdocdir}/CHANGELOG.md
%doc %{_pkgdocdir}/config
%doc %{_pkgdocdir}/examples/sfptpdctl.c
%doc %{_pkgdocdir}/examples/Makefile.sfptpdctl
%doc %{_pkgdocdir}/examples/README.sfptpdctl
%doc %{_pkgdocdir}/examples/sfptpd_json_parse.html
%{_mandir}/man8/sfptpd.8*
%{_mandir}/man8/sfptpdctl.8*
%{_mandir}/man8/tstool.8*
%dir %attr(-,sfptpd,sfptpd) %{_localstatedir}/lib/%{name}
%ghost %attr(-,sfptpd,sfptpd) %{_localstatedir}/lib/%{name}/config
%ghost %attr(-,sfptpd,sfptpd) %{_localstatedir}/lib/%{name}/interfaces
%ghost %attr(-,sfptpd,sfptpd) %{_localstatedir}/lib/%{name}/sync-instances
%ghost %attr(-,sfptpd,sfptpd) %{_localstatedir}/lib/%{name}/topology
%ghost %attr(-,sfptpd,sfptpd) %{_localstatedir}/lib/%{name}/version
%ghost %attr(-,sfptpd,sfptpd) %{_localstatedir}/lib/%{name}/ptp-nodes

%files python3
%{_sbindir}/sfptpmon
%doc %{_pkgdocdir}/examples/monitoring_console.py
%doc %{_pkgdocdir}/examples/sfptpdctl.py
%doc %{_pkgdocdir}/examples/sfptpd_stats_collectd.py
%license LICENSE
%{_mandir}/man8/sfptpmon.8*
%{_libexecdir}/%{name}/chrony_clockcontrol.py

%changelog
* Mon Oct 06 2025 AMD NIC Support <support-nic@amd.com> - 3.9.0.1005-1
- see CHANGELOG.md in source archive for changelog
