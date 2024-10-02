# Solarflare Enhanced PTP Daemon

The sfptpd service synchronises local clocks together with multiple PTP and PPS
sources in one integrated application with high quality timestamp filtering and
supports LACP and active-backup bonds with and without VLANs. The default and
Enterprise PTP profiles are supported.

Instantaneous and long term statistics for monitoring along with support for
'NetSync Monitor' and standard PTP event monitoring support compliance.

Integration is provided to work with NTP sources via `ntpsec`, `ntpd` and
subject to limitations, `chronyd`. The daemon works on Linux systems for 3.0
kernels and later.

See [the changelog](CHANGELOG.md) for recent additions to sfptpd.  The current
stable branch is [v3_8](https://github.com/Xilinx-CNS/sfptpd/tree/v3_8).

For a **quick start** (operating PTP on bond0, domain 100), see
[one line docker example](/INSTALL.md#running-a-pre-built-container-image).

## Building and running from source

1. Change directory to the root of the sfptpd source repository or package
2. Type `make all`
3. The executable daemon is `sfptpd` in the `build/` directory

The built sfptpd daemon does not require installation. However, it must
normally be started as root.

Example installation recipies may be found in [INSTALL.md](INSTALL.md) and
examples configuration files under [config](config/).

A simple back-to-back PTP example:

```sh
# On host A:
sudo build/sfptpd -i eth1 -f config/ptp_master_freerun.cfg

# On host B, first disabling ntpd and chronyd for simplicity:
for i in ntp{,d} chrony{,d}; do sudo service $i stop 2>/dev/null; done
sudo build/sfptpd -i eth1 -f config/ptp_slave.cfg
```

## Supported releases

Supported releases of sfptpd are available from
<https://www.xilinx.com/download/drivers>. These are supported by AMD for the
Solarflare adapters and operating systems listed in the relevant release notes.

The user guide for supported releases is available at
<https://docs.xilinx.com/r/en-US/ug1602-ptp-user>.

## Community-supported usage

The sfptpd daemon provides a system-centric time sync solution that can
be used with any network adapter and driver supporting standard Linux time
APIs. The monolithic design of sfptpd enables a holistic approach to
system time synchronisation, supporting link aggregation and integrating
remote and local clock synchronisation.

### Using non-Solarflare network adapters

Enable the use of non-Solarflare adapters with:

```ini
non_solarflare_nics on
```

By default, non-Solarflare adapters are not synchronised to avoid hitting
a potential limitation with some drivers.

Sfptpd is normally configured to synchronise all available NIC clocks
automatically so that applications can obtain meaningful hardware timestamps
on all interfaces.

In the case of Solarflare NICs, the `sfc` net driver presents the same "PTP
Hardware Clock" (PHC) device (e.g. `/dev/ptp0`) for each of the physical
network ports on a single adapter. Some NICs, however, present separate and
_apparently_ independent PHC devices for each network port which actually
represent the same underlying physical clock.

When using third party NICs affected by this issue the recommended mitigation
is to list explicitly the NIC clocks to be disciplined with `clock_list`.

## Footnotes

```
SPDX-License-Identifier: BSD-3-Clause
SPDX-FileCopyrightText: (c) Copyright 2013-2024 Advanced Micro Devices, Inc.
```

This software is licensed as BSD-3-Clause AND BSD-2-Clause AND NTP AND ISC.
See [LICENSE](LICENSE) file for details.
