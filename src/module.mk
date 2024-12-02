# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2022 Xilinx, Inc.

# sfptpd core makefile

include mk/pushd.mk

# Include the makefiles for any subdirectories

dir := $(d)/freerun
include $(dir)/module.mk
dir := $(d)/pps
include $(dir)/module.mk
dir := $(d)/ptp
include $(dir)/module.mk
dir := $(d)/ntp
include $(dir)/module.mk
dir := $(d)/crny
include $(dir)/module.mk
dir := $(d)/sfptpdctl
include $(dir)/module.mk
dir := $(d)/priv_helper
include $(dir)/module.mk
ifndef NO_GPS
dir := $(d)/gps
include $(dir)/module.mk
endif

# Local variables

INCDIRS += -I$(d)/include -I$(d)/ptp/ptpd2  -I$(d)/ptp/ptpd2/dep

LIB_SRCS_$(d) := sfptpd_logging.c sfptpd_config.c sfptpd_engine.c \
	sfptpd_servo.c sfptpd_clock.c sfptpd_time.c sfptpd_statistics.c \
	sfptpd_sync_module.c sfptpd_filter.c sfptpd_interface.c \
	sfptpd_misc.c sfptpd_thread.c sfptpd_general_config.c \
	sfptpd_bic.c sfptpd_control.c \
	sfptpd_netlink.c sfptpd_phc.c sfptpd_db.c \
	sfptpd_app.c sfptpd_link.c \
	sfptpd_clockfeed.c \
	sfptpd_priv.c \
	sfptpd_multicast.c

LIB_$(d) := common

include mk/library.mk

EXEC_SRCS_$(d) := sfptpd_main.c

EXEC_$(d) := sfptpd

dir := $(d)/tstool
include $(dir)/module.mk

include mk/executable.mk

include mk/popd.mk

# fin
