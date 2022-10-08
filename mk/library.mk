# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# Variables and rules to build a library

# Include this after defining LIB_SRCS_$(d) and LIB_$(d)

LIB_OBJS_$(d) := $(LIB_SRCS_$(d):%.c=$(BUILD_DIR)/$(d)/%.o)

LIB_DEPS_$(d) := $(LIB_OBJS_$(d):%.o=%.d)

LIBRARIES := $(BUILD_DIR)/$(d)/lib$(LIB_$(d)).a $(LIBRARIES)

$(BUILD_DIR)/$(d)/lib$(LIB_$(d)).a: $(LIB_OBJS_$(d))
				    $(ARCHIVE)

-include $(LIB_DEPS_$(d))

# fin
