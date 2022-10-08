# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# NTP Sync Mode Makefile

include mk/pushd.mk


LIB_SRCS_$(d) := sfptpd_ntp_module.c \
	sfptpd_ntpd_client_mode6.c sfptpd_ntpd_client_mode7.c \
	sfptpd_ntpd_client.c

LIB_$(d) := ntp


include mk/library.mk
include mk/popd.mk

# fin
