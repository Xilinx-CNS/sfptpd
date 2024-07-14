# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2024 Advanced Micro Devices, Inc.

# sfptpdctl makefile

include mk/pushd.mk

# Include the makefiles for any subdirectories

# Local variables

EXEC_SRCS_$(d) := tstool.c

EXEC_$(d) := tstool

include mk/executable.mk

include mk/popd.mk

# fin
