# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# Include this at the end of all sub-makefiles

# Remove current directory from stack
d := $(dirstack_$(sp))
sp := $(basename $(sp))

# fin
