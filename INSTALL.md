# Installation and packaging recipes for sfptpd

The sfptpd daemon can be built by `make all` and used from the `build` directory without installation. This page offers some examples of how sfptpd can be installed or packaged in various scenarios.

(c) Copyright 2022-2023 Advanced Micro Devices, Inc.

## For most recent distributions
```sh
sudo make prefix=/usr install
```

## With sysv init and no Python 3
```sh
sudo make prefix=/usr INST_INITS=sysv INST_OMIT=sfptpmon install
```

## Default operation
Installs to /usr/local

```sh
sudo make install
```

## Into a container image
```sh
make DESTDIR=../staging INST_INITS= install
```

## Building a source RPM
```sh
mkdir -p ~/rpmbuild/SOURCES
ver="$(scripts/sfptpd_versioning derive)"
git archive --prefix="sfptpd-$ver/" --format=tgz -o ~/rpmbuild/SOURCES/sfptpd-$ver.tgz HEAD
curl https://raw.githubusercontent.com/Xilinx-CNS/sfptpd-rpm/generic/sfptpd.spec | sed "s/^\(Version: \).*/\1 $ver/g" > sfptpd.spec
curl https://raw.githubusercontent.com/Xilinx-CNS/sfptpd-rpm/generic/sfptpd.sysusers -o ~/rpmbuild/SOURCES/sfptpd.sysusers
rpmbuild -bs sfptpd.spec
```

## Building a container image
```sh
make patch_version
docker build -f Containerfile .
```

### Running the container image
```sh
sudo docker run \
  --network=host \
  --cap-add NET_BIND_SERVICE,NET_ADMIN,NET_RAW,SYS_TIME \
  $(for d in $(ls /dev/{ptp*,pps*}); do echo "--device $d"; done) \
  -i sfptpd:latest \
  -v -f - < config/default.cfg
```
