# Installation and packaging recipes for sfptpd

The sfptpd daemon can be built by `make all` and used from the `build`
directory without installation. This page offers some examples of how sfptpd
can be installed or packaged in various scenarios.

(c) Copyright 2022-2025 Advanced Micro Devices, Inc.

## Default operation, installing to `/usr/local`

```sh
sudo make install
```

## Install to `/usr`

```sh
sudo make prefix=/usr install
```

## Build a debian package

```sh
debuild -i -us -uc -b
```

## Building a source RPM

```sh
make build_srpm
```

Read more about [RPM builds](scripts/rpm/README.md).

## Into a container image

```sh
make DESTDIR=../staging INST_INITS= install
```

## Build an RPM for EL6 on EL7 build host

```sh
RPM_OSVER=el6 make build_srpm
```

Then invoke `rpmbuild --rebuild` on the generated `el6.src.rpm`

Add the following to the sfptpd config:

```
run_dir /var/run/sfptpd
```

## Building a container image

```sh
make patch_version
docker build -f Containerfile .
```

### Running the container image

This example feeds a local configuration into the container via stdin for
convenience. In production it would be appropriate to map a configuration
file through instead.

```sh
sudo docker run \
  --network=host \
  --cap-add NET_BIND_SERVICE,NET_ADMIN,NET_RAW,SYS_TIME \
  $(for d in $(ls /dev/{ptp*,pps*}); do echo "--device $d"; done) \
  -i sfptpd:latest \
  -v -f - < config/default.cfg
```

### Running a pre-built container image

This **one-line quick start** example invokes a built-in example configuration, specifying
a default interface and PTP domain on the command line.

```sh
sudo docker run \
  --network=host \
  --cap-add NET_BIND_SERVICE,NET_ADMIN,NET_RAW,SYS_TIME \
  $(for d in $(ls /dev/ptp*); do echo "--device $d"; done) \
  ghcr.io/xilinx-cns/sfptpd:master-dev -f /config/ptp_master_freerun.cfg \
  -i bond0 -D 100
```

Host networking is not required if a MACVLAN or IPVLAN is mapped through to the
container but due to security limitations in the kernel hardware timetamping
needs to be enabled in the host namespace if this is done. A suitable command
for doing this is:

```sh
tstool interface set_ts ens1f0 on all
```

Note that the sfptpd running within a container cannot communicate with
chronyd, so chronyd must be disabled.

## With no Python 3

```sh
sudo make prefix=/usr INST_OMIT=sfptpmon install
```

## Service definitions and non-systemd systems

Service startup scripts and definitions are usually short enough that it is
best to devise one for the system at hand rather than using a generic one but
the following service definitions are available in this repository:

- [systemd unit](scripts/systemd/sfptpd.service)
- [Debian-standard init script](debian/sfptpd.init)
- [Debian-standard runit service directory](scripts/runit)
- [RHEL 6-standard init script](scripts/rpm/el6/sfptpd.init)
