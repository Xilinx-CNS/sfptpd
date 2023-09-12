# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2023 Advanced Micro Devices, Inc.

ARG UBI9_VERSION=latest

FROM registry.access.redhat.com/ubi9-minimal:${UBI9_VERSION} AS builder
WORKDIR /src

# git used to derive version number when run from a git clone
RUN microdnf -y install git tar bzip2 redhat-rpm-config make gcc

# Obtain headers from source packages in lieu of libmnl-devel and libcap-devel
RUN microdnf -y --enablerepo=ubi-9-baseos-source download --source libmnl libcap
RUN mkdir libcap libmnl
RUN rpm -i libcap-*.src.rpm \
 && tar xzf ~/rpmbuild/SOURCES/libcap-*.tar.gz --strip-components=1 -C libcap \
 && make -C libcap/libcap install
RUN rpm -i libmnl-*.src.rpm \
 && tar xjf ~/rpmbuild/SOURCES/libmnl-*.tar.bz2 --strip-components=1 -C libmnl \
 && { cd libmnl && ./configure --prefix=/usr && make install; }

# Use build flags from redhat-rpm-config to select the same optimisations
# and hardening chosen by base platform
RUN echo \
  export CFLAGS=\"$(rpm --eval %{__global_cflags})\" \
  export LDFLAGS=\"$(rpm --eval %{__global_ldflags})\" \
  > /build.env

# Build sfptpd
COPY . /src/sfptpd
WORKDIR /src/sfptpd
RUN make patch_version
RUN . /build.env && DESTDIR=/staging INST_INITS= make install

FROM registry.access.redhat.com/ubi9-minimal:${UBI9_VERSION} AS runtime
RUN microdnf install -y libmnl
COPY --from=builder /staging /
WORKDIR /var/lib/sfptpd

# Override any 'daemon' setting in selected configuration.
# Select a default configuration that can be overriden by runtime arguments.
ENTRYPOINT ["/usr/sbin/sfptpd", "--no-daemon", "-f", "/usr/share/doc/sfptpd/config/default.cfg" ]

# Send output to the console if running default config without arguments
CMD ["--console"]
