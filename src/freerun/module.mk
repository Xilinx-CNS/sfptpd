# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# Freerun Sync Module makefile

include mk/pushd.mk


LIB_SRCS_$(d) := sfptpd_freerun_module.c

LIB_$(d) := freerun


include mk/library.mk
include mk/popd.mk

# fin
