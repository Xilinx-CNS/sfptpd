# SPDX-License-Identifier: BSD-2-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# PTPD2 Library makefile

include mk/pushd.mk


LIB_SRCS_$(d) := arith.c \
	bmc.c \
	display.c \
	management.c \
	protocol.c \
	ptpd_lib.c \
	foreign.c \
	dep/ipv4_acl.c \
	dep/msg.c \
	dep/net.c \
	dep/servo.c \
	dep/sys.c \
	dep/timer.c \
	dep/ntpengine/ntp_isc_md5.c \
	evtmon.c \
	monitor.c

LIB_$(d) := ptpd2


include mk/library.mk
include mk/popd.mk

# fin
