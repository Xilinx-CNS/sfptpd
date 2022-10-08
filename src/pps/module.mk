# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# PPS Sync Mode Makefile

include mk/pushd.mk


LIB_SRCS_$(d) := sfptpd_pps_module.c

LIB_$(d) := pps


include mk/library.mk
include mk/popd.mk

# fin
