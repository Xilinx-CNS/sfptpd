AMD Solarflare Enhanced PTP Daemon
==================================

Version: v3.8.0.1005

These release notes relate to official supported binary releases of sfptpd.
Please see [the changelog](CHANGELOG.md) for a list of changes since earlier
releases.

> [!NOTE]
> Features listed in the changelog that require building from source or relate
> to operating systems or hardware not listed below as supported, are not
> supported by AMD and should be considered exclusive to the open source project.

(c) Copyright 2012-2024 Advanced Micro Devices, Inc.


Overview
--------

This package contains the AMD Solarflare Enhanced PTPD (sfptpd) service.

The sfptpd application co-ordinates time synchronisation between system &
NIC clocks and remote time sources and sinks using PTP, NTP and PPS.


Supported hardware platforms
----------------------------

The daemon contained in this package supports the following AMD adapters:

- AMD Solarflare Server Adapters:
   - XtremeScale(TM) SFN8522 Dual Port SFP+ Server Adapter (with Plus license)
   - XtremeScale(TM) SFN8542 Dual Port QSFP+ Server Adapter (with Plus license)
   - XtremeScale(TM) SFN8042 Dual Port QSFP+ Server Adapter (with Plus license)
   - XtremeScale(TM) SFN8722 Dual Port SFP+ OCP Server Adapter (with Plus license)
   - XtremeScale(TM) X2522 10GbE Adapter
   - XtremeScale(TM) X2522-25 10/25GbE Adapter
   - XtremeScale(TM) X2541 Single Port 10/25/40/50/100GbE Adapter
   - XtremeScale(TM) X2542 Dual Port 10/25/40/50/100GbE Adapter
   - Alveo(TM) XtremeScale(TM) X3522 low latency network adapter

- Support for non-Solarflare adapters is best effort. Particular attention
  should be paid to the tendency to advertise an independent physical clock
  device for each interface when they are in fact the same underlying clock,
  which can be mitigated by listing clocks explicitly.

The latest versions of drivers and firmware for AMD adapters are available
under the Linux category at: <https://www.xilinx.com/download/drivers>


Linux distribution support
--------------------------

This package is supported on:

- Red Hat Enterprise Linux 7.9
- Red Hat Enterprise Linux 8.1 - 8.10
- Red Hat Enterprise Linux 9.0 - 9.4
- Canonical Ubuntu Server LTS 20.04, 22.04, 24.04
- Debian 10 "Buster"
- Debian 11 "Bullseye"
- Debian 12 "Bookworm"
- Linux kernels 3.0 - 6.10

Other and older platforms and non-amd64 architectures may be compatible with
sfptpd but are not supported by AMD. Please raise issues and patches for
unsupported OSs on the community-supported repository at
<https://github.com/Xilinx-CNS/sfptpd>.


Documentation
-------------

For detailed instructions on how to use the sfptpd with AMD Solarflare adapters
please refer to the "Enhanced PTP User Guide" (UG1602) available from
<https://docs.xilinx.com/r/en-US/ug1602-ptp-user>


Support
-------

Please contact your local AMD Solarflare NIC support representative or email
<support-nic@amd.com>.

Issues using sfptpd with non-Solarflare NICs are best raised at
<https://github.com/Xilinx-CNS/sfptpd/issues> for support from the user
community.


Known Issues
------------

- Issue SWNETLINUX-78
   - If the firmware is upgraded using sfupdate while sfptpd is running, sfptpd
     will stop working. AMD recommends that sfptpd is stopped prior to
     performing a firmware upgrade on an adapter.

- Issue SWNETLINUX-4466
   - With affected driver versions PPS is not functional unless timestamping
     is enabled on the relevant interface. For more details see:
     <https://support.xilinx.com/s/article/000033083>

- Issue SWNETLINUX-5126
   - With affected driver verions (e.g. v5.3.16.1004, released November 2023),
     sending a packet with a hardware timetamp on a 7000-series NIC can cause
     an sfc net driver crash in the kernel. 7000-series devices are out of
     support by AMD Solarflare including with sfptpd. Other devices are
     unaffected. For more details see:
     <https://support.xilinx.com/s/article/000036427>


Advanced notice of possible future changes
------------------------------------------

- Explicit declaration of crny sync module

  If chronyd is in use it is recommended to specify an instance of the new
  `crny` sync module explicitly as a future release may not default to
  creating the implicit instance.

- Supporting new IEEE1588 specifications

  Future releases may report support for PTP v2.1 on the wire by default. If
  this causes difficulty with unpatched time servers, specify `ptp_version 2.0`
  in the `ptp` configuration section.

- Deprecation of remote monitor

  The built-in receiver for PTP event monitoring messages (`remote_monitor`)
  has been replaced by the 'sfptpmon' script in the `sfptpd-python3` package.
  The built-in receiver may be disabled in a future feature release. (Note
  that the current version of the script only supports unicast signalling
  on RHEL7, not multicast. Unicast signalling is recommended.)


Summary of major new features since v3.7.1.1006
-----------------------------------------------

For a full list of changes see [the changelog](CHANGELOG.md). These
include changes of interest to community users building from source.

The following are selected as notable new features for AMD-supported
use cases:

- LACP PTP support extended to dual boundary clock solutions. (SWPTP-738)
- Timestamp handling enhanced to improve daemon performance. (SWPTP-831)
- Configurable filename patterns for multi-host config deployment. (SWPTP-649)
- New scheme for handling clock comparisons enhances performance. (SWPTP-1386)
- Debian package now available for Debian and Ubuntu. (SWPTP-1446)
- Privileged helper allows all features while running not as root. (SWPTP-1479)


Copyright
---------

Please see [LICENSE](LICENSE) included in this distribution for terms of use.

sfptpd is provided under a BSD 3-clause license.

This application includes a modified version of the `ptpd` PTPD2 sourceforge
project available at <http://ptpd.sourceforge.net/>. The PTPD2 software is
distributed under a BSD 2-clause [license](PTPD2_COPYRIGHT). The version
sourced is ptpd v2.3.

The program includes part of ntpd to implement NTP query and control
protocols; this is distributed under the [NTP license](NTP_COPYRIGHT).
The version sourced is ntp-4.2.6p5 and the following files in the source code
are affected:

- `src/ntp/sfptpd_ntpd_client_mode6.c`
- `src/ntp/sfptpd_ntpd_client_mode7.c`
- `src/ptp/ptpd2/dep/ntpengine/ntp_isc_md5.c`
- `src/ptp/ptpd2/dep/ntpengine/ntp_isc_md5.h`

