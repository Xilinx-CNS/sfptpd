# Solarflare Enhanced PTP Daemon Installation Instructions

The Solarflare Enhanced PTP Daemon works on Linux systems for 3.0 kernels and
later.

(c) Copyright 2013-2023 Xilinx, Inc.

## Building

1) Change directory to the root of the sfptpd source repository or package
2) Type `make all`
3) The executable daemon is `sfptpd` in the `build/` directory

## Execution

The built sfptpd daemon does not require installation. However, it must
normally be run as root.

### Back-to-back PTP example

On host A:

```
sudo build/sfptpd -i eth1 -f config/ptp_master_freerun.cfg
```

On host B:

```
for i in ntp{,d} chrony{,d}; do sudo service $i stop 2>/dev/null; done
sudo build/sfptpd -i eth1 -f config/ptp_slave.cfg
```

## Installation

Some example installation recipes follow.

### For most recent distributions
``` sudo make prefix=/usr install ```

### Example for an installation with sysv init and no Python 3
``` sudo make prefix=/usr INST_INITS=sysv INST_OMIT=sfptpmon install ```

### Default operation
Installs to /usr/local

``` sudo make install ```

### Into a container image
``` make DESTDIR=../staging INST_INITS= install```

### Building a source RPM
```
mkdir -p ~/rpmbuild/SOURCES
ver="$(scripts/sfptpd_versioning derive)"
git archive --prefix="sfptpd-$ver/" --format=tgz -o ~/rpmbuild/SOURCES/sfptpd-$ver.tgz HEAD
curl https://raw.githubusercontent.com/Xilinx-CNS/sfptpd-rpm/generic/sfptpd.spec | sed "s/^\(Version: \).*/\1 $ver/g" > sfptpd.spec
curl https://raw.githubusercontent.com/Xilinx-CNS/sfptpd-rpm/generic/sfptpd.sysusers -o ~/rpmbuild/SOURCES/sfptpd.sysusers
rpmbuild -bs sfptpd.spec
```

### Building a container image
```
docker build .
```

### Running a container image
```
sudo docker run \
  --network=host \
  --cap-add NET_BIND_SERVICE,NET_ADMIN,NET_RAW,SYS_TIME \
  $(for d in $(ls /dev/{ptp*,pps*}); do echo "--device $d"; done) \
  -i sfptpd:latest \
  -v -f - < config/default.cfg
```
