AMD Solarflare Enhanced PTP Daemon
==================================

Version: v3.8.0.x (unreleased)

These release notes relate to official supported binary releases of sfptpd.
Please see [the changelog](CHANGELOG.md) for a list of changes since earlier
releases.

(c) Copyright 2012-2023 Advanced Micro Devices, Inc.


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

- OEM Server Adapters (with Precision Time license upgrade):
   - HP 570FLB Dual-Port 10GbE FlexibleLOM Server Adapter
   - HP 570M Dual-Port 10GbE Mezzanine Server Adapter

- Support for non-Solarflare adapters is best effort. Particular attention
  should be paid to the tendency to advertise an independent physical clock
  device for each interface when they are in fact the same underlying clock,
  which can be mitigated by listing clocks explicitly.

The latest versions of drivers and firmware for AMD adapters are available
under the Linux category at: <https://www.xilinx.com/download/drivers>


Linux distribution support
--------------------------

This package is supported on:

- Red Hat Enterprise Linux 7.5 - 7.9
- Red Hat Enterprise Linux 8.1 - 8.9
- Red Hat Enterprise Linux 9.0 - 9.3
- SuSE Linux Enterprise Server 12 sp4 and sp5
- SuSE Linux Enterprise Server 15 sp1 - sp5
- Canonical Ubuntu Server LTS 18.04, 20.04, 22.04
- Debian 10 "Buster"
- Debian 11 "Bullseye"
- Debian 12 "Bookworm"
- Linux kernels 3.0 - 6.4


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


Advanced notice of possible future changes
------------------------------------------

- Future change to default for `non_xilinx_nics`

  Future feature releases may change the default setting of `non_xilinx_nics`
  from `off` to `on`, enabling support for all NICs with suitable capabilities
  by default. Where this change is not desired, it is recommended to change
  configurations now to be explicit about the desired behaviour.

- Explicit declaration of crny sync module

  If chronyd is in use it is recommended to specify an instance of the new
  crny sync module explicitly as a future release may not create the implicit
  instance or may do so only if chronyd is detected at startup time.

- Supporting new IEEE1588 specifications

  Future releases may default to supporting new PTP specs out of the box. If
  it is required to avoid new features for interoperability reasons it is
  recommended to specify `ptp_version 2.0` in the `ptp` configuration section.

- Deprecation of remote monitor

  The built-in receiver for PTP event monitoring messages (controlled by the
  `remote_monitor` and `json_remote_monitor` configuration options) is
  deprecated in favour of the `sfptpmon` script, which is a more appropriate
  solution for production use. The built-in receiver may be disabled in
  future feature releases of sfptpd. The option for sending these event
  messages remains supported.


Copyright
---------

Please see [LICENSE](LICENSE) included in this distribution for terms of use.

sfptpd is provided under a BSD 3-clause license.

This application includes a modified version of the `ptpd` PTPD2 sourceforge
project available at <http://ptpd.sourceforge.net/>. The PTPD2 software is
distributed under a BSD 2-clause [license](PTPD2_COPYRIGHT). The version
sourced is ptpd v2.3.

The program includes part of ntpd to implement NTP query and control
protocols; this is distributed under the [NTP license](NTP_COPYRIGHT.html).
The version sourced is ntp-4.2.6p5 and the following files in the source code
are affected:

- `src/ntp/sfptpd_ntpd_client_mode6.c`
- `src/ntp/sfptpd_ntpd_client_mode7.c`
- `src/ptp/ptpd2/dep/ntpengine/ntp_isc_md5.c`
- `src/ptp/ptpd2/dep/ntpengine/ntp_isc_md5.h`

