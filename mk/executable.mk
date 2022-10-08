# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# Variables and rules to build an executable

# Include this after defining SRCS_$(d) and LIB_$(d)

EXEC_OBJS_$(d) := $(EXEC_SRCS_$(d):%.c=$(BUILD_DIR)/$(d)/%.o)

EXEC_DEPS_$(d) := $(EXEC_OBJS_$(d):%.o=%.d)

TARGETS := $(TARGETS) $(BUILD_DIR)/$(EXEC_$(d))

$(BUILD_DIR)/$(EXEC_$(d)): $(EXEC_OBJS_$(d)) $(LIBRARIES)
			   $(LINK)

$(EXEC_$(d)): $(BUILD_DIR)/$(EXEC_$(d))

-include $(EXEC_DEPS_$(d))

# fin
