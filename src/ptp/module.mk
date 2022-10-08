# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# PTP Sync Module makefile

include mk/pushd.mk


# Include the makefiles for any subdirectories

dir := $(d)/ptpd2
include $(dir)/module.mk


LIB_SRCS_$(d) := sfptpd_ptp_module.c sfptpd_ptp_config.c \
	sfptpd_ptp_timestamp_dataset.c \
	sfptpd_ptp_monitor.c

LIB_$(d) := ptp


include mk/library.mk
include mk/popd.mk

# fin
