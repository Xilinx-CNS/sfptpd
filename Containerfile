# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2023 Advanced Micro Devices, Inc.
# hadolint global ignore=DL3006,DL3059,DL3041,SC1091
ARG UBI_IMAGE=redhat/ubi9-minimal:latest
ARG SFPTPD_VERSION

FROM $UBI_IMAGE AS builder
ARG SFPTPD_VERSION
WORKDIR /src

# hadolint ignore=DL3040
RUN microdnf --setopt install_weak_deps=0 install -y \
  redhat-rpm-config make gcc libmnl-devel libcap-devel

# Use build flags from redhat-rpm-config to select the same optimisations
# and hardening chosen by base platform
RUN echo "export \
  CFLAGS=\"$(rpm --eval '%{__global_cflags}')\" \
  LDFLAGS=\"$(rpm --eval '%{__global_ldflags}')\"" \
  > /build.env

# Build sfptpd
WORKDIR /src/sfptpd
RUN --mount=target=. \
  source /build.env \
  && make install BUILD_DIR=/src/sfptpd-build DESTDIR=/staging prefix=/usr INST_INITS= \
  && cp LICENSE /staging

FROM $UBI_IMAGE AS runtime
ARG SFPTPD_VERSION
LABEL \
  name="sfptpd" \
  summary="sfptpd" \
  description="Solarflare Enhanced PTP Daemon" \
  maintainer="Advanced Micro Devices, Inc." \
  vendor="Advanced Micro Devices, Inc." \
  version="$SFPTPD_VERSION" \
  release="$SFPTPD_VERSION"
RUN microdnf install -y libmnl && microdnf clean all
COPY --from=builder /staging /
COPY --from=builder /staging/LICENSE /licenses/
WORKDIR /var/lib/sfptpd

# Override any 'daemon' setting in selected configuration.
# Select a default configuration that can be overriden by runtime arguments.
ENTRYPOINT ["/usr/sbin/sfptpd", "--no-daemon", "-f", "/usr/share/doc/sfptpd/config/default.cfg" ]

# Send output to the console if running default config without arguments
CMD ["--console"]
