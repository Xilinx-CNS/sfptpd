/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_bic.c
 * @brief  Select best instance clock from different instances
 */

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <math.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <pthread.h>

#include "sfptpd_logging.h"
#include "sfptpd_bic.h"


/****************************************************************************
 * Macros
 ****************************************************************************/

/* BIC component specific trace */
#define DBG_L1(x, ...)  TRACE(SFPTPD_COMPONENT_ID_BIC, 1, x, ##__VA_ARGS__)
#define DBG_L2(x, ...)  TRACE(SFPTPD_COMPONENT_ID_BIC, 2, x, ##__VA_ARGS__)
#define DBG_L3(x, ...)  TRACE(SFPTPD_COMPONENT_ID_BIC, 3, x, ##__VA_ARGS__)
#define DBG_L4(x, ...)  TRACE(SFPTPD_COMPONENT_ID_BIC, 4, x, ##__VA_ARGS__)
#define DBG_L5(x, ...)  TRACE(SFPTPD_COMPONENT_ID_BIC, 5, x, ##__VA_ARGS__)
#define DBG_L6(x, ...)  TRACE(SFPTPD_COMPONENT_ID_BIC, 6, x, ##__VA_ARGS__)


/****************************************************************************
 * Types and Structures
 ****************************************************************************/

struct ordered_instance {
	struct sync_instance_record *record;
	int decisive_rule_index;
};


/****************************************************************************
 * Constants
 ****************************************************************************/

const struct sfptpd_selection_policy sfptpd_default_selection_policy = {
	SFPTPD_SELECTION_STRATEGY_AUTOMATIC,
	{
		SELECTION_RULE_MANUAL,
		SELECTION_RULE_EXT_CONSTRAINTS,
		SELECTION_RULE_STATE,
		SELECTION_RULE_NO_ALARMS,
		SELECTION_RULE_USER_PRIORITY,
		SELECTION_RULE_CLUSTERING,
		SELECTION_RULE_CLOCK_CLASS,
		SELECTION_RULE_TOTAL_ACCURACY,
		SELECTION_RULE_ALLAN_VARIANCE,
		SELECTION_RULE_STEPS_REMOVED,

		/* The tie-break rule is implied at the end. */
		SELECTION_RULE_END
	}
};

const char *sfptpd_selection_rule_names[SELECTION_RULE_MAX] = {
	[SELECTION_RULE_MANUAL] = "manual",
	[SELECTION_RULE_EXT_CONSTRAINTS] = "ext-constraints",
	[SELECTION_RULE_STATE] = "state",
	[SELECTION_RULE_NO_ALARMS] = "no-alarms",
	[SELECTION_RULE_USER_PRIORITY] = "user-priority",
	[SELECTION_RULE_CLUSTERING] = "clustering",
	[SELECTION_RULE_CLOCK_CLASS] = "clock-class",
	[SELECTION_RULE_TOTAL_ACCURACY] = "total-accuracy",
	[SELECTION_RULE_ALLAN_VARIANCE] = "allan-variance",
	[SELECTION_RULE_STEPS_REMOVED] = "steps-removed",
	[SELECTION_RULE_TIE_BREAK] = "tie-break"
};

/* When making a comparison, the state of each sync module is converted
 * to a priority where a lower value signifies a higher priority. */
const int sfptpd_state_priorities[SYNC_MODULE_STATE_MAX] = {
	[SYNC_MODULE_STATE_LISTENING] = 1,
	[SYNC_MODULE_STATE_SLAVE] = 0,
	[SYNC_MODULE_STATE_MASTER] = 2,
	[SYNC_MODULE_STATE_PASSIVE] = 2,
	[SYNC_MODULE_STATE_DISABLED] = 3,
	[SYNC_MODULE_STATE_FAULTY] = 3,
	[SYNC_MODULE_STATE_SELECTION] = 1
};


/****************************************************************************
 * Function prototypes
 ****************************************************************************/

static struct sync_instance_record *sfptpd_bic_select(const struct sfptpd_selection_policy *policy,
						      struct sync_instance_record *instance_record_a,
						      struct sync_instance_record *instance_record_b,
						      int *decisive_rule_index,
						      const char *phase);


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static const char *get_selection_rule_name(enum sfptpd_selection_rule rule)
{
	assert(rule < SELECTION_RULE_MAX);
	return sfptpd_selection_rule_names[rule];
}


static int ext_constraint_priority(sfptpd_sync_module_constraints_t constraints)
{
	if (SYNC_MODULE_CONSTRAINT_TEST(constraints, MUST_BE_SELECTED))
		return -1;
	else if(SYNC_MODULE_CONSTRAINT_TEST(constraints, CANNOT_BE_SELECTED))
		return 1;
	else
		return 0;
}


/** Select between two instances for the "best" clock
 *
 * See sfptpd_config.h for selection algorithm
 *
 * @param policy The selection policy
 * @param instance_record_a First instance
 * @param instance_record_b Second instance
 * @param decisive_rule_index Returning the index of the rule the was decisive
 * @return instance selected
 */
static struct sync_instance_record *sfptpd_bic_select(const struct sfptpd_selection_policy *policy,
						      struct sync_instance_record *instance_record_a,
						      struct sync_instance_record *instance_record_b,
						      int *decisive_rule_index,
						      const char *phase)
{
	struct sfptpd_sync_instance_status *status_a;
	struct sfptpd_sync_instance_status *status_b;
	double total_accuracy_a;
	double total_accuracy_b;
	int state_priority_a;
	int state_priority_b;
	struct sync_instance_record *choice;
	int rule_idx;
	int difference;
	char constraints_a[SYNC_MODULE_CONSTRAINT_ALL_TEXT_MAX];
	char constraints_b[SYNC_MODULE_CONSTRAINT_ALL_TEXT_MAX];

	assert (NULL != instance_record_a);
	assert (NULL != instance_record_b);

	status_a = &instance_record_a->status;
	status_b = &instance_record_b->status;

	assert(status_a->state < SYNC_MODULE_STATE_MAX);
	assert(status_b->state < SYNC_MODULE_STATE_MAX);

	choice = NULL;

	DBG_L3("selection%s: comparing %s and %s\n",
	       phase,
	       instance_record_a->info.name,
	       instance_record_b->info.name);

	for (rule_idx = 0; choice == NULL; rule_idx++) {
		enum sfptpd_selection_rule rule = policy->rules[rule_idx];
		const char *rule_name = sfptpd_selection_rule_names[rule];

		switch (policy->rules[rule_idx]) {
		case SELECTION_RULE_MANUAL:
			DBG_L3("selection%s:   comparing %s: %s, %s\n",
			       phase, rule_name,
			       instance_record_a->selected ? "manually-selected" : "not-manually-selected",
			       instance_record_b->selected ? "manually-selected" : "not-manually-selected");

			if (instance_record_a->selected)
				choice = instance_record_a;
			else if (instance_record_b->selected)
				choice = instance_record_b;
			break;
		case SELECTION_RULE_EXT_CONSTRAINTS:
			sfptpd_sync_module_constraints_text(status_a->constraints,
							    constraints_a, sizeof constraints_a);
			sfptpd_sync_module_constraints_text(status_b->constraints,
							    constraints_b, sizeof constraints_b);

			DBG_L3("selection%s:   comparing %s: [%s], [%s]\n",
			       phase, rule_name, constraints_a, constraints_b);

			difference = ext_constraint_priority(status_a->constraints) -
				     ext_constraint_priority(status_b->constraints);
			if (difference < 0)
				choice = instance_record_a;
			else if (difference > 0)
				choice = instance_record_b;
			break;
		case SELECTION_RULE_STATE:
			state_priority_a = sfptpd_state_priorities[status_a->state];
			state_priority_b = sfptpd_state_priorities[status_b->state];

			DBG_L3("selection%s:   comparing %s: %s (%d), %s (%d)\n",
			       phase, rule_name,
			       sync_module_state_text[status_a->state], state_priority_a,
			       sync_module_state_text[status_b->state], state_priority_b);

			if (state_priority_a < state_priority_b) {
				choice = instance_record_a;
			} else if (state_priority_a > state_priority_b) {
				choice = instance_record_b;
			}
			break;
		case SELECTION_RULE_NO_ALARMS:
			DBG_L3("selection%s:   comparing %s: %s, %s\n",
			       phase, rule_name,
			       status_a->alarms == 0 ? "no-alarms" : "alarms",
			       status_b->alarms == 0 ? "no-alarms" : "alarms");

			if ((status_a->alarms == 0) && (status_b->alarms != 0)) {
				choice = instance_record_a;
			} else if ((status_a->alarms != 0) && (status_b->alarms == 0)) {
				choice = instance_record_b;
			}
			break;
		case SELECTION_RULE_USER_PRIORITY:
			DBG_L3("selection%s:   comparing %s: %d, %d\n",
			       phase, rule_name,
			       status_a->user_priority, status_b->user_priority);

			if (status_a->user_priority < status_b->user_priority) {
				choice = instance_record_a;
			} else if (status_a->user_priority > status_b->user_priority) {
				choice = instance_record_b;
			}
			break;
		case SELECTION_RULE_CLUSTERING:
			DBG_L3("selection%s:   comparing %s: %d, %d\n",
			       phase, rule_name,
			       status_a->clustering_score, status_b->clustering_score);

			if (status_a->clustering_score > status_b->clustering_score) {
				choice = instance_record_a;
			} else if (status_a->clustering_score < status_b->clustering_score) {
				choice = instance_record_b;
			}
			break;
		case SELECTION_RULE_CLOCK_CLASS:
			DBG_L3("selection%s:   comparing %s: %s (%d), %s (%d)\n",
			       phase, rule_name,
			       sfptpd_clock_class_text(status_a->master.clock_class), status_a->master.clock_class,
			       sfptpd_clock_class_text(status_b->master.clock_class), status_b->master.clock_class);
			if (status_a->master.clock_class < status_b->master.clock_class) {
				choice = instance_record_a;
			} else if (status_a->master.clock_class > status_b->master.clock_class) {
				choice = instance_record_b;
			}
			break;
		case SELECTION_RULE_TOTAL_ACCURACY:
			total_accuracy_a = status_a->master.accuracy + status_a->local_accuracy;
			total_accuracy_b = status_b->master.accuracy + status_b->local_accuracy;

			DBG_L3("selection%s:   comparing %s: %lf, %lf\n",
			       phase, rule_name,
			       total_accuracy_a, total_accuracy_b);

			if (total_accuracy_a < total_accuracy_b) {
				choice = instance_record_a;
			} else if (total_accuracy_a > total_accuracy_b) {
				choice = instance_record_b;
			}
			break;
		case SELECTION_RULE_ALLAN_VARIANCE:
			DBG_L3("selection%s:   comparing %s: %Lg, %Lg\n",
			       phase, rule_name,
			       status_a->master.allan_variance, status_b->master.allan_variance);

			if (status_a->master.allan_variance < status_b->master.allan_variance) {
				choice = instance_record_a;
			} else if (status_a->master.allan_variance > status_b->master.allan_variance) {
				choice = instance_record_b;
			}
			break;
		case SELECTION_RULE_STEPS_REMOVED:
			DBG_L3("selection%s:   comparing %s: %d, %d\n",
			       phase, rule_name,
			       status_a->master.steps_removed, status_b->master.steps_removed);

			if (status_a->master.steps_removed < status_b->master.steps_removed) {
				choice = instance_record_a;
			} else if (status_a->master.steps_removed > status_b->master.steps_removed) {
				choice = instance_record_b;
			}
			break;
		case SELECTION_RULE_TIE_BREAK:
			/* Indeterminate but need a deterministic answer: choose lowest pointer. */
			DBG_L3("selection%s: can't decide between instance clocks %s and %s: settling with %s\n",
			       phase,
			       instance_record_a->info.name,
			       instance_record_b->info.name,
			       instance_record_a->info.name);

			/* Assigning to 'choice' breaks the loop. */
			choice = instance_record_a < instance_record_b ? instance_record_a : instance_record_b;
			break;
		default:
			assert(!"Invalid selection rule in policy");
		}
	}

	/* Rule incremented at the end of the loop as there are no break-outs,
	   so adjust to refer to the last rule. */
	rule_idx--;

	assert(choice != NULL);

	if (decisive_rule_index != NULL) {
		*decisive_rule_index = rule_idx;
	}

	DBG_L2("selection%s: in comparison, preferring %s to %s by rule %s (%d)\n",
	       phase,
	       choice->info.name,
	       choice == instance_record_a ? instance_record_b->info.name : instance_record_a->info.name,
	       get_selection_rule_name(policy->rules[rule_idx]),
	       rule_idx);

	return choice;
}


static int ordered_instance_compar(const void *a, const void *b, void *context) {
	const struct sfptpd_selection_policy *policy = (struct sfptpd_selection_policy *) context;

	struct ordered_instance *aa = (struct ordered_instance *) a;
	struct ordered_instance *bb = (struct ordered_instance *) b;
	struct sync_instance_record *better;

	better = sfptpd_bic_select(policy, aa->record, bb->record, NULL, "(sorting)");

	return better == aa->record ? -1 : 1;
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct sync_instance_record *sfptpd_bic_choose(const struct sfptpd_selection_policy *policy,
					       struct sync_instance_record *instance_records,
					       int num_instances)
{
	struct sync_instance_record *result;
	struct ordered_instance *list;
	int i;

	if (num_instances == 0) {
		WARNING("No instances from which to select a sync instance\n");
		return NULL;
	} else if (num_instances == 1) {
		TRACE_L3("selection: %s is only candidate from which to choose\n",
		     instance_records[0].info.name);
		return &instance_records[0];
	}

	/* Create a list of records to sort */
	list = malloc(num_instances * sizeof *list);
	for (i = 0; i < num_instances; i++) {
		list[i].record = &instance_records[i];
	}

	qsort_r(list, num_instances, sizeof *list, ordered_instance_compar, (void *) policy);

	/* Fill in decisive rules */
	for (i = 0; i < num_instances - 1; i++) {
		sfptpd_bic_select(policy, list[i].record, list[i+1].record,
				  &list[i].decisive_rule_index,
				  "(checking-decisive-rule)");
		INFO("selection: rank %i: %s by rule %s (%d)%s\n",
		     i + 1,
		     list[i].record->info.name,
		     get_selection_rule_name(policy->rules[list[i].decisive_rule_index]),
		     list[i].decisive_rule_index,
		     i == 0 ? " <- BEST" : "");

		/* Record rank in the record for diagnostic use only */
		list[i].record->rank = i + 1;
	}
	assert(num_instances > 1);
	INFO("selection: rank %i: %s <- WORST\n",
	     num_instances,
	     list[num_instances - 1].record->info.name);

	/* Record rank in the record for diagnostic use only */
	list[num_instances - 1].record->rank = i + 1;

	result = list[0].record;
	free(list);

	return result;
}

void sfptpd_bic_select_instance(struct sync_instance_record *instance_records,
				int num_instances,
				struct sync_instance_record *selected_instance)
{
	int i;

	/* Only one instance may be selected at once */
	for (i = 0; i < num_instances; i++) {
		instance_records[i].selected = (selected_instance == &instance_records[i]);
	}
}

/* fin */
