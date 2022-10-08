# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# Set up some rules

.SUFFIXES:
.SUFFIXES:		.c .o

# Add .d to Make's recognized suffixes.
SUFFIXES += .d

all:			targets


# General directory independent rules

$(BUILD_DIR)/%.o:	%.c
			$(MKDIR) $(dir $@)
			$(COMPILE)

$(BUILD_DIR)/%:		%.o
			$(MKDIR) $(dir $@)
			$(ARCHIVE)


# fin
