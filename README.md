# Solarflare Enhanced PTP Daemon Installation Instructions

The Solarflare Enhanced PTP Daemon works on Linux systems for 3.0 kernels and
later.

(c) Copyright 2013, 2022 Xilinx, Inc.

## Building

1) Change directory to the root of the sfptpd source repository or package
2) Type `make all`
3) The executable daemon is `sfptpd` in the `build/` directory

## Execution

The built sfptpd daemon does not require installation. However, it must be run
as root.

## Installation

Some example installation recipes follow.

### Default (to `/usr/local`)
``` sudo make install ```

### RHEL 6
``` sudo make PREFIX=/usr INST_INITS=sysv install ```

### Other distributions
``` sudo make PREFIX=/usr install ```

### Building a source RPM
```
ver="$(sed -n s,'^#define SFPTPD_VERSION_TEXT.*"\(.*\)"',\\1,gp < src/include/sfptpd_version.h)~$(date +%Y%M%d).git$(git rev-parse --short HEAD)"
mkdir -p ~/rpmbuild/SOURCES
git archive --prefix="sfptpd-$ver/" --format=tgz -o ~/rpmbuild/SOURCES/sfptpd-$ver.tgz
curl https://raw.githubusercontent.com/Xilinx-CNS/sfptpd-rpm/generic/sfptpd.spec | sed -e "s/^\(Version: \).*/\1 $ver/g" -e "s/^\(Release: \).*/\1 1%{?dist}/g" > sfptpd.spec
rpmbuild -bs sfptpd.spec
```
