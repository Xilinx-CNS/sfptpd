# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2023 Xilinx, Inc.

# Chrony Sync Module Makefile

include mk/pushd.mk


LIB_SRCS_$(d) := sfptpd_gps_module.c

LIB_$(d) := gps


include mk/library.mk
include mk/popd.mk

# fin
