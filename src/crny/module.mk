# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# Chrony Sync Module Makefile

include mk/pushd.mk


LIB_SRCS_$(d) := sfptpd_crny_module.c \
	sfptpd_crny_helper.c

LIB_$(d) := crny


include mk/library.mk
include mk/popd.mk

# fin
