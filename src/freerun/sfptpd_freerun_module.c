/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2012-2023, Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_freerun_module.c
 * @brief  Freerun Synchronization Module
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdbool.h>

#include "sfptpd_app.h"
#include "sfptpd_thread.h"
#include "sfptpd_message.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_freerun_module.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_clock.h"
#include "sfptpd_interface.h"
#include "sfptpd_constants.h"


/****************************************************************************
 * Defaults
 ****************************************************************************/


/****************************************************************************
 * Types
 ****************************************************************************/

typedef struct freerun_instance freerun_instance_t;

typedef struct freerun_module {
	/* Handle of engine */
	struct sfptpd_engine *engine;

	/* Initial link table */
	struct sfptpd_link_table link_table;

	/* Linked list of instances */
	freerun_instance_t *instances;
} freerun_module_t;

struct freerun_instance {
	/* Handle of module */
	struct freerun_module *module;

	/* Pointer to the daemon configuration */
	struct sfptpd_freerun_module_config *config;

	/* Sync module control flags */
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;

	/* Handle of the clock */
	struct sfptpd_clock *clock;

	/* Pointer to next instance in linked list */
	struct freerun_instance *next;
};

/****************************************************************************
 * Config File Options
 ****************************************************************************/

static int parse_interface(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[])
{
	sfptpd_freerun_module_config_t *fr = (sfptpd_freerun_module_config_t *)section;
	assert(num_params == 1);

	sfptpd_strncpy(fr->interface_name, params[0], sizeof(fr->interface_name));

	return 0;
}

static int parse_priority(struct sfptpd_config_section *section, const char *option,
			  unsigned int num_params, const char * const params[])
{
	sfptpd_freerun_module_config_t *fr = (sfptpd_freerun_module_config_t *)section;
	int tokens, priority;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%u", &priority);
	if (tokens != 1)
		return EINVAL;

	fr->priority = (unsigned int)priority;
	return 0;
}

static int parse_clock_class(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_freerun_module_config_t *fr = (sfptpd_freerun_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "locked") == 0) {
		fr->clock_class = SFPTPD_CLOCK_CLASS_LOCKED;
	} else if (strcmp(params[0], "holdover") == 0) {
		fr->clock_class = SFPTPD_CLOCK_CLASS_HOLDOVER;
	} else if (strcmp(params[0], "freerunning") == 0) {
		fr->clock_class = SFPTPD_CLOCK_CLASS_FREERUNNING;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_clock_accuracy(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_freerun_module_config_t *fr = (sfptpd_freerun_module_config_t *)section;
	int tokens;
	assert(num_params == 1);

	if (strcmp(params[0], "unknown") == 0) {
		fr->clock_accuracy = INFINITY;
		return 0;
	}

	tokens = sscanf(params[0], "%Lf", &(fr->clock_accuracy));
	if (tokens != 1)
		return EINVAL;

	return 0;
}


static int parse_clock_traceability(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	int rc = 0;
	int param;
	sfptpd_freerun_module_config_t *fr = (sfptpd_freerun_module_config_t *)section;

	fr->clock_time_traceable = false;
	fr->clock_freq_traceable = false;

	for (param = 0; param < num_params; param++) {
		if (strcmp(params[param], "time") == 0) {
			fr->clock_time_traceable = true;
		} else if (strcmp(params[param], "freq") == 0) {
			fr->clock_freq_traceable = true;
		} else {
			rc = EINVAL;
		}
	}

	return rc;
}


static const sfptpd_config_option_t freerun_config_options[] =
{
	{"interface", "<INTERFACE_NAME | system>",
		"The value 'system' specifies the system clock. Any other value specifies "
		"the name of the interface hosting the local reference clock.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_interface},
	{"priority", "<NUMBER>",
		"Relative priority of sync module instance. Smaller values have higher "
		"priority. The default 128.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_priority},
	{"clock_class", "<locked | holdover | freerunning>",
		"Clock class. Default (correct) value for a freerun clock is freerunning.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_clock_class,
		.hidden = true},
	{"clock_accuracy", "<NUMBER | unknown>",
		"Clock accuracy in ns or unknown. Default value is unknown.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_clock_accuracy,
		.hidden = true},
	{"clock_traceability", "<time | freq>*",
		"Traceability of clock time and frequency. Default for freerun is neither.",
		~0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_clock_traceability,
		.hidden = true},
};

static const sfptpd_config_option_set_t freerun_config_option_set =
{
	.description = "Free-run Configuration File Options",
	.category = SFPTPD_CONFIG_CATEGORY_FREERUN,
	.num_options = sizeof(freerun_config_options)/sizeof(freerun_config_options[0]),
	.options = freerun_config_options
};


/****************************************************************************
 * Internal Functions
 ****************************************************************************/

static struct freerun_instance *freerun_find_instance_by_clock(freerun_module_t *fr,
							       struct sfptpd_clock *clock) {
	struct freerun_instance *instance;

	/* Walk linked list, looking for the clock */
	for (instance = fr->instances;
	     instance && instance->clock != clock;
	     instance = instance->next);

	return instance;
}

static bool freerun_is_instance_in_list(freerun_module_t *fr,
				       freerun_instance_t *instance) {
	struct freerun_instance *ptr;

	assert(instance);

	/* Walk linked list, looking for the clock */
	for (ptr = fr->instances;
	     ptr && ptr != instance;
	     ptr = ptr->next);

	return (ptr == NULL) ? false : true;
}

static void freerun_destroy_instances(freerun_module_t *fr) {
	freerun_instance_t *instance;
	freerun_instance_t *next;

	next = fr->instances;
	fr->instances = NULL;

	for (instance = next; instance; instance = next) {
		next = instance->next;
		free(instance);
	}

}

static int freerun_create_instances(struct sfptpd_config *config,
				    freerun_module_t *fr)
{
	sfptpd_freerun_module_config_t *instance_config;
	freerun_instance_t *instance, **instance_ptr;

	assert(config != NULL);
	assert(fr != NULL);
	assert(fr->instances == NULL);

	/* Prepare linked list */
	instance_ptr = &fr->instances;

	/* Setting up initial state: find the first instance configuration */
	instance_config = (struct sfptpd_freerun_module_config *)
		sfptpd_config_category_first_instance(config,
						      SFPTPD_CONFIG_CATEGORY_FREERUN);

	/* Loop round available instance configurations */
	while (instance_config) {
		INFO("freerun %s: creating sync-instance\n", SFPTPD_CONFIG_GET_NAME(instance_config));

		instance = (freerun_instance_t *)calloc(1, sizeof *instance);
		if (instance == NULL) {
			CRITICAL("freerun %s: failed to allocate sync instance memory\n",
				 SFPTPD_CONFIG_GET_NAME(instance_config));
			freerun_destroy_instances(fr);
			return ENOMEM;
		}

		/* Populate instance state */
		instance->module = fr;
		instance->config = instance_config;

		/* Append to linked list */
		*instance_ptr = instance;
		instance_ptr = &instance->next;

		/* Get next configuration, if present */
		instance_config = (struct sfptpd_freerun_module_config *)
			sfptpd_config_category_next_instance(&instance_config->hdr);
	}

	return 0;
}


struct phy_search_result {
	const struct sfptpd_link *link;
	struct sfptpd_interface *interface;
	struct sfptpd_clock *clock;
	long double holdover;
	long double accuracy;
	enum sfptpd_clock_stratum stratum;
};

struct phy_search_result freerun_find_physical_link(freerun_module_t *fr,
						    const struct sfptpd_link *link)
{
	struct phy_search_result best = { NULL, NULL, NULL, INFINITY, INFINITY,
					  SFPTPD_CLOCK_STRATUM_X };
	struct phy_search_result candidate = best;
	struct sfptpd_clock *system_clock = sfptpd_clock_get_system_clock();
	const struct sfptpd_link *other;
	int row;

	if (link == NULL)
		return best;
	candidate.link = link;

	/* Resolve VLANs first. */
	while (candidate.link->type == SFPTPD_LINK_VLAN) {
		other = sfptpd_link_by_if_index(&fr->link_table, candidate.link->if_link);
		if (other == NULL) {
			ERROR("freerun %s: inner link not found resolving VLAN %s\n",
			      link->if_name, candidate.link->if_name);
		} else {
			TRACE_L4("freerun %s: resolved VLAN %s to %s\n",
				 link->if_name, candidate.link->if_name, other->if_name);
			candidate.link = other;
		}
	}

	candidate.interface = sfptpd_interface_find_by_if_index(candidate.link->if_index);
	if (candidate.interface != NULL) {
		candidate.clock = sfptpd_interface_get_clock(candidate.interface);
		if (candidate.clock != NULL && candidate.clock != system_clock) {
			sfptpd_clock_get_accuracy(candidate.clock,
						  &candidate.stratum,
						  &candidate.accuracy,
						  &candidate.holdover);
			return candidate;
		} else {
			TRACE_L4("freerun %s: candidate physical interface %s does not have a hw clock\n",
				 link->if_name, candidate.link->if_name);
		}
	} else {
		TRACE_L4("freerun %s: candidate physical interface %s does not have an interface object\n",
			 link->if_name, candidate.link->if_name);
	}

	/* Use VLAN-resolved link as new starting point */
	link = candidate.link;

	/* Do a depth-first search of tree from this logical interface,
	 * trying the physical interfaces we come across. */
	for (row = 0; row < fr->link_table.count; row++) {
		other = fr->link_table.rows + row;

		if (other->bond.if_master == link->if_index) {
			candidate = freerun_find_physical_link(fr, other);
			if (candidate.link != NULL) {
				TRACE_L4("freerun: candidate physical interface %s\n",
					 candidate.link->if_name);
				if (candidate.holdover < best.holdover ||
				    (candidate.holdover == best.holdover &&
				     candidate.accuracy < best.accuracy) ||
				    (candidate.holdover == best.holdover &&
				     candidate.accuracy == best.accuracy &&
				     candidate.stratum < best.stratum) ||
				    (candidate.clock != NULL &&
				     best.clock == NULL)) {
					best = candidate;
					TRACE_L4("freerun: ... is new best!\n");
				}
			}
		}
	}

	if (best.clock != NULL)
		TRACE_L4("freerun: %s chosen %s\n",
			 link->if_name, best.link->if_name);

	return best;
}


static int freerun_select_clock(freerun_module_t *fr,
				freerun_instance_t *instance)
{
	struct phy_search_result candidate = { NULL, NULL };
	struct sfptpd_clock *system_clock;
	struct sfptpd_freerun_module_config *config;
	struct freerun_instance *other_instance;
	int rc;

	assert(fr != NULL);
	config = instance->config;
	assert(config != NULL);

	system_clock = sfptpd_clock_get_system_clock();

	if (strcmp(config->interface_name, "system") == 0) {
		candidate.clock = system_clock;
	} else {
		/* A NIC must be specified and it must have a hardware clock */
		if (config->interface_name[0] == '\0') {
			ERROR("freerun %s: no interface specified for nic clock\n",
			SFPTPD_CONFIG_GET_NAME(config));
			return EINVAL;
		}

		candidate = freerun_find_physical_link(fr,
						       sfptpd_link_by_name(&fr->link_table,
									   config->interface_name));
	}

	/* Check that we have found a physical clock corresponding to logical
	 * interface specified. */
	if (candidate.clock == NULL) {
		ERROR("freerun %s: no hardware clock found for %s\n",
		      SFPTPD_CONFIG_GET_NAME(config), config->interface_name);
		return ENODEV;
	}

	/* Check if the clock is in use in another instance */
	other_instance = freerun_find_instance_by_clock(fr, candidate.clock);
	if (other_instance) {
		ERROR("freerun %s: clock on nic %s is already in use for instance %s\n",
		      SFPTPD_CONFIG_GET_NAME(config),
		      config->interface_name,
		      other_instance->config->hdr.name);
		return EBUSY;
	}

	/* Initial control flags. All instances start de-selected and with
	 * clock control disabled but with timestamp processing enabled. Note
	 * that the control flags have little meaning for the freerun module. */
	instance->ctrl_flags = SYNC_MODULE_CTRL_FLAGS_DEFAULT;

	/* Sanity checks complete: save the clock. */
	instance->clock = candidate.clock;

	/* Set the NIC clock based on the system clock to ensure
	   it has a sensible initial value */
	rc = sfptpd_clock_set_time(instance->clock, system_clock, NULL, true);

	if (rc != 0) {
		TRACE_L4("freerun %s: failed to compare and set clock %s to system clock, error %s\n",
			 SFPTPD_CONFIG_GET_NAME(config),
			 sfptpd_clock_get_short_name(instance->clock),
			 strerror(rc));
		return rc;
	}

	TRACE_L4("freerun %s: selected clock %s as reference\n",
		 SFPTPD_CONFIG_GET_NAME(config),
		 sfptpd_clock_get_long_name(instance->clock));
	return 0;
}


static void freerun_on_get_status(freerun_module_t *fr,
				  sfptpd_sync_module_msg_t *msg)
{
	freerun_instance_t *instance;
	struct sfptpd_sync_instance_status *status;

	assert(fr != NULL);
	assert(msg != NULL);
	assert(msg->u.get_status_req.instance_handle != NULL);

	instance = (freerun_instance_t *) msg->u.get_status_req.instance_handle;
	assert(instance);
	assert(freerun_is_instance_in_list(fr, instance));

	status = &msg->u.get_status_resp.status;
	status->state = SYNC_MODULE_STATE_SLAVE;
	status->alarms = 0;
	status->clock = instance->clock;
	sfptpd_time_zero(&status->offset_from_master);
	status->user_priority = instance->config->priority;
	status->master.clock_id = SFPTPD_CLOCK_ID_UNINITIALISED;
	status->master.remote_clock = false;
	status->master.clock_class = instance->config->clock_class;
	status->master.time_source = SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;
	status->master.accuracy = instance->config->clock_accuracy;
	status->master.allan_variance = NAN;
	status->master.steps_removed = 0;
	status->master.time_traceable = instance->config->clock_time_traceable;
	status->master.freq_traceable = instance->config->clock_freq_traceable;
	status->local_accuracy = SFPTPD_ACCURACY_FREERUN;

	SFPTPD_MSG_REPLY(msg);
}


static void freerun_on_control(freerun_module_t *fr,
			       sfptpd_sync_module_msg_t *msg)
{
	freerun_instance_t *instance;

	assert(fr != NULL);
	assert(msg != NULL);
	assert(msg->u.control_req.instance_handle != NULL);

	instance = (freerun_instance_t *)msg->u.control_req.instance_handle;
	assert(instance);
	assert(freerun_is_instance_in_list(fr, instance));

	instance->ctrl_flags &= ~msg->u.control_req.mask;
	instance->ctrl_flags |= (msg->u.control_req.flags & msg->u.control_req.mask);

	SFPTPD_MSG_REPLY(msg);
}


static void freerun_on_step_clock(freerun_module_t *fr,
				  sfptpd_sync_module_msg_t *msg)
{
	freerun_instance_t *instance;

	assert(fr != NULL);
	assert(msg != NULL);
	assert(msg->u.step_clock_req.instance_handle != NULL);

	instance = (freerun_instance_t *) msg->u.step_clock_req.instance_handle;
	assert(instance);
	assert(freerun_is_instance_in_list(fr, instance));

	/* Step the slave clock by the specified amount */
	(void)sfptpd_clock_adjust_time(instance->clock, &msg->u.step_clock_req.offset);

	SFPTPD_MSG_REPLY(msg);
}


static void freerun_on_save_state(freerun_module_t *fr,
				  sfptpd_sync_module_msg_t *msg)
{
	freerun_instance_t *instance;
	char flags[256];

	assert(fr != NULL);
	assert(msg != NULL);

	for (instance = fr->instances; instance; instance = instance->next) {
		assert(instance->clock != NULL);
		sfptpd_sync_module_ctrl_flags_text(instance->ctrl_flags, flags, sizeof(flags));

		sfptpd_log_write_state(instance->clock,
				       SFPTPD_CONFIG_GET_NAME(instance->config),
				       "instance: %s\n"
				       "clock-name: %s\n"
				       "clock-id: %s\n"
				       "state: freerunning-clock\n"
				       "control-flags: %s\n",
				       SFPTPD_CONFIG_GET_NAME(instance->config),
				       sfptpd_clock_get_long_name(instance->clock),
				       sfptpd_clock_get_hw_id_string(instance->clock),
				       flags);
	}

	/* In the case of the free-run sync-module, we don't touch the clock
	 * frequency adjustment so don't save this. */

	SFPTPD_MSG_FREE(msg);
}


static void freerun_on_write_topology(freerun_module_t *fr,
				      sfptpd_sync_module_msg_t *msg)
{
	freerun_instance_t *instance;
	FILE *stream;

	assert(fr != NULL);
	assert(msg != NULL);
	assert(msg->u.write_topology_req.stream != NULL);
	assert(msg->u.write_topology_req.instance_handle != NULL);

	stream = msg->u.write_topology_req.stream;
	instance = (freerun_instance_t *) msg->u.write_topology_req.instance_handle;
	assert(instance);
	assert(freerun_is_instance_in_list(fr, instance));
	assert(instance->clock != NULL);

	/* This should only be called on selected instances */
	assert(instance->ctrl_flags & SYNC_MODULE_SELECTED);

	fprintf(stream,
		"====================\n"
		"state: freerun\n"
		"====================\n\n");
	sfptpd_log_topology_write_field(stream, true, sfptpd_clock_get_long_name(instance->clock));
	sfptpd_log_topology_write_field(stream, true, sfptpd_clock_get_hw_id_string(instance->clock));

	SFPTPD_MSG_REPLY(msg);
}


static int freerun_on_startup(void *context)
{
	freerun_module_t *fr = (freerun_module_t *)context;
	freerun_instance_t *instance;
	int rc = 0;

	assert(fr != NULL);

	/* Determine the local reference clock for each instance*/
	for (instance = fr->instances;
	     instance;
	     instance = instance->next) {
		rc = freerun_select_clock(fr, instance);
		if (rc != 0) break;
	}

	return rc;
}


static void freerun_on_shutdown(void *context)
{
	freerun_module_t *fr = (freerun_module_t *)context;
	assert(fr != NULL);

	sfptpd_link_table_free_copy(&fr->link_table);
	freerun_destroy_instances(fr);

	/* Free the sync module memory */
	free(fr);
}


static void freerun_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	freerun_module_t *fr = (freerun_module_t *)context;
	sfptpd_sync_module_msg_t *msg = (sfptpd_sync_module_msg_t *)hdr;

	assert(fr != NULL);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_APP_MSG_RUN:
		/* This module doesn't have any timers */
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_GET_STATUS:
		freerun_on_get_status(fr, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_CONTROL:
		freerun_on_control(fr, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_UPDATE_GM_INFO:
		/* This module doesn't use this message */
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_UPDATE_LEAP_SECOND:
		/* This module doesn't use this message */
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STEP_CLOCK:
		freerun_on_step_clock(fr, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_LOG_STATS:
		/* The freerun module doesn't have any stats */
		SFPTPD_MSG_REPLY(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_SAVE_STATE:
		freerun_on_save_state(fr, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_WRITE_TOPOLOGY:
		freerun_on_write_topology(fr, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STATS_END_PERIOD:
		/* The freerun module doesn't have any stats */
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_TEST_MODE:
		/* This module doesn't have any test modes */
		SFPTPD_MSG_FREE(msg);
		break;

	default:
		WARNING("freerun: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}


static void freerun_on_user_fds(void *context, unsigned int num_fds,
				struct sfptpd_thread_readyfd fds[])
{
	/* The freerun module doesn't use user file descriptors */
}


static const struct sfptpd_thread_ops freerun_thread_ops = 
{
	freerun_on_startup,
	freerun_on_shutdown,
	freerun_on_message,
	freerun_on_user_fds
};


/****************************************************************************
 * Public Functions
 ****************************************************************************/

static void freerun_config_destroy(struct sfptpd_config_section *section)
{
	assert(section != NULL);
	assert(section->category == SFPTPD_CONFIG_CATEGORY_FREERUN);
	free(section);
}


static struct sfptpd_config_section *freerun_config_create(const char *name,
							   enum sfptpd_config_scope scope,
							   bool allows_instances,
							   const struct sfptpd_config_section *src)
{
	struct sfptpd_freerun_module_config *new;

	assert((src == NULL) || (src->category == SFPTPD_CONFIG_CATEGORY_FREERUN));

	new = (struct sfptpd_freerun_module_config *)calloc(1, sizeof(*new));
	if (new == NULL) {
		ERROR("freerun %s: failed to allocate memory for configuration\n", name);
		return NULL;
	}

	/* If the source isn't null, copy the section contents. Otherwise,
	 * initialise with the default values. */
	if (src != NULL) {
		memcpy(new, src, sizeof(*new));
	} else {
		new->interface_name[0] = '\0';
		new->priority = SFPTPD_DEFAULT_PRIORITY;
		new->clock_class = SFPTPD_CLOCK_CLASS_FREERUNNING;
		new->clock_accuracy = INFINITY;
		new->clock_time_traceable = false;
		new->clock_freq_traceable = false;
	}

	/* Initialise the header. */
	SFPTPD_CONFIG_SECTION_INIT(new, freerun_config_create,
				   freerun_config_destroy,
				   SFPTPD_CONFIG_CATEGORY_FREERUN,
				   scope, allows_instances, name);

	return &new->hdr;
}


int sfptpd_freerun_module_config_init(struct sfptpd_config *config)
{
	struct sfptpd_freerun_module_config *new;
	assert(config != NULL);

	new = (struct sfptpd_freerun_module_config *)
		freerun_config_create(SFPTPD_FREERUN_MODULE_NAME,
				      SFPTPD_CONFIG_SCOPE_GLOBAL, true, NULL);
	if (new == NULL)
		return ENOMEM;

	/* Add the configuration */
	SFPTPD_CONFIG_SECTION_ADD(config, new);

	/* Register the configuration options */
	sfptpd_config_register_options(&freerun_config_option_set);
	return 0;
}


struct sfptpd_freerun_module_config *sfptpd_freerun_module_get_config(struct sfptpd_config *config)
{
	return (struct sfptpd_freerun_module_config *)
		sfptpd_config_category_global(config, SFPTPD_CONFIG_CATEGORY_FREERUN);
}


void sfptpd_freerun_module_set_default_interface(struct sfptpd_config *config,
						 const char *interface_name)
{
	struct sfptpd_freerun_module_config *fr;
	assert(config != NULL);
	assert(interface_name != NULL);

	fr = sfptpd_freerun_module_get_config(config);
	assert(fr != NULL);

	sfptpd_strncpy(fr->interface_name, interface_name, sizeof(fr->interface_name));
}

int sfptpd_freerun_module_create(struct sfptpd_config *config,
				 struct sfptpd_engine *engine,
				 struct sfptpd_thread **sync_module,
				 struct sfptpd_sync_instance_info *instances_info_buffer,
				 int instances_info_entries,
				 const struct sfptpd_link_table *link_table,
				 bool *link_subscriber)
{
	freerun_module_t *fr;
	freerun_instance_t *instance;
	int rc;

	assert(config != NULL);
	assert(engine != NULL);
	assert(sync_module != NULL);

	INFO("freerun: creating sync-module\n");

	*sync_module = NULL;
	fr = (freerun_module_t *)calloc(1, sizeof(*fr));
	if (fr == NULL) {
		CRITICAL("freerun: failed to allocate sync module memory\n");
		return ENOMEM;
	}

	fr->engine = engine;

	/* Take a copy of link table for resolving interfaces in thread */
	rc = sfptpd_link_table_copy(link_table, &fr->link_table);
	if (rc != 0) {
		goto fail1;
	}

	/* Create all the sync instances */
	rc = freerun_create_instances(config, fr);
	if (rc != 0) {
		goto fail2;
	}

	/* Create the sync module thread- the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("freerun", &freerun_thread_ops, fr, sync_module);
	if (rc != 0) {
		goto fail2;
	}

	/* If a buffer has been provided, populate the instance information */
	if (instances_info_buffer != NULL) {
		memset(instances_info_buffer, 0,
		       instances_info_entries * sizeof(*instances_info_buffer));

		for (instance = fr->instances;
		     (instance != NULL) && (instances_info_entries > 0);
		     instance = instance->next) {
			instances_info_buffer->module = *sync_module;
			instances_info_buffer->handle = (struct sfptpd_sync_instance *) instance;
			instances_info_buffer->name = instance->config->hdr.name;
			instances_info_buffer++;
			instances_info_entries--;
		}
	}

	return 0;

 fail2:
	sfptpd_link_table_free_copy(&fr->link_table);
 fail1:
	free(fr);
	return rc;
}


/* fin */
