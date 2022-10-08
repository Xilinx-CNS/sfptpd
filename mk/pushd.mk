# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# Include this at the start of all sub-makefiles

# Add new directory to stack
sp := $(sp).x
dirstack_$(sp) := $(d)
d := $(dir)

# fin
