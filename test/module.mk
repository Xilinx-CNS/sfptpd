# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# sfptpd test makefile

include mk/pushd.mk

# Include the makefiles for any subdirectories

#dir := $(d)/subdir
#include $(dir)/module.mk


# Local variables

INCDIRS += -I$(d)/../src/include -I$(d)/../src/ptp/ptpd2  -I$(d)/../src/ptp/ptpd2/dep

EXEC_SRCS_$(d) := sfptpd_test.c sfptpd_test_config.c sfptpd_test_ht.c \
		  sfptpd_test_stats.c sfptpd_test_filters.c sfptpd_test_threading.c \
		  sfptpd_test_bic.c sfptpd_test_fmds.c sfptpd_test_link.c \
		  sfptpd_test_time.c

EXEC_$(d) := sfptpd_test

include mk/executable.mk

include mk/popd.mk

# fin
