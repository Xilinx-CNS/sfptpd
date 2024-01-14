# RPM definitions for sfptpd

The top-level makefile includes a rule for building sfptpd source RPMs:

```sh
make build_srpm
```

This script uses the build system's default rpm macro value for `%{dist}` to
choose which spec file to use for the target. If RPMs are alien to the build
host, the appropriate match is imprecise or if cross-building, this value
needs to be overridden, e.g.

```sh
RPM_OSVER=el9 make build_srpm
```

The main github repository for sfptpd includes a workflow action that generates
test RPM packages. The generated (unsupported) RPMs are available for test by
clicking on the commit log summary line in the following index:
<https://github.com/Xilinx-CNS/sfptpd/actions/workflows/rpm.yml>

## Key differences across OS/rpm versions

### Unique to EL9

* Creates an `sfptpd` user
   - uses `sysusers.d`
   - includes `udev.d` rules
   - state files to be owned by `sfptpd` user
   - comments out `SFPTPD_USER` setting in `/etc/sysconfig/sfptpd` because
     chronyd integration cannot work as a non-root user, unless that user
     is the same as chronyd!
   - users only have to uncomment out the above setting to run as a non-root
     user if appropriate for them

### EL8

The EL8 spec file is used for the generic EL8+ RPM released and supported by
AMD Solarflare at <https://www.xilinx.com/download/drivers> as of v3.7.

Quirks for EL8 and later:

* Explicit `%set_build_flags` macro used because in containerised github
  rpmbuild workflows these are not automatically being set. The standard build
  flags are desired to ensure the built binary is to the hardening standards
  expected of the target distribution.

### Changes in EL7 wrt EL8

* Post-release versioning (`sfptpd-X.Y.Z.W^20240114.git1234567`) is not
  supported in older RPM versions so pre-release versioning with `~` instead of
  `^` is used for post-release versions. This is handled in `mk/rpm.mk`.
* Disable Python bytecode generation for python subpackage
* `systemd-rpm-macros` package does not exist in EL7 so we need the `systemd`
  package to bring in the standard macros for installing systemd services.

### Changes in EL6 wrt EL7

EL6 is not a supported platform from v3.7 onwards but at the time of writing,
sfptpd can be built and executed with no known issues. One of the following
approaches is needed:

1. Build on EL7 with glibc compat (used by github workflow).
2. Build on EL6 with backported kernel headers (untested).
3. Revert commit 31161af and reinstate any kernel compat defs (untested).

* Old-style license summary field does not take SPDX expressions
* Build root specified and cleaned
* Package documentation directory needs defining
* Use Python2 for scripts and exclude Python3-only `sfptpmon`
* Define `GLIBC_COMPAT` for cross-building to ensure compatible symbol
  versions are used.
* There is no separate license file location
* Use sysv init script instead of systemd service

## Updating spec files

1. Please make changes first for the most recent OS version, i.e. `el9`.
2. Then merge manually to progressively older versions,
   i.e. `el9`->`el8`->`el7`->`el6`.

e.g.:

```sh
cd scripts/rpm/el7
diff -- ../el8/sfptpd.spec | patch -u --merge
```

Use the `--dry-run` option with `patch` to check what will be changed in
advance.
