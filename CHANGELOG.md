# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

(c) Copyright 2012-2026 Advanced Micro Devices, Inc.

## Unreleased

### Added

- Chrony integration improvements
  - WIP: changes to how chrony offset is tracked. (SWPTP-1611)
  - chrony + PPS example removed while combination is unsupported. (SWPTP-1611)
  - restore chrony config on exit. (SWPTP-1612)
  - new edit-chrony-cmdline script edits in place and is therefore compatible
    with the built-in manipulation in sfptpd, unlike old script. (SWPTP-1613)
  - add chrony clock control enable for when initially disabled. (SWPTP-1614)
  - fix to report the chrony offset actually used in stats. (SWPTP-1616)
  - understand non-NTP chrony peers, controlled by `allow_refclk`. (SWPTP-1618)
- Add `--cpu` option to affinitise all or some threads. (SWPTP-1626)

### Fixed

- Issue SWPTP-1634
  - Fix top clockfeed module that could cause assertion failure if engine
    thread paused.
- Issue SWPTP-1645
  - Fix parsing of `phc_pps_methods` configuration.

## [3.9.0.1007] - 2025-11-07

### Added

- Export stats with OpenMetrics exposition. (SWPTP-1000)
  - Exported by default over Unix socket (`openmetrics_unix off` to disable).
  - Exported over TCP if configured with `openmetrics_tcp <listen-addr>`.
  - Configure real time stats buffer size with `openmetrics_rt_stats_buf`.
  - Specify metrics family prefix with `openmetrics_prefix`.
- Export real time stats over OpenMetrics HTTP socket. (SWPTP-1385)
  - For JSON Lines, use path `/rt-stats.jsonl`, for example:
    `curl --unix-socket /run/sfptpd/metrics.sock http://_/rt-stats.jsonl`
  - For JSON Seq (RFC7464), use path `/rt-stats.json-seq`
  - For sfptpd stats log text, use path `/rt-stats.txt` (can be used to access
    stats with `stats_log off`)
  - To avoid consuming real time stats from the buffer when reading, insert
    `peek/` into path
- Add `phc_dedup` option to identify duplicate non-Solarflare PHC devices
  sharing a single NIC clock and treat as one device. (SWPTP-1348)
  - This option requires a calibration step that may take a few seconds.
  - This is an experimental feature.
- Add `all` and `most` logging modules, e.g. `trace_level all <n>` (SWPTP-1421)
- Extend PTP ACLs to cover IPv6 addresses. (SWPTP-1536).
- Make access modes configurable for runtime files and sockets. (SWPTP-1551)
- Add `servo_log_all_samples` option to output every sample for secondary
  servos (16 per second by default) into real-time stats. (SWPTP-1566)
- Add new clock display format interpolators `%i`, `%n` and `%m`. (SWPTP-1569)
- Add `observe_readonly_clocks` option to create passive servos for monitoring
  sync between local clocks not performed by sfptpd. (SWPTP-1579)
- Multiple PPS sync instances may be created for a single interface and
  external connectors individually configured for `pps-in` or `pps-out`
  functions, for boards where this is needed. (SWPTP-1576)
- Make the determination of what counts as a suitable physical interface for
  timestamping configurable for experimental and testing purposes. Only the
  default value (Ethernet and MACVLAN interfaces) is supported. (SWPTP-1578)
- Add `fir_filter_size` option for secondary servos. (SWPTP-1584)
- Add hardware clock control and diagnostic utility `tstool`.

### Changed

- Default control socket moved to `/run/sfptpd/control-v1.sock` (SWPTP-1551)

### Fixed

- Issue SWPTP-1066
  - Fix poor system clock adjustment when frequency error exceeds 500000ppb.
- Issue SWPTP-1569
  - Avoid aborting when a clock name would exceed 63 characters.
- Issue SWPTP-1570
  - Ensure multicast used when multiple PTP instances per interface.
- Issue SWPTP-1586
  - Avoid crash on `sfptpdctl stepclocks` when implicit crny instance disabled.
- Issue SWPTP-1590
  - Fix clock control and system clock blocking behaviour when chrony state
    changes unexpectedly.
- Issue SWPTP-1595
  - Evade driver warnings in logs by not probing for recently-removed private
    ioctls unless preferred Linux method not available.
- Issue SWPTP-1597
  - Eliminate risk of file descriptor leak in check for incompatible
    programs running.

## [3.8.1.1004] - 2025-02-14

### Added

- Add `avoid_efx_ioctl` option to avoid sfc proprietary ioctl(). (SWPTP-1535)
  - This prevents setting of the sync flags optionally used by Onload.

### Changed
- Timeout for preferred NTP mode 7 control reduced to 300ms. (SWPTP-1532)

### Fixed

- Issue SWPTP-1506
  - chrony: use smoothed offset from chrony not last NTP sample
- Issue SWPTP-1531
  - freerun: fix resolution of bond when VLAN specified
- Issue SWPTP-1559
  - Avoid double UTC compensation for looped-back Tx software timestamps
- Issue SWPTP-1560
  - Fix potential buffer overflow reading long PPS device 'names' from sysfs
- Issue SWPTP-1561
  - Avoid bailing out if diagnostic copy of log file cannot be written out
- Xilinx-CNS/sfptpd#17
  - Avoid crash if a clock step occurs when a NIC has gone away

## [3.8.0.1005] - 2024-10-10 [Feature Release]

### Added

- Update sfptpd to be sub nanosecond-ready. (SWPTP-58)
  - Some times and offsets are now output at picosecond resolution that were
    previously output at nanosecond resolution.
- Add --socket option for sfptpdctl to control multiple processes. (SWPTP-624)
- Add support for interpolating values into file paths and other output.
  - Configurable patterns for log, state, control and stats paths. (SWPTP-649)
  - Configurable patterns for clock names and ids. (SWPTP-997)
  - See [example config](config/option-examples.cfg) for specification.
- Enhance LACP support. (SWPTP-738)
  - Support dual boundary clock topology when using multicast delay measurement.
  - Send DelayReq over the physical interface that last received a Sync from
    the current PTP master by maintaining a pool of sockets. (SWPTP-976)
  - Switch the local reference clock to be the one by which the latest Sync
    message was timestamped. (SWPTP-1434)
  - To enable, see [example](config/ptp_slave_lacp.cfg).
- Show unicast/multicast delay response flags in state file. (SWPTP-807)
- Get transmit timestamps via epoll to avoid blocking PTP thread. (SWPTP-831)
- Ethtool queries conducted over netlink on supported kernels. (SWPTP-1304)
- IP address of parent clock added to topology files (SWPTP-1312)
- Add option to configure state file and stats log update rates. (SWPTP-1326)
  - e.g. `reporting_intervals save_state 120 stats_log 2`
- Add `step_threshold` option to change the offset threshold for allowing
  a step (when permitted by clock control setting). (SWPTP-1365)
- Add shared clock feed. (SWPTP-1386)
  - Deliver clock comparisons via shared feed, nearly halving the number
    of measurements needed in a multi-NIC system.
  - Reduce impact of missing and delayed clock measurements
- Use 1588-2019 method for constructing clock ids from EUI-64s. (SWPTP-1402)
  - Allow unique clock id bits to be set with `unique_clockid_bits`.
  - Allow legacy (2008) clock ids to be used with `legacy_clock_ids`.
- Add `pidadjust` control command to tweak PID controller co-efficients
  at runtime for experimental purposes only. (SWPTP-1411)
  - Run `sfptpdctl` without arguments for syntax.
- Always set NIC clocks on startup by default. (SWPTP-1431)
  - Can be reverted to previous behaviour of only setting when not already
    set or if used as PTP master or with a freerunning clock instance.
- Add Debian packaging. (SWPTP-1446)
  - As a new package type, the .deb follows best practice for the Debian
    and derivative distributions by creating and running sfptpd as a new
    system user.
- Add `-D` command line option to specify default PTP domain. (SWPTP-1454)
- Add master-only PTP mode. (SWPTP-1459)
- Add optional privileged helper process when running as non-root. (SWPTP-1479)
  - Enables hotplugging and connecting to and controlling chronyd.
- Allow repeated -v arguments as convenience to increase verbosity. (SWPTP-1489)

### Added for unsupported source builds only

- Allow 32-bit userspace on 64-bit kernels. (SWPTP-181)
- Add `gps` sync module to use `gpsd` for PPS time of day.
  - Build with `make NO_GPS=` having installed `libgps-dev`.
  - Instantiate the `gps` sync module giving connection details, e.g.
    `gpsd ::1 2947` and set as time of day source.
- Add Y2038 support when built for 32-bit targets with 64-bit time enabled.
  (Xilinx-CNS/sfptpd#12)

### Changed

- Default polling interval for chronyd doubled to 2s.
- Default value of `one_phc_per_nic` is now `off`. (SWPTP-1348)
   - See note under [v3.5.0.1004](#3501004---2022-05-13-feature-release)
     and the [README](README.md#using-non-solarflare-network-adapters).
- Default threshold to step rather than slew reduced to 900ms. (SWPTP-1519)
   - This avoids asymmetric behaviour for offsets slightly above or below
     1.0s as can occur with a leap second or when an erroneous 1s step is
     corrected.

### Removed

- The source can no longer be built on RHEL 6, which is unsupported. (But it
  may be possible to build for RHEL 6 on RHEL 7 by defining `GLIBC_COMPAT=1`
  or with `RPM_OSVER=el6 make build_srpm`.) (SWPTP-1371)

### Fixed

- Issue SWPTP-1396
  - Provide correct clock timestamps in real time stats corresponding to the
    reported offsets rather than time of logging message.
- Issue SWPTP-1481
  - When `max_missing_delayresps` config supplied, stop using the wrong
    value for hybrid fallback.
- Issue SWPTP-1515
  - Fix issue with BMC discriminator disqualifying sources when evaluated
    inbetween Sync and FollowUp reception.
- Xilinx-CNS/sfptpd#9
  - Fix issue that manifests as not being able to control the system clock
    on some systems, such as Raspberry Pi 5 with Debian 12.

## [3.7.1.1007] - 2024-01-25

### Added

- Command line enhancements to support container usage. (SWPTP-1401)
  - Allow config to be read from stdin with `-f -`
  - Add `--console` option to redirect logs to console
- Add configurable missing delay response thresholds (SWPTP-897)
  - `max_missing_delayresps <for-alarm> <for-hybrid-fallback>`
  - default changed from `3 3` to `5 3`.

### Changed

- Python scripts reorganised (SWPTP-1416)
  - New RPM package `sfptpd-python3` contains all the Python code
  - Base `sfptpd` package does not depend on Python.
  - Chrony control script moved to `/usr/libexec`.
  - Example python scripts for customisation do not have a shebang.
  - Chrony clock control can now be enabled with `clock_control on` instead
    of specifying a path to the helper script.

> [!NOTE]
> Configurations using the old example chronyd script location will have the
> new location substituted for compatibility but it is advised to replace this
> with a simple `clock_control on` configuration unless this script has been
> customised for the user's system.

### Fixed

- Issue SWPTP-145
  - Use `SO_TIMESTAMPING` for software timestamps when supported in driver
- Issue SWPTP-855
  - Redact NTP key in diagnostic output
- Issue SWPTP-1318
  - Retry synthetic PPS diff method shortly after enabling (in-tree enhancement)
- Issue SWPTP-1353
  - Make timestamp enablement failure less noisy on startup
  - Retry timestamp enablement on EAGAIN from NIC, not just EBUSY
- Issue SWPTP-1364
  - Let systemd restart sfptpd on failure with supplied unit configuration
- Issue SWPTP-1395
  - Fix missing `sfptpmon` script in `.tar.gz` release package
  - Omit unnecessary new reference to system clock frequency correction state
    file in `.rpm` packages to avoid confusing `alien` repackager tool
- Issue SWPTP-1403
  - Ignore irrelevant PTP packets in `sfptpmon` event monitoring script instead
    of exiting
- Issue SWPTP-1405
  - Avoid theoretical risk of double clock correction on startup
  - Extend period NIC considered to have unset time to 5 years after epoch
- Issue SWPTP-1406
  - When `chronyd` is started or stopped, reflect this fact in the 'external
    constraints' instance selection rules.
- Issue SWPTP-1408
  - Fix possible risk of stack corruption in teaming netlink handler
- Issue SWPTP-1412
  - Only show instance selection rankings when candidate changes.
- Issue SWPTP-1425
  - Populate `PTPMON_RESP` TLV with last Sync timestamp not `PTPMON_REQ` receipt
- Issue SWPTP-1429
  - Avoid reverse name lookup in crny diagnostic gathering
- Issue SWPTP-1432
  - Wait until timers created before polling for changes via netlink
- Issue SWPTP-1443
  - Handle lack of HW PPS methods in driver cleanly
- Issue SWPTP-1444
  - Handle NIC clock object creation failure cleanly
- Issue SWPTP-1445
  - Ignore next PPS period after a step instead of briefly raising an alarm
- Issue SWPTP-1447
  - Fix false assumptions of netlink event ordering leading to assertion failure

## [3.7.0.1006] - 2023-08-17 [Feature Release]

### Added

- Allow running as non-root user with minimum required capabilities (SWPTP-442)
   - enable with `user` configuration or `--user` command line options
   - example `udev` rules support hotplug with non-root user
- Support multiple PPS sources (SWPTP-776)
- Support bridges including over bonds in similar way to LACP bonds (SWPTP-826)
- Support MACVLANs when timestamping enabled in host netns (SWPTP-992)
- Allow a bond, bridge or vlan to be specified for freerun clock (SWPTP-1169)
- Replace bond, team and interface probing with netlink scanning (SWPTP-1269)
- Mix `efx` clock diff method with list of preferred phc methods (SWPTP-1285)
- Add sync-instances file showing factors influencing selection (SWPTP-1286)
- Add ntp and servo logging modules for targetted diagnostics (SWPTP-1291)
- Reflect external constraints (`ntpd`, `chronyd`) as selection rule (SWPTP-1295)
- Better quality logging and trace on hotplug events (SWPTP-1300)
- IP address of parent clock added to state and nodes files (SWPTP-1312)
- Better init system integration with new command line options and
  synchronises with systemd on startup as a 'notify'-type service (SWPTP-1321)
  ```
  --daemon       supersedes now-deprecated 'daemon' config file option
  --no-daemon    overrides now-deprecated 'daemon' config file option
  --version      outputs version number
  --test-config  tests configuration file
  --user USER[:GROUP]
  ```
- Support up to 6 destinations for PTP event monitoring messages (SWPTP-1340)
- Add `sfptpmon` script to receive PTP event monitoring messages (SWPTP-1342)
- Accept any port for unicast PTP event monitoring destinations (SWPTP-1346)
- The `ntpsec` implementation of ntpd available in the EPEL repositories for
     RHEL 9 is confirmed to be fully interoperable with sfptpd, allowing
     system clock disciplining to be controlled at runtime.

> [!IMPORTANT]
> The root user is required if the `chronyd` clock control script example
> is used to restart `chronyd`

> [!NOTE]
> All options for `hotplug_detection_mode` other than the synonyms `auto` and
> `netlink` are now redundant and ignored. None of the old modes has any
> benefit over the new netlink implementation, which eliminates many
> system calls.

### Removed

- Special provision for SFN5000 and SFN6000 series adapters removed (SWPTP-1306)
- Provision for hardware timestamping without Linux PHC removed (SWPTP-1306)
- Support, testing and packaging for RHEL6 is dropped (SWPTP-1371)

### Fixed

- Issue SWPTP-1278
  - Fix retrieval of transmit timestamps to recover from nic reset quicker
- Issue SWPTP-1290
  - Fix failure subsequently to enable timestamps when started with empty bond
- Issue SWPTP-1296
  - Improve logging of timer fd activity to identify system configuration issues
- Issue SWPTP-1314
  - Fix issues using PPS via `/dev/ptp` external timestamping pin
- Issue SWPTP-1326
  - Alter buffering for RT JSON stats output to avoid excessive writes per sec
- Issue SWPTP-1347
  - Remove bogus reserved field from rx timing event monitoring TLV
- Issue SWPTP-1358
  - Fix `chronyd` client state handling avoiding crashes after system startup
- Issue SWPTP-1366
  - Fix `ntpd` client handling to avoid quitting when peer stats not available

> [!WARNING]
> Where sfptpd clients emit Rx Sync Timing Data event monitoring they need
> to be updated from older versions at the same time as upgrading from
> an sfptpd remote monitor or the monitored data will be unusable.
> This only applies to users of the `mon_rx_sync_timing_data` option.

## [3.6.0.1015] - 2023-01-30 [Feature Release]

### Added

- Support Xilinx Alveo X3522 Ethernet adapters (SWPTP-1162)
- Support for operating alongside the chronyd service is now provided in a
  separate 'crny' sync module from the 'ntp' module used for ntpd, e.g.
    ```
    [general]
    sync_module crny crny1
    ```
  To aid the transition from existing behaviour under the 'ntp' sync module
  a suitable instance is created implicitly. To **disable** the implicit
  sync instance for simplified logging and diagnostics, specify a sync module
  with no instances:
    ```
    [general]
    sync_module crny
    ```
- Allow user-defined scripts to reconfigure and restart a chronyd service
  as a workaround to lack of a 'clock control' facility in chronyd. This
  brings parity in the supportable use cases of ntpd and chronyd. (SWPTP-1216)
    ```
    [crny]
    control_script /usr/share/doc/sfptpd/examples/chrony_clockcontrol.py
    ```
- Allow operation but block system clock updates when an uncontrollable
  chronyd service is running, except when this condition holds at startup.
  This is improved behaviour when the conflict is transient. It is, however,
  recommended in such situations to configure sfptpd explicitly to treat
  the system clock as read only. To disable this check at startup,
  configure as follows. (SWPTP-1266)
    ```
    [general]
    ignore_critical clock-control-conflict
    ```
- Allow operation in the absence of a kernel PTP subsystem or PTP and PPS
  without a PTP hardware clock. This change supports esoteric and diagnostic
  use cases but is not recommended or supported in production. (SWPTP-1207)
    ```
    [general]
    ignore_critical no-ptp-subsystem no-ptp-clock
    ```
- Write running configuration into state directory as a diagnostic. (SWPTP-198)
- Control system clock flags to let kernel set RTC when in sync. (SWPTP-216)
- Add `manual-startup` selection policy to start with a configured sync
  instance before moving onto automatic selection. (SWPTP-823)
- Add separate trace component to help understand the detailed reason for
  changes in sync instance selection. Disable in normal use to avoid excessive
  logging.
    ```
    [general]
    trace_level bic 3
    ```
> [!IMPORTANT]
> Restarting chronyd is destabilising for NTP synchronisation so this
> feature should be avoided when chrony's time sync performance influences
> sync instance selection. In the absence of native runtime control for
> chronyd it is best used in an always-on or always-off mode with sfptpd. If
> chronyd control is required for fallback it is advisable to give it a
> lower (numerically higher) `priority` setting so it is only selected on
> total failure of PTP and to increase `selection_holdoff_period` to 60s.
> Similarly, it would not be recommended to use this clock control method
> while chrony was operating as a clustering determinant (i.e. discriminator).

> [!TIP]
> It is recommended to be *explicit* about whether or not sfptpd should talk
> to the chrony service to be resilient to future changes in defaults, by
> using the `sync_module` option to enable or disable it.
> (Added: 2023-10-27)

### Changed

- Make `correct-clock` the default mode for the epoch guard. This option has
  proven effective and reliable since its introduction. (SWPTP-1283)

### Fixed

- Issue SWPTP-1257
  - It is not fatal for the system clock not to be using the superior TSC.
- Issue SWPTP-1264
  - Drop check for ancient driver/fw versions, avoiding parse failures
    for in-tree drivers in new kernels.
- Issue SWPTP-1280
  - Only use Allan Variance in clock source comparisons when measured by
    both sources, otherwise fall through to next selection criterion.
- Issue SWPTP-1281
  - Allow for delayed first sample when using PPS clock diff method; helps
    when using in-tree sfc driver.

## [3.5.0.1004] - 2022-05-13 [Feature Release]

### Added

- Support in-tree driver (SWPTP-211)  
  Out-of-tree driver from support-nic.xilinx.com is recommended for best
  performance.
- Allow configuration of preferred order of clock diff methods
  for non-Xilinx NICs (SWPTP-1193), e.g.:
    ```
    [general]
    phc_diff_methods sys-offset-precise pps sys-offset-ext sys-offset read-time
    ```
- Support synchronisation to PPS inputs via `/dev/pps` or `/dev/ptp`.
  The preferred order of methods can also be configured (SWPTP-1164), e.g.:
    ```
    [general]
    phc_pps_methods devpps devptp
    ```
- Assume multiple PTP Hardware Clock (PHC) devices presented by the same
  PCI device represent the same underlying clock, which is often the case
  for non-Xilinx NICs. Enabled by default. (SWPTP-1181)
  - To disable:
     ```
     [general]
     assume_one_phc_per_nic off
    ```
- Allow hardware or software timestamping to be required for a given PTP
  sync instance and refuse to start if requirement cannot be met (SWPTP-212)
    ```
    [ptp1]
    timestamping hw
    ```
- Allow maximum NIC clock skew amount to be limited to less than its
  advertised capabilities in ppb (SWPTP-815), e.g.:
    ```
    [general]
    limit_freq_adj 50000000
    ```
- Decode advertised PTP clock accuracy to ns in state files (SWPTP-1045)
- Support `PTP_SYS_OFFSET_EXTENDED` clock diff method (SWPTP-1187)
- Allow control socket location to be specified (SWPTP-624)
- Detect if `systemd-timesyncd` is running (SWPTP-986)
- Alarm secondary servo on 30s of sustained sync failure (SWPTP-1049)

> [!IMPORTANT]
> It is no longer clear that `assume_one_phc_per_nic off` is the most
> effective solution for working around the limitations of the way
> PHC clocks are made available on non-Solarflare interfaces and users
> may be better off selecting an explicit `clock_list` for third party NICs.
> (Added: 2023-10-26)

### Fixed

- Issue SWPTP-1203
  - Synchronise normal thread activity start to avoid premature inter-thread
    messaging
- Issue SWPTP-1188
  - Show offset as only a comparison in stats when ntpd not disciplining
- Issue SWPTP-1184
  - Fix failure to configure hardware timestamping where one interface in a
    bond only supports software timestamping
- Issue SWPTP-1183
  - Fix use of PPS clock diff method for non-Xilinx NICs after a step

## [3.4.1.1000] - 2021-12-16

### Added
- Allow PTP as PPS time-of-day source (SWPTP-1163)
  - A single sync instance can be specified in a global PPS section, e.g.:
    ```
    [pps]
    time_of_day ptp1
    ```

### Fixed

- Issue SWPTP-1172
  - Handle reference clocks and unreachable peers reported by chronyd suitably
    rather than crashing with an assertion failure
- Issue SWPTP-424
  - Propagate time and frequency traceability flags from PPS to PTP

## [3.4.0.1003] - 2021-10-29 [Feature Release]

### Added

- Add simple clustering algorithm between sync instances (SWPTP-421).  
  In this mode a passive sync instance (e.g. NTP) is designated as the
 'discriminator' between multiple active sync instances (e.g. PTP) to
  allow wildly divergent time sources to be excluded in sync instance
  selection.
  Example configuration for preferring instances within 100ms of NTP ref:
    ```
    [general]
    clustering discriminator ntp1 100000000 1
    ```
- Extend interoperability with chronyd (SWPTP-978).  
  Support chronyd as NTP server when either sfptpd or chronyd is
  statically configured not to discipline the system clock. For example,
  to enable the chronyd + PPS use case launch chronyd with the `-x` option.
- Support the draft PTP Enterprise Profile (SWPTP-975).  
  Suitable defaults and configuration constraints can be selected by using
  the following option, which should appear before other options.
    ```
    [ptp]
    ptp_profile enterprise
    ```
  See https://datatracker.ietf.org/doc/html/draft-ietf-tictoc-ptp-enterprise-profile-21
- Support the IEEE1588-2019 revision of the PTP standard (SWPTP-591).  
  Implementations of the 2019 revision, known informally as PTP v2.1, should
  be backwards compatible with implementations of the 2008 revision (v2.0)
  but to assure continued interoperability with intolerant peer devices
  the protocol version must be specified to use new features, such as the
  `COMMUNICATION_CAPABILITIES` TLV:
    ```
    [ptp]
    ptp_version 2.1
    ```
- Support for the IPv6/UDP PTP transport (annex E) (SWPTP-601).
  The link-local and global scopes are supported. Example configuration:
    ```
    [ptp]
    transport ipv6
    scope link-local
    ```
- Add `probe-only` interface detection mode to avoid Netlink (SWPTP-1145):
  `hotplug_detection_mode probe`.
- Raise alarm on secondary servos that fail to operate continuously for 30
  seconds or more such as due to clock comparison failure. (SWPTP-1049).
- Use `COMMUNICATIONS_CAPABILITIES` TLV to agree multicast vs unicast delay
  request capabilities with corresponding PTP node (SWPTP-687).
- Extend application of 'epoch guard' for hosts with multiple NICs.
  (SWPTP-1109)
- Convert example scripts to python 3 (SWPTP-957).
- Report outlier statistics in long term stats (SWPTP-540).

> [!TIP]
> While similar to the sfptpd Best Master Clock (BMC) Discriminator
> feature, the 'clustering' feature operates between sync instances
> (i.e. on separate PTP domains and/or interfaces) rather than within
> a PTP domain and is a preferable solution because full clock reconstruction
> is continuously performed for each candidate source.

> [!NOTE]
> Frequent outliers are always expected to show up in statistics as they
> are intrinsic to the adaptive algorithm.

### Fixed
- Issue SWPTP-1098, SWPTP-1102
  - Fix `clock_readonly` option.
- Issue SWPTP-1091
  - Correct handling of timestamp when `Follow_Up` messages received or handled
    ahead of `Sync`, occasionally resulting in large time jumps.
- Issue SWPTP-1078
  - Fix invalid JSON in real time stats output.
- Issue SWPTP-895
  - Serialise log rotation with log writing to avoid broken output.
- Issue SWPTP-844, SWPTP-1150
  - Fix association of PTP sync instances to interfaces when there are multiple
    instances on multiple interfaces so that correct operation is no longer
    sensitive to ordering in configuration.

## [3.3.1.1001] - 2020-10-02

### Added
- New `step-on-first-lock` clock control mode (SWPTP-1032)
  - Add mode to allow clock steps after first lock onto a PTP master.
  - This mode addresses the unintuitive behaviour of `step-at-startup` when
    there are multiple local clocks that have already been slewing to NIC.
- Configurable bond/hotplug detection mode (SWPTP-1041)
  - Add option to detect bond changes only via netlink events to avoid
    expensive polling of bond state when teamd in use.
  - Enable with `hotplug_detection_mode netlink`.

### Fixed
- Issue SWPTP-1002
  - Propagate `TIME_TRACEABLE` flag when acting both as master and slave
- Issue SWPTP-1005
  - Consider UTC offset in advertisement when computing BMC discriminator offset
- Issue SWPTP-1037
  - Correct handling of cached `Follow_Up` messages received ahead of Sync

## [v3.3.0.1007] - 2020-03-03 [Feature Release]

### Added
- BMC Discriminator (SWPTP-906)  
  Provide option to disqualify PTP masters which advertise a time that
  differs by more than a given threshold from a specific time source such
  as the selected NTP server.
- Hybrid Network Mode Without Fallback (SWPTP-684)  
  Provide a new mode for DelayReq/DelayResp messaging that does not fall
  back to multicast when there is no response.
- Freerun with System Clock (SWPTP-884)  
  Allow the system clock to be used as the clock source with the freerunning
  sync module. In combination with other options this enables sfptpd to
  operate alongside chronyd for some use cases.
- Epoch Guard (SWPTP-908)  
  Detect when a local clock has been reset to 1970, e.g. on an unexpected
  NIC reset and raise an alarm, prevent it being used to synchronise other
  clocks (default behaviour) and/or step it immediately.
- Chronyd (SWPTP-565)  
  Support limited use cases where sfptpd operates alongside chronyd with
  read-only access to the system clock. Due to chronyd not supporting
  external control of whether it disciplines clocks it is still recommended
  to disable chronyd and install ntpd where NTP functionality is desired.
- Systemd example (SWPTP-536)  
  Install a systemd unit file to start sfptpd instead of a sysv-style
  initscript and an example configuration file suitable for passive
  operation alongside chronyd.
- Configurable state path (SWPTP-909)  
  Provide `state_path` option to change the location of sfptpd state files.
- Slave UTC override option (SWPTP-726)  
  A new option `ptp_utc_offset_handling override <N>` uses a fixed UTC
  offset of N seconds regardless of any offset announced by PTP masters.
  This option aids interoperability with faulty GMs but requires manual
  intervention to handle leap seconds.
- Log sync instance alarm changes (SWPTP-988)  
  Any change in alarms or lack of alarms is shown in the message log.

> [!NOTE]
> The [sync instances clustering feature](#3401003---2021-10-29-feature-release)
> is preferred to the BMC discriminator wherever possible. The use case for
> the BMC discriminator is rogue time sources within a single PTP domain on
> the same interface. (Added: 2023-10-26)

### Fixed

- Issue SWPTP-984
  - Avoid stale offsets in text stats when sync instance leaves slave state
- Issue SWPTP-968
  - Apply a minimum logging level with `-v` rather than overriding
    `trace_level`
- Issue SWPTP-916
  - Permit DelayReq intervals of up to 32s to match the default profile range
- Issue SWPTP-902/88775
  - Ensure pipes are always sized sufficiently for message queues
- Issue SWPTP-896/88291, SWPTP-892/87934
  - Abort startup if kernel not built with PHC support
- Issue SWPTP-876/84990
  - Fix late message handling by rate-limiting probing for bond/team updates
- Issue SWPTP-866/82382
  - Fix VPD access failures by prefering to read VPD from PCIe config space
- Issue SWPTP-851/79031
  - Fix an issue where the NTP module would hang if an application other than
    chronyd opened UDP port 323. ("Failed to allocate message" symptom.)
  - Add additional tracing options for threading module.
- Issue SWPTP-840/78802
  - Use clearer log message for pending sync instance selection
- Issue SWPTP-837/78323
  - Accept FollowUp messages received before their corresponding Sync message
- Issue SWPTP-708/72389, SWPTP-893/87987
  - Fix consequences of NIC reset with new 'epoch guard' feature SWPTP-908 above
- Issue SWPTP-792/74449
  - Fix hotplug handling of interface renaming
- Issue SWPTP-610/70128
  - Create state path directory before opening message log, allowing the mesage
    log to be created in the same directory (not recommended).

## [3.2.6.1003] - 2018-09-20 [Feature Release]

### Added
- Statistics. Include time of minimum and maximum for each statistics time
  range.
- Sync Instance Selection. Provide option to specify the initial sync
  instance to be selected using the `manual-startup` mode of the
  `selection_policy` directive.

### Fixed
- Issues 77818/76890
  - Bonding. Fix issue where use of `ifdown`/`ifup` scripts on a bond can cause
    sfptpd to exit.
- Issues 76886/67823
  - PTP. Only raise a DelayResp alarm if three consecutive DelayResps fail to
    be received.
- Issue 76096
  - Emit a warning if sfptpd detects that it's been descheduled for too long.
- Issue 76086
  - Allow clock names (e.g. `phc0`) to be used in the `clock_list` directive.
- Issue 75041
  - Emit a warning at startup if the kernel's current clocksource is not TSC.
- Issue 74320
  - Refuse to start sfptpd if multiple instances are using software
    timestamping. Use of software timestamping is not compatible with multiple
    instances.
- Issue 74104
  - Fix logging issue when a sync instance is selected where the wrong decisive
    rule can be reported in some circumstances.
- Issue 73605
  - NTP. Allow startup if ntpd is disciplining the system clock, but the
    `clock_list` configuration directive doesn't include `system`.
- Issue 71479
  - PTP. Refuse to start if interface has no IP address configured.
- Issue 70876
  - Include instance name in state files.

## [3.2.5.1000] - [Not GA]

- Issue 78221:
  - Bonding. Fix issue where after a bonding failover the multicast TTL value
    is not re-initialised correctly.
- Issue 76542:
  - PPS. Fix issue where PPS statistics are missing from the PTP long-term
    stats log when pps logging enabled.
- Issue 76029:
  - Fix issue that causes the daemon to crash at startup when the `clock_list`
    option is used to specify a list of clocks to be disciplined that does not
    include the system clock.

## [3.2.4.1000] - [Not GA]

- Issue 77369:
  - Multiple PTP instances. Fix issue that can cause PTP traffic to be received
    by the wrong PTP instance when an interface is taken down and the IP address
    removed.

## [3.2.3.1000] - [Not GA]

- Issue 75431:
  - Bonding. Fix issue where sfptpd does not detect interfaces in a bond
    being brought up or taken down using `ifup`/`ifdown`.

## [3.2.2.1000] - [Not GA]

- Issue 73989:
  - Fix issue that can cause PHC API to be used when not ready.
- Issue 73875:
  - Fix issue that can cause incorrect times to be recorded in log messages.

## [3.2.1.1004] - 2017-09-19 [Feature Release]

### Added

- Slave event & status reporting via TLVs with remote monitor (EXPERIMENTAL).
- Support for non-Solarflare NICs (EXPERIMENTAL).
- Clock-ctrl-failure alarm added for primary and secondary servos.
- Machine-readable local logging:
  JSON-format long term stats files automatically created;
  To enable JSON Lines stats logging use the option: `json_stats <filename>`
- Support for Meinberg NetSync Monitor extension including MTIE reporting.
  To enable NetSync use the option: `mon_meinberg_netsync`
- Recording of the times of maximum and minimum events in long term stats
  along with qualification status.
- Configurable automatic sync instance selection policy and improved logging
  of decisive factor for selection of a new sync instance.
- Support for NICs to be inserted and removed at runtime when running sfptpd
  over a bond.

### Fixed

- Issue 71792
  - Record Announce intervals received from Foreign Masters when in the
    PTP_LISTENING state and use the longest to determine Foreign Master Record
    expiry rather than using the default Announce interval.
- Issue 71508
  - Fix error in estimated accuracy of NTP synchronization that could cause
    NTP to be incorrectly selected in preference to PTP.
- Issue 71397
  - New `allow-deny`, `deny-allow` ACL order options match behaviour of ptpd2
    and Apache webserver ACLs. Old `permit-deny`, `deny-permit` ACL orders have
    the opposite behaviour and are deprecated but get translated.
- Issue 71264
  - Write log files atomically so that they never appear empty when read.
- Issue 70616
  - Support NTP mode 6 communications (ntpq) in addition to mode 7 (ntpdc).
- Issue 69725
  - Fix issue that causes a crash if IPv6 NTP peers are found.
- Issue 68960
  - Check whether chronyd is running when sfptpd is started.
- Issue 61820
  - Fix issue that can occasionally cause a crash on receipt of a signal.

## [3.0.1.1004] - 2017-05-26

### Fixed

- Issue 71252
  - Fix issue that causes the old adapter clock continue to be used as the
    Local Reference Clock when an active-backup bond fails over to an
    interface associated with a different clock.
- Issue 70127
  - Fix issue that can cause sfptpd to become unresponsive when socket
    communications between sfptpd and the NTP daemon timeout e.g. due to an IP
    Table rule that causes traffic to the NTP daemon to be dropped.
- Issue 70328
  - New configuration option `ptp_delay_resp_ignore_port_id` to allow sfptpd to
    interoperate with switches that don't correctly implement boundary clock
    support when used with LACP.
- Issue 69699
  - Fix reversed display of NTP peer IP addresses.
- Issue 69424
  - Fix interoperability issue with masters that don't go passive,
    e.g. FsmLabs GM.
- Issue 69299
  - Fix file attributes (user, group) in RPM packages.

## [3.0.0.1033] - 2017-02-27 [Feature Release]

### Added

- Support for PTP multiple masters
- Support for NTP fallback
- Improved timestamp filtering and rejection
- Configurable synchronization thresholds for the PTP, PPS and local
  clock servos in order to better interoperate with less accurate time sources.
- New control utility for selecting sync instances in manual selection mode,
  triggering log rotation and causing clocks to be stepped.

### Fixed

- Issue 66361
  - Fix issue that causes sfptpd to refuse to start when operating in freerun
    NTP mode and FsmLabs Timekeeper is used as the NTP Daemon.
- Issue 64057
  - Fix issue that can cause a segmentation fault on account of the incorrect
    handling of management messages containing GET requests.

## [2.4.0.1000] - 2016-07-06 [Feature Release]

### Added

- Support for LACP bonds
- A Configuration option to select which clocks are disciplined by the daemon.

### Fixed

- Issue 60672
  - Fix issue that causes the default configuration in `/etc/sfptpd.conf` to
    be overwritten when upgrading the sfptpd RPM package.
- Issue 60416
  - Fix issue that can cause a memory leak when PTP Management messages are
    received by the daemon but not rejected.
- Issue 57262
  - Fix issue that can cause sfptpd to fail to start if NTPd has no peers.
    This can occur during NTPd startup if the NTPd is unable to quickly resolve
    the DNS names of the peers.
- Issue 53399
  - Fix issue that causes sfptpd to disable timestamping on non-Solarflare
    adapters when the daemon exits.
- Issue 46699
  - Fixed handling of logMessageInterval field in unicast PTP messages
- Issue 45906
  - Fix issue then can cause sfptpd to fail to start if a physical interface
    exists that does not have a permanent MAC address.
- Issue 33716
  - The `ptp_delay_discard_threshold` option does not function correctly and
    support has been removed from this release.

## [2.2.4.70] - 2015-01-30

### Fixed

- Issue 48359
  - Improve filtering and rejection of spurious PPS events by adding a
    configurable outlier filter based on deviation from the mean. Add bad
    PPS signal alarm and statistics to count outliers.
- Issue 46699
  - Fixed handling of `logMessageInterval` field in unicast PTP messages

## [2.2.2.60] - 2014-08-08

### Fixed

- Issue 45921
  - Fixed issue that prevented the daemon being used in software timestamping
    mode with some non-Solarflare adapters.
- Issue 45913
  - Fixed issue that did not allow the daemon to be used with locking disabled
- Issue 45906
  - Improved interface parsing to ignore non-ethernet devices

## [2.2.1.58] - 2014-07-23 [Feature Release]

### Added

- Support for Flareon network adapter SFN7142Q (requires Precision Time
  license).
- Support for synchronizing to a PPS source using NTP to provide Time-of-Day
- Support for PTP management messages
- Access control lists for timing and management messages
- Clock control option to allow clocks to be stepped forwards only
- Configurable UTC offset valid handling for improved interoperability
- Configuration option to ignore lock file.
- Integration of changes from sourceforge PTPD2 project v2.3
- Improved diagnostics including:
  - Report of PTP and timestamping capabilities of each host interface
  - Detection of Transparent Clocks
  - Reporting the set of PTP clocks active in the network
  - Reporting the active PTP domains

### Fixed

- Issue 45758
  - Fixed data sharing violation that could potentially result in stack
    corruption and cause the daemon to crash.
- Issue 44508
  - Fixed issue that can cause the daemon to fail to correctly reconfigure
    SFN6322F/SFN5322F adapters following an abnormal shutdown.
- Issue 42473
  - Fixed issue that can cause the daemon to crash if run on a system
    containing a Solarflare 4000 series adapter.
- Issue 42454
  - Fixed issue that resulted in lock file having non-standard contents when
    operating in daemon mode.
- Issue 41245
  - Add configuration option to allow more than one instance of sfptpd to be
    run simultaneously.
- Issue 41222
  - Improved detection of whether sfptpd is already running: ignore scripts
    with the same name
- Issue 41210
  - Fixed race condition in daemonize operation that can cause lock file
    relocking to fail on some systems

## [2.1.0.34] - 2013-12-05

### Fixed
- Issue 41057
  - Fix issue that can cause frequency correction and state files to be written
    at every sync interval.

## [2.1.0.33] - 2013-12-03 [Feature Release]

### Added
- Support for Flareon network adapaters SFN7322F and SFN7122F
  (SFN7122F requires AppFlex license).
- Improved diagnostics and error reporting, including:
  - Reporting of current clock offsets and error conditions in the topology file
  - Detection of missing Sync, FollowUp and DelayResponse messages
  - Colour coding of statistics output to indicate error conditions
  - Configuration option to run as a daemon
- Enable receive packet timestamping on a set of interfaces
- Optionally leave timestamping enabled when daemon exits
- Specify reference clock in freerun mode

### Fixed
- Issue 40507
  - Improve detection of other running time services (`ntpd`, `ptpd`, `sfptpd`).
    Previous implementation matched any process command line containing one of
    the strings.
- Issue 36755
  - Disabled PTP Management support. In the previous version some Management
    message support was unintentionally enabled.
- Issue 35966
  - Fixed PPS statistics logging. Propagation delay should be subtracted rather
    than added to PPS offset.
- Issue 35558
  - Update topology and state files immediately if any of the PTP parent
    parameters change.
- Issue 35147
  - Added fault restart timer to avoid high CPU usage on continuous fault,
    e.g. if driver unloaded.
- Issue 34587
  - Modified timestamp retrieval to detect and differentiate between different
    failure modes. Ensure correct handling of socket errors in all cases.
  - Modified mechanism to retrieve transmit timestamps to reduce CPU usage in
    the case where the timestamp is returned late or not returned at all.

## [2.0.0.23] - 2013-03-27

### Fixed

- Issue 35391
  - Fixed interoperability issue with Boundary Clocks
- Issue 34693
  - Added configuration file option `message-log` to allow messages to be logged
    to file in addition to statistics.
    Improved log rotation to make it behave more conventionally.
- Issue 34445
  - Fixed issue where sfptpd did not correctly detect and warn if driver version
    too old.
- Issue 34099
  - Improve mean path delay when master to slave propagation delay is small. If
    calculated mean path delay is negative value, use 0 rather than previous
    value.

## [2.0.0.17] - 2012-12-20 [Feature Release]

### Added
- **Solarflare Enhanced PTPd (sfptpd) GA Release**
- Support for PTP over VLANs
- Support for PTP over bonded interfaces
- Support for PTP Hybrid Mode (mixed multicast/unicast)
- Synchronization of multiple PTP capable NICs
- File based configuration
- Saved frequency correction information
- Short and long term statistics collection
