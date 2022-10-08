/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

#ifndef _SFPTPD_BIC_H
#define _SFPTPD_BIC_H

#include <stdbool.h>

#include "sfptpd_sync_module.h"
#include "sfptpd_instance.h"

/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Selection strategy */
enum sfptpd_selection_strategy {
	SFPTPD_SELECTION_STRATEGY_AUTOMATIC,
	SFPTPD_SELECTION_STRATEGY_MANUAL,
	SFPTPD_SELECTION_STRATEGY_MANUAL_STARTUP,
};

enum sfptpd_selection_rule {
	/* The list of rules is zero-terminated */
	SELECTION_RULE_END = 0,

	/* The tie-break rule becomes an implied one at the end */
	SELECTION_RULE_TIE_BREAK = SELECTION_RULE_END,

	SELECTION_RULE_MANUAL,
	SELECTION_RULE_STATE,
	SELECTION_RULE_NO_ALARMS,
	SELECTION_RULE_USER_PRIORITY,
	SELECTION_RULE_CLUSTERING,
	SELECTION_RULE_CLOCK_CLASS,
	SELECTION_RULE_TOTAL_ACCURACY,
	SELECTION_RULE_ALLAN_VARIANCE,
	SELECTION_RULE_STEPS_REMOVED,

	/* This constant is used as a count of the number of
	   available rules. */
	SELECTION_RULE_MAX,
};


struct sfptpd_selection_policy {
	enum sfptpd_selection_strategy strategy;
	enum sfptpd_selection_rule rules[SELECTION_RULE_MAX];
};


/****************************************************************************
 * Constants
 ****************************************************************************/

/** Clustering modes */
enum sfptpd_clustering_mode {
        SFPTPD_CLUSTERING_DISABLED,
        SFPTPD_CLUSTERING_MODE_DISCRIMINATOR,
};

extern const char *sfptpd_selection_rule_names[SELECTION_RULE_MAX];
extern const struct sfptpd_selection_policy sfptpd_default_selection_policy;

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Choose a Best Instance Clock from a number of instances.
 *
 * @param instance_statuses Array of instance records
 * @param num_instances The number of elements in @ref instance_statuses.
 * @return instance NULL no suitable instance, otherwise selected instance
 */
struct sync_instance_record *sfptpd_bic_choose(const struct sfptpd_selection_policy *policy,
					       struct sync_instance_record *instance_records,
					       int num_instances);

/** Manually select a Best Instance Clock
 *
 * @param instance_statuses Array of instance records
 * @param num_instances The number of elements in @ref instance_statuses.
 * @param selected_instance The instance to be selected
 */
void sfptpd_bic_select_instance(struct sync_instance_record *instance_records,
				int num_instances,
				struct sync_instance_record *selected_instance);

#endif /* _SFPTPD_BIC_H */
