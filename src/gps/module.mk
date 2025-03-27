# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2023, Advanced Micro Devices, Inc.

# GPS Sync Module Makefile

include mk/pushd.mk


LIB_SRCS_$(d) := sfptpd_gps_module.c

LIB_$(d) := gps


include mk/library.mk
include mk/popd.mk

# fin
