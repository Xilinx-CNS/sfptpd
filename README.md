# Solarflare Enhanced PTP Daemon

The Solarflare Enhanced PTP Daemon works on Linux systems for 3.0 kernels and
later.

(c) Copyright 2013-2023 Advanced Micro Devices, Inc.

## Copyright and license for sfptpd

SPDX-License: BSD-3-Clause AND BSD-2-Clause AND NTP AND ISC

See [LICENSE](LICENSE) file for details.

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

## Supported releases of sfptpd

Supported releases of sfptpd are available from
<https://www.xilinx.com/download/drivers> which are supported by AMD for
the in-support Solarflare adapters and operating systems listed in the
relevant release notes.

## Using the unsupported sfptpd source project

See [changelog](CHANGELOG.md) for the latest additions to the sfptpd project.

The sfptpd daemon was designed to provide a system-centric time sync solution
for users of Solarflare network adapters, overcoming the limitations of other
software (e.g. link aggregation) and integrating NIC-to-NIC and NIC-to-host
clock synchronization with the external synchronization via PTP and PPS.

### Using non-Solarflare PTP-capable network adapters

This application also provides a capable solution for users of non-Solarflare
adapters but this functionality is not enabled by default so that users are
aware of the limitations of this combination:

By default, sfptpd will synchronize all available NIC clocks so that
applications can obtain meaningful hardware timestamps on all interfaces. In
the case of Solarflare NICs, the `sfc` net driver presents the same "PTP
Hardware Clock" (PHC) device (e.g. `/dev/ptp0`) for each of the physical
network ports on a single adapter. Other NICs typically present an
apparently independent PHC device for each network port. However, these
apparently independent PHC devices typically are not independent but represent
the same underlying physical clock. This unfortunate arrangement has one of two
consequences:

1. When sfptpd tries to synchronise the same physical clock using four
   apparently but not actually independent PHC devices, double or quadruple
   corrections occur and the synchronisation outcome is pathological. To
   mitigate this sfptpd has an option, currently on by default, called
   `assume_one_phc_per_nic on`.
2. If all but one of the apparently but not actually independent PHC devices is
   treated by the driver as read only **and** the instance made writable by the
   net driver is not the one on the lowest-numbered port, then `sfptpd` will
   attempt to discipline the clock but there will be no effect. This
   situation can be mitigated by turning off the above option with
   `assume_one_phc_per_nic off`, but will result in wasteful clock comparisons
   for all the PHC devices that are effectively read only.

The current best recommendation to mitigate these limitations is to list
explicitly the network ports of the NIC clocks to be disciplined with
`clock_list`. This is equivalent to what would in any case be necessary when
configuring other time sync software.

Enable the use of non-Solarflare adapters with:

```config
non_solarflare_nics on
```
