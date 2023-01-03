/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_general_config.c
 * @brief  Command line and configuration file parsing for sfptpd
 */

#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_misc.h"
#include "sfptpd_statistics.h"
#include "sfptpd_phc.h"
#include "sfptpd_crny_module.h"


/****************************************************************************
 * Config File Options
 ****************************************************************************/

static int parse_sync_module(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[]);
static int parse_selection_policy(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[]);
static int parse_selection_policy_rules(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[]);
static int parse_phc_diff_method_order(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[]);
static int parse_selection_holdoff_interval(struct sfptpd_config_section *section, const char *option,
					    unsigned int num_params, const char * const params[]);
static int parse_message_log(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[]);
static int parse_stats_log(struct sfptpd_config_section *section, const char *option,
   			   unsigned int num_params, const char * const params[]);
static int parse_daemon(struct sfptpd_config_section *section, const char *option,
			unsigned int num_params, const char * const params[]);
static int parse_lock(struct sfptpd_config_section *section, const char *option,
		      unsigned int num_params, const char * const params[]);
static int parse_state_path(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[]);
static int parse_control_path(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[]);
static int parse_sync_interval(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[]);
static int parse_sync_threshold(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[]);
static int parse_clock_control(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[]);
static int parse_epoch_guard(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[]);
static int parse_clock_list(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[]);
static int parse_clock_readonly(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[]);
static int parse_persistent_clock_correction(struct sfptpd_config_section *section, const char *option,
					     unsigned int num_params, const char * const params[]);
static int parse_non_solarflare_nics(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[]);
static int parse_assume_one_phc_per_nic(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[]);
static int parse_avoid_efx_ioctl(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[]);
static int parse_timestamping_interfaces(struct sfptpd_config_section *section, const char *option,
					 unsigned int num_params, const char * const params[]);
static int parse_timestamping_disable_on_exit(struct sfptpd_config_section *section, const char *option,
					      unsigned int num_params, const char * const params[]);
static int parse_pid_filter_kp(struct sfptpd_config_section *section, const char *option,
			      unsigned int num_params, const char * const params[]);
static int parse_pid_filter_ki(struct sfptpd_config_section *section, const char *option,
			      unsigned int num_params, const char * const params[]);
static int parse_trace_level(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[]);
static int parse_test_mode(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[]);

static int parse_json_stats(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[]);

static int parse_json_remote_monitor(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[]);
static int parse_hotplug_detection_mode(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[]);
static int parse_clustering(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[]);
static int parse_clustering_guard_threshold(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[]);
static int parse_limit_freq_adj(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[]);
static int parse_phc_pps_methods(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[]);
static int parse_ignore_critical(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[]);
static int parse_rtc_adjust(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[]);

static int validate_config(struct sfptpd_config_section *parent);

static const sfptpd_config_option_t config_general_options[] =
{
	/* Generic config options */
	{"sync_module", "<freerun | ptp | pps | ntp | crny> [instance-names]",
		"Create instances of the specified sync module",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_sync_module},
	{"selection_policy", "<automatic | manual | manual-startup> [initial-instance]",
		"Use automatic (default), manual or manual followed by "
		"automatic sync instance selection",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_selection_policy},
	{"selection_policy_rules", "<manual | state | no-alarms | user-priority | clustering | clock-class | total-accuracy | allan-variance | steps-removed>*",
		"Define the list of rules for the automatic selection policy",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_selection_policy_rules},
	{"phc_pps_methods", "<devpps | devptp>*",
		"Define the order of non-proprietary PPS methods to try",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_phc_pps_methods},
	{"selection_holdoff_interval", "NUMBER",
		"Specifies how long to wait after detecting a better instance "
		"before selecting it. Default is "
		STRINGIFY(SFPTPD_DEFAULT_SELECTION_HOLDOFF_INTERVAL) " seconds.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_selection_holdoff_interval},
	{"message_log", "<syslog | stderr | filename>",
		"Specifies where to send messages generated by the application. By default messages are sent to stderr",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_message_log},
	{"stats_log", "<off | stdout | filename>",
		"Specifies if and where to log statistics generated by the application. By default statistics logging is disabled",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_stats_log},
	{"daemon", "",
		"Run as a daemon. Disabled by default",
		0, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_daemon},
	{"lock", "<off | on>",
		"Specify whether to use a lock file to stop multiple simultaneous instances of the daemon. Enabled by default",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_lock},
	{"state_path", "<path>",
		"Directory in which to store sfptpd state data. Defaults to " SFPTPD_DEFAULT_STATE_PATH,
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_state_path},
	{"control_path", "<path>",
		"Path for Unix domain control socket. Defaults to " SFPTPD_DEFAULT_CONTROL_PATH,
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_control_path},
	{"sync_interval", "NUMBER",
		"Specifies the interval in 2^number seconds at which the clocks are synchronized to the local reference clock",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_sync_interval},
	{"local_sync_threshold", "NUMBER",
		"Threshold in nanoseconds of the offset between the system clock and a NIC clock over a "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT)
		"s period to be considered in sync (converged). The default is "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE, false,
		parse_sync_threshold},
	{"clock_control", "<slew-and-step | step-at-startup | no-step | no-adjust | step-forward | step-on-first-lock>",
		"Specifies how the clocks are controlled. By default clocks are stepped and slewed as necessary",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_clock_control},
	{"epoch_guard", "<alarm-only | prevent-sync | correct-clock>",
		"Guards against propagation of times near the epoch. The default is correct-clock",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_epoch_guard},
	{"clock_list", "[<name | mac-address | clock-id | ifname>]*",
		"Specifies the set of clocks that sfptpd should discipline. By default all clocks are disciplined",
		~0, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_clock_list},
	{"clock_readonly", "[<name | mac-address | clock-id | ifname>]",
		"Specifies a set of clocks that sfptpd should never step or slew, under any circumstance. Use with care.",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_clock_readonly},
	{"persistent_clock_correction", "<off | on>",
		"Specifies whether to used saved clock frequency corrections when disciplining clocks. Enabled by default",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_persistent_clock_correction},
	{"non_solarflare_nics", "<off | on>",
		"Specify whether to use timestamping and hardware clock "
		"capabilities of non-Solarflare adapters. Disabled by default",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, true,
		parse_non_solarflare_nics},
	{"non_xilinx_nics", "<off | on>",
		"Specify whether to use timestamping and hardware clock "
		"capabilities of non-Xilinx adapters. Disabled by default",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_non_solarflare_nics},
	{"assume_one_phc_per_nic", "<off | on>",
		"Specify whether multiple reported clock devices on a NIC "
		"should be assumed to represent the same underlying clock. "
		"Enabled by default",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_assume_one_phc_per_nic},
	{"avoid_efx_ioctl", "<off | on>",
		"Specify whether to avoid private SIOCEFX ioctl for Solarflare "
		"adapters where possible. Disabled by default",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, true,
		parse_avoid_efx_ioctl},
	{"phc_diff_methods", "<sys-offset-precise | pps | sys-offset-ext | sys-offset | read-time>*",
		"Define the list of PHC diff methods used",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_phc_diff_method_order},
	{"timestamping_interfaces", "[<name | mac-address | *>]",
		"Specifies set of interfaces on which general receive packet timestamping should be enabled",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_timestamping_interfaces},
	{"timestamping_disable_on_exit", "<off | on>",
		"Specifies whether timestamping should be disabled when daemon exits",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_timestamping_disable_on_exit},
	{"pid_filter_p", "NUMBER",
		"Secondary servo PID filter proportional term coefficient. Default value is "
		STRINGIFY(SFPTPD_DEFAULT_SERVO_K_PROPORTIONAL) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE, false,
		parse_pid_filter_kp},
	{"pid_filter_i", "NUMBER",
		"Secondary servo PID filter integral term coefficient. Default value is "
		STRINGIFY(SFPTPD_DEFAULT_SERVO_K_INTEGRAL) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE, false,
		parse_pid_filter_ki},
	{"trace_level", "[<general | threading | bic>] NUMBER",
		"Specifies a module trace level, if built with trace enabled. If module name is omitted, will set the 'general' module trace level. Default is 0 - no trace",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false,
		parse_trace_level},
	{"test_mode", "",
		"Enables features to aid testing. Disabled by default",
		0, SFPTPD_CONFIG_SCOPE_GLOBAL, true,
		parse_test_mode},
	{"json_stats", "<filename>",
		"Output realtime module statistics in JSON-lines format to this file (http://jsonlines.org). Disabled by default.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false, parse_json_stats},
	{"json_remote_monitor", "<filename>",
		"Output realtime information collected by the PTP remote monitor in JSON-lines format to this file (http://jsonlines.org). Disabled by default.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false, parse_json_remote_monitor},
	{"hotplug_detection_mode", "<netlink-and-probe | netlink | probe | manual "
		"| manual-with-scan>",
		"Configure how the daemon should detect hotplug insertion and "
		"removal of interfaces and bond changes. In manual mode the "
		"sfptpdctl control tool must be used to tell sfptpdctl which "
		"interfaces to use (with initial scan with -with-scan). "
		"In netlink mode changes are detected by "
		"Netlink events. In probe mode changes are detected by probing. "
		"The default mode, netlink-and-probe combines both these techniques.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false, parse_hotplug_detection_mode},
	{"clustering", "discriminator <INSTANCE> <THRESHOLD> <NO_DISCRIMINATOR_SCORE>",
		"Implements clustering based on MODE. Currently only supports "
		"discriminator mode, which disqualifies sync instances that differ "
		"from discriminator INSTANCE in excess of THRESHOLD ns. INSTANCE must "
		"be a sync instance name. NO_DISCRIMINATOR_SCORE is the clustering "
                "score returned when no discriminator is available.",
		4, SFPTPD_CONFIG_SCOPE_GLOBAL, false, parse_clustering},
	{"clustering_guard", "<off | on> <THRESHOLD>",
		"Specifies whether to turn on the clusterig guard feature, as well as "
                "the threshold for clustering score to be compared to.",
		2, SFPTPD_CONFIG_SCOPE_GLOBAL, false, parse_clustering_guard_threshold},
	{"limit_freq_adj", "NUMBER",
		"Limit NIC clock frequency adjustment to the lesser of "
		"advertised capability and NUMBER ppb.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, false, parse_limit_freq_adj},
	{"ignore_critical", "<no-ptp-clock | no-ptp-subsystem | clock-control-conflict>*",
		"Ignore certain critical warnings that would normally "
		"terminate execution but may be expected in some niche "
		"or diagnostic use cases.",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, false, parse_ignore_critical},
	{"rtc_adjust", "<off | on>",
		"Specify whether to let the kernel adjust sync the RTC clock. "
		"Enabled by default",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, true,
		parse_rtc_adjust},
};

static const sfptpd_config_option_set_t config_general_option_set =
{
	.description = "Generic Configuration File Options",
	.category = SFPTPD_CONFIG_CATEGORY_GENERAL,
	.num_options = sizeof(config_general_options)/sizeof(config_general_options[0]),
	.options = config_general_options,
	.validator = validate_config,
};

static const char *config_general_name = "general";


/****************************************************************************
 * Config Option Handlers
 ****************************************************************************/

static int parse_sync_module(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[])
{
	struct sfptpd_config *config;
	struct sfptpd_config_section *parent, *new;
	assert(num_params >= 1);

	config = section->config;
	assert(config != NULL);

	/* Find the global configuration for this category using the following
	 * steps:
	 *  1) Look for a configuration section with this name
	 *  2) Check that it supports instances
	 */
	parent = sfptpd_config_find(config, params[0]);
	if ((parent == NULL) || (parent->scope == SFPTPD_CONFIG_SCOPE_INSTANCE)) {
		CFG_ERROR(section, "config: unknown sync module %s\n", params[0]);
		return ENOENT;
	}

	if (!parent->allows_instances) {
		CFG_ERROR(section,
			  "config: sync module %s does not support instances\n",
			  params[0]);
		return EINVAL;
	}

	((sfptpd_config_general_t *) section)->declared_sync_modules |= (1 << parent->category);

	params++;
	num_params--;

	for ( ; num_params != 0; params++, num_params--) {
		if (strlen(params[0]) >= SFPTPD_CONFIG_SECTION_NAME_MAX) {
			CFG_ERROR(section, "instance name %s too long\n",
				  params[0]);
			return ERANGE;
		}

		/* If the section already exists then fail */
		if (sfptpd_config_find(config, params[0]) != NULL) {
			CFG_ERROR(section, "instance %s already exists\n",
				  params[0]);
			return EEXIST;
		}

		/* Create a new section based on the global configuration */
		assert(parent->ops.create != NULL);
		new = parent->ops.create(params[0], SFPTPD_CONFIG_SCOPE_INSTANCE,
					 false, parent);
		if (new == NULL) {
			CFG_ERROR(section, "failed to create instance %s\n",
				  params[0]);
			return ENOMEM;
		}

		/* Add the instance into the configuration */
		sfptpd_config_section_add(config, new);

		TRACE_L1("config: created %s instance \'%s\'\n",
			 parent->name, params[0]);
	}

	return 0;
}

static int parse_selection_policy(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params >= 1);

	if (strcmp(params[0], "automatic") == 0) {
		general->selection_policy.strategy = SFPTPD_SELECTION_STRATEGY_AUTOMATIC;
	} else {
		if (strcmp(params[0], "manual") == 0) {
			general->selection_policy.strategy = SFPTPD_SELECTION_STRATEGY_MANUAL;
		} else if (strcmp(params[0], "manual-startup") == 0) {
			general->selection_policy.strategy = SFPTPD_SELECTION_STRATEGY_MANUAL_STARTUP;
		} else {
			CFG_ERROR(section, "unknown selection mode: %s\n", params[0]);
			return EINVAL;
		}

		if (num_params == 1) {
			CFG_ERROR(section, "no initial instance name\n");
			return ERANGE;
		} else if (strlen(params[1]) >= SFPTPD_CONFIG_SECTION_NAME_MAX) {
			CFG_ERROR(section, "instance name %s too long\n",
				  params[1]);
			return ERANGE;
		}
		/* Can't easily check that the instance name is valid here as that 
		 * implies ordering of elements in the configuration */
		strcpy(general->initial_sync_instance, params[1]);
	}

	return 0;
}


static int parse_selection_policy_rules(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	int i, j, k;

	if (num_params >= SELECTION_RULE_MAX) {
		CFG_ERROR(section, "too many rules (%d) listed in selection policy\n", num_params);
		return EINVAL;
	}

	for (i = 0; i < num_params; i++) {
		for (j = 0; j < SELECTION_RULE_MAX; j++) {
			if (strcmp(params[i], sfptpd_selection_rule_names[j]) == 0) {

				for (k = 0; k < i; k++) {
					if (general->selection_policy.rules[k] == j) {
						CFG_ERROR(section,
							  "rule %s listed more than once\n",
							  params[i]);
						return EINVAL;
					}
				}

				general->selection_policy.rules[i] = j;
				break;
			}
		}
		if (j == SELECTION_RULE_MAX) {
			CFG_ERROR(section, "unknown selection rule %s\n", params[i]);
			return EINVAL;
		}
	}

	general->selection_policy.rules[i] = SELECTION_RULE_TIE_BREAK;

	return 0;
}


static int parse_phc_diff_method_order(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	int i, j, k;

	if (num_params > SFPTPD_DIFF_METHOD_MAX) {
		CFG_ERROR(section, "too many methods (%d) listed in phc diff methods\n", num_params);
		return EINVAL;
	}

	for (i = 0; i < num_params; i++) {
		for (j = 0; j < SFPTPD_DIFF_METHOD_MAX; j++) {
			if (strcmp(params[i], sfptpd_phc_diff_method_text[j]) == 0) {

				for (k = 0; k < i; k++) {
					if (general->phc_diff_methods[k] == j) {
						CFG_ERROR(section,
							  "diff method %s listed more than once\n",
							  params[i]);
						return EINVAL;
					}
				}

				general->phc_diff_methods[i] = j;
				break;
			}
		}
		if (j == SFPTPD_DIFF_METHOD_MAX) {
			CFG_ERROR(section, "unknown diff method %s\n", params[i]);
			return EINVAL;
		}
	}

	general->phc_diff_methods[i] = SFPTPD_DIFF_METHOD_MAX;

	return 0;
}


static int parse_phc_pps_methods(struct sfptpd_config_section *section, const char *option,
                                 unsigned int num_params, const char * const params[])
{
        sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
        int i, j, k;

        if (num_params > SFPTPD_PPS_METHOD_MAX) {
                CFG_ERROR(section, "too many methods (%d) listed in phc pps method list\n", num_params);
                return EINVAL;
        }

        for (i = 0; i < num_params; i++) {
                for (j = 0; j < SFPTPD_PPS_METHOD_MAX; j++) {
                        if (strcmp(params[i], sfptpd_phc_pps_method_text[j]) == 0) {

                                for (k = 0; k < i; k++) {
                                        if (general->phc_pps_method[k] == j) {
                                                CFG_ERROR(section,
                                                          "rule %s listed more than once\n",
                                                          params[i]);
                                                return EINVAL;
                                        }
                                }

                                general->phc_pps_method[i] = j;
                                break;
                        }
                }
                if (j == SFPTPD_PPS_METHOD_MAX) {
                        CFG_ERROR(section, "unknown phc pps method %s\n", params[i]);
                        return EINVAL;
                }
        }

        if (i < SFPTPD_PPS_METHOD_MAX)
                general->selection_policy.rules[i++] = SFPTPD_PPS_METHOD_MAX;

        return 0;
}


static int parse_selection_holdoff_interval(struct sfptpd_config_section *section, const char *option,
					    unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);
	int tokens, interval;

	tokens = sscanf(params[0], "%i", &interval);
	if (tokens != 1)
		return EINVAL;

	if (interval < 0) {
		ERROR("config [%s]: %s must be non-negative\n",
		      section->name, option);
		return ERANGE;
	}

	general->selection_holdoff_interval = interval;

	return 0;
}


static int parse_message_log(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "syslog") == 0) {
		general->message_log = SFPTPD_MSG_LOG_TO_SYSLOG;
	} else if (strcmp(params[0], "stderr") == 0) {
		general->message_log = SFPTPD_MSG_LOG_TO_STDERR;
	} else {
		general->message_log = SFPTPD_MSG_LOG_TO_FILE;
		sfptpd_strncpy(general->message_log_filename, params[0],
			       sizeof(general->message_log_filename));
	}

	return 0;
}


static int parse_stats_log(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->stats_log = SFPTPD_STATS_LOG_OFF;
		general->stats_log_filename[0] = '\0';
	} else if (strcmp(params[0], "stdout") == 0) {
		general->stats_log = SFPTPD_STATS_LOG_TO_STDOUT;
		general->stats_log_filename[0] = '\0';
	} else {
		general->stats_log = SFPTPD_STATS_LOG_TO_FILE;
		sfptpd_strncpy(general->stats_log_filename, params[0],
			       sizeof(general->stats_log_filename));
	}

	return 0;
}


static int parse_daemon(struct sfptpd_config_section *section, const char *option,
			unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	general->daemon = true;
	return 0;
}


static int parse_lock(struct sfptpd_config_section *section, const char *option,
		      unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->lock = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->lock = true;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_state_path(struct sfptpd_config_section *section, const char *option,
			      unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	sfptpd_strncpy(general->state_path, params[0],
		       sizeof(general->state_path));

	return 0;
}


static int parse_control_path(struct sfptpd_config_section *section, const char *option,
			      unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	sfptpd_strncpy(general->control_path, params[0],
		       sizeof(general->control_path));

	return 0;
}


static int parse_sync_interval(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);
	int tokens;

	tokens = sscanf(params[0], "%i", &(general->clocks.sync_interval));
	if (tokens != 1)
		return EINVAL;

	if ((general->clocks.sync_interval > SFPTPD_MAX_SYNC_INTERVAL) || 
	    (general->clocks.sync_interval < SFPTPD_MIN_SYNC_INTERVAL)) {
		ERROR("config [%s]: %s not in valid range [%d,%d]\n",
		      section->name, option, 
		      SFPTPD_MIN_SYNC_INTERVAL, SFPTPD_MAX_SYNC_INTERVAL);
		return ERANGE;
	}

	return 0;
}


static int parse_sync_threshold(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	int tokens;
	long double threshold;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%Lf", &threshold);
	if (tokens != 1)
		return EINVAL;

	general->convergence_threshold = threshold;
	return 0;
}


static int parse_clock_control(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;

	assert(num_params == 1);

	if (strcmp(params[0], "slew-and-step") == 0) {
		general->clocks.control = SFPTPD_CLOCK_CTRL_SLEW_AND_STEP;
	} else if (strcmp(params[0], "step-at-startup") == 0) {
		general->clocks.control = SFPTPD_CLOCK_CTRL_STEP_AT_STARTUP;
	} else if (strcmp(params[0], "no-step") == 0) {
		general->clocks.control = SFPTPD_CLOCK_CTRL_NO_STEP;
	} else if (strcmp(params[0], "no-adjust") == 0) {
		general->clocks.control = SFPTPD_CLOCK_CTRL_NO_ADJUST;
        } else if (strcmp(params[0], "step-forward") == 0) {
                general->clocks.control = SFPTPD_CLOCK_CTRL_STEP_FORWARD;
	} else if (strcmp(params[0], "step-on-first-lock") == 0) {
		general->clocks.control = SFPTPD_CLOCK_CTRL_STEP_ON_FIRST_LOCK;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_epoch_guard(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;

	assert(num_params == 1);

	if (strcmp(params[0], "alarm-only") == 0) {
		general->epoch_guard = SFPTPD_EPOCH_GUARD_ALARM_ONLY;
	} else if (strcmp(params[0], "prevent-sync") == 0) {
		general->epoch_guard = SFPTPD_EPOCH_GUARD_PREVENT_SYNC;
	} else if (strcmp(params[0], "correct-clock") == 0) {
		general->epoch_guard = SFPTPD_EPOCH_GUARD_CORRECT_CLOCK;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_clock_list(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[])
{
	unsigned int i, max_clocks;
	sfptpd_config_clocks_t *c;
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;

	c = &general->clocks;

	max_clocks = sizeof(c->clocks)/sizeof(c->clocks[0]);

	for (i = 0; (num_params != 0) && (i < max_clocks); i++) {
		/* If all has been specified set a flag and tidy up later */
		sfptpd_strncpy(c->clocks[i], *params, sizeof(c->clocks[i]));
		params++;
		num_params--;
	}

	/* We have a list of clocks so clear the discipline all flag */
	c->discipline_all = false;
	c->num_clocks = i;

	if (num_params != 0) {
		ERROR("config [%s]: %s maximum number of clocks (%d) exceeded\n",
		      section->name, option, max_clocks);
		return ENOSPC;
	}

	return 0;
}


static int parse_clock_readonly(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	unsigned int i, max_clocks;
	sfptpd_config_clocks_t *c;
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;

	assert(num_params >= 1);

	c = &general->clocks;

	max_clocks = sizeof(c->readonly_clocks)/sizeof(c->readonly_clocks[0]);

	for (i = 0; (num_params != 0) && (i < max_clocks); i++) {
		sfptpd_strncpy(c->readonly_clocks[i], *params, sizeof(c->readonly_clocks[i]));
		params++;
		num_params--;
	}

	c->num_readonly_clocks = i;

	/* initialize readonly_clocks_applied and clock_list_applied */
	for (i = 0; i < SFPTPD_CONFIG_TOKENS_MAX; i++) {
		c->readonly_clocks_applied[i] = CLOCK_OPTION_NOT_APPLIED;
		c->clock_list_applied[i] = CLOCK_OPTION_NOT_APPLIED;
	}

	if (num_params != 0) {
		ERROR("config [%s]: %s maximum number of clocks (%d) exceeded\n",
		      section->name, option, max_clocks);
		return ENOSPC;
	}

	return 0;
}


static int parse_persistent_clock_correction(struct sfptpd_config_section *section, const char *option,
					     unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->clocks.persistent_correction = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->clocks.persistent_correction = true;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_non_solarflare_nics(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->non_sfc_nics = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->non_sfc_nics = true;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_assume_one_phc_per_nic(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->assume_one_phc_per_nic = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->assume_one_phc_per_nic = true;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_avoid_efx_ioctl(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->avoid_efx = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->avoid_efx = true;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_timestamping_interfaces(struct sfptpd_config_section *section, const char *option,
					 unsigned int num_params, const char * const params[])
{
	unsigned int i, max_interfaces;
	sfptpd_config_timestamping_t *ts;
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;

	assert(num_params >= 1);

	ts = &general->timestamping;

	max_interfaces = sizeof(ts->interfaces)/sizeof(ts->interfaces[0]);

	for (i = 0; (num_params != 0) && (i < max_interfaces); i++) {
		/* If all has been specified set a flag and tidy up later */
		if (strcmp(*params, "*") == 0)
			ts->all = true;
		sfptpd_strncpy(ts->interfaces[i], *params, sizeof(ts->interfaces[i]));
		params++;
		num_params--;
	}

	ts->num_interfaces = i;

	if (num_params != 0) {
		ERROR("config [%s]: %s maximum number of interfaces (%d) exceeded\n",
		      section->name, option, max_interfaces);
		return ENOSPC;
	}

	return 0;
}


static int parse_timestamping_disable_on_exit(struct sfptpd_config_section *section, const char *option,
					      unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->timestamping.disable_on_exit = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->timestamping.disable_on_exit = true;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_pid_filter_kp(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	int tokens;
	long double kp;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%Lf", &kp);
	if (tokens != 1)
		return EINVAL;

	if ((kp < 0.0) || (kp > 1.0)) {
		CFG_ERROR(section, "pid_filter_p %s outside valid range [0,1]\n",
			  params[0]);
		return ERANGE;
	}

	general->pid_filter.kp = kp;
	return 0;
}


static int parse_pid_filter_ki(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	int tokens;
	long double ki;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%Lf", &ki);
	if (tokens != 1)
		return EINVAL;

	if ((ki < 0.0) || (ki > 1.0)) {
		CFG_ERROR(section, "pid_filter_i %s outside valid range [0,1]\n",
			  params[0]);
		return ERANGE;
	}

	general->pid_filter.ki = ki;
	return 0;
}


static int parse_trace_level(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	int tokens;

	assert(general != NULL);

	if (num_params == 1) {
		tokens = sscanf(params[0], "%i", &(general->trace_level));
		if (tokens != 1)
			return EINVAL;
	} else {
		assert(num_params == 2);

		char module_name[64];
		int trace_level;
		tokens = sscanf(params[0], "%63s", module_name);
		if (tokens != 1)
			return EINVAL;
		tokens = sscanf(params[1], "%i", &trace_level);
		if (tokens != 1)
			return EINVAL;

		if (strcmp(module_name, "general") == 0)
			general->trace_level = trace_level;
		else if (strcmp(module_name, "threading") == 0)
			general->threading_trace_level = trace_level;
		else if (strcmp(module_name, "bic") == 0)
			general->bic_trace_level = trace_level;
		else {
			ERROR("Unknown <module> argument for `trace_level`: '%s'\n", module_name);
			return EINVAL;
		}
	}

	return 0;
}


static int parse_test_mode(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	general->test_mode = true;

	return 0;
}

static int parse_json_stats(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(general != NULL);
	assert(num_params == 1);

	if (strlen(params[0]) >= PATH_MAX) {
		CFG_ERROR(section, "file name %s too long\n", params[0]);
		return EINVAL;
	}
	sfptpd_strncpy(general->json_stats_filename, params[0],
				   sizeof general->json_stats_filename);
	return 0;
}


static int parse_json_remote_monitor(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(general != NULL);
	assert(num_params == 1);

	if (strlen(params[0]) >= PATH_MAX) {
		CFG_ERROR(section, "file name %s too long\n", params[0]);
		return EINVAL;
	}
	sfptpd_strncpy(general->json_remote_monitor_filename, params[0],
				   sizeof general->json_remote_monitor_filename);
	return 0;
}


static int parse_hotplug_detection_mode(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "auto") == 0 ||
	    strcmp(params[0], "netlink-and-probe") == 0) {
		general->hotplug_detection = SFPTPD_HOTPLUG_DETECTION_INITIAL_SCAN
					   | SFPTPD_HOTPLUG_DETECTION_NETLINK
					   | SFPTPD_HOTPLUG_DETECTION_PROBE;
	} else if (strcmp(params[0], "manual") == 0) {
		general->hotplug_detection = SFPTPD_HOTPLUG_DETECTION_MANUAL;
	} else if (strcmp(params[0], "manual-with-scan") == 0) {
		general->hotplug_detection = SFPTPD_HOTPLUG_DETECTION_MANUAL
					   | SFPTPD_HOTPLUG_DETECTION_INITIAL_SCAN;
	} else if (strcmp(params[0], "netlink") == 0) {
		general->hotplug_detection = SFPTPD_HOTPLUG_DETECTION_INITIAL_SCAN
					   | SFPTPD_HOTPLUG_DETECTION_NETLINK;
	} else if (strcmp(params[0], "probe") == 0) {
		general->hotplug_detection = SFPTPD_HOTPLUG_DETECTION_INITIAL_SCAN
					   | SFPTPD_HOTPLUG_DETECTION_PROBE;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_clustering(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 4);
	long double threshold;
        int score;
	int tokens;
	if (strcmp(params[0], "discriminator") != 0) {
		CFG_ERROR(section, "mode %s is not supported. Currently the only supported mode is discriminator\n",
			  params[0]);
		return ERANGE;
	}
	general->clustering_mode = SFPTPD_CLUSTERING_MODE_DISCRIMINATOR;
	if (strlen(params[1]) >= SFPTPD_CONFIG_SECTION_NAME_MAX) {
		CFG_ERROR(section, "instance name %s too long\n",
			  params[1]);
		return ERANGE;
	}

	strcpy(general->clustering_discriminator_name, params[1]);

	tokens = sscanf(params[2], "%Lf", &threshold);
	if (tokens != 1)
		return EINVAL;

	/* User supplied parameter already in ns, no need to convert */
	general->clustering_discriminator_threshold = threshold;

	tokens = sscanf(params[3], "%d", &score);
	if (tokens != 1)
		return EINVAL;

	general->clustering_score_without_discriminator = score;

	return 0;
}


static int parse_clustering_guard_threshold(struct sfptpd_config_section *section, const char *option,
				   unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 2);
	int threshold;
	int tokens;
        
	if (strcmp(params[0], "off") == 0) {
		general->clustering_guard_enabled = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->clustering_guard_enabled = true;
	} else {
		return EINVAL;
	}

	tokens = sscanf(params[1], "%d", &threshold);
	if (tokens != 1)
		return EINVAL;

	if (threshold != 1) {
		CFG_ERROR(section, "currently only a clustering threshold of 1 is supported\n");
		return ERANGE;
	}

	general->clustering_guard_threshold = threshold;

	return 0;
}


static int parse_limit_freq_adj(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	int tokens;
	long double freq_adj;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%Lf", &freq_adj);
	if (tokens != 1)
		return EINVAL;

	general->limit_freq_adj = freq_adj;
	return 0;
}


static int parse_ignore_critical(struct sfptpd_config_section *section, const char *option,
			         unsigned int num_params, const char * const params[])
{
	int rc = 0;
	int i;
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;

	assert(num_params >= 1);

	for (i = 0; i < num_params; i++) {
		if (!strcmp(params[i], "no-ptp-clock"))
			general->ignore_critical[SFPTPD_CRITICAL_NO_PTP_CLOCK] = true;
		else if (!strcmp(params[i], "no-ptp-subsystem"))
			general->ignore_critical[SFPTPD_CRITICAL_NO_PTP_SUBSYSTEM] = true;
		else if (!strcmp(params[i], "clock-control-conflict"))
			general->ignore_critical[SFPTPD_CRITICAL_CLOCK_CONTROL_CONFLICT] = true;
		else
			rc = EINVAL;
	}

	return rc;
}


static int parse_rtc_adjust(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->rtc_adjust = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->rtc_adjust = true;
	} else {
		return EINVAL;
	}

	return 0;
}


static int validate_config(struct sfptpd_config_section *parent)
{
	struct sfptpd_config *config = parent->config;
	struct sfptpd_config_section *section, *new;

	/* Ensure an crny sync instance is declared */
	section = sfptpd_config_find(config, SFPTPD_CRNY_MODULE_NAME);
	assert(section != NULL);
	assert(section->scope != SFPTPD_CONFIG_SCOPE_INSTANCE);
	assert(section->ops.create != NULL);

	if ((((sfptpd_config_general_t *) parent)->declared_sync_modules & (1 << SFPTPD_CONFIG_CATEGORY_CRNY)) == 0) {
		new = section->ops.create(NULL, SFPTPD_CONFIG_SCOPE_INSTANCE,
					  false, section);
		if (new == NULL) {
			CFG_ERROR(parent, "failed to create implicit crny instance\n");
			return ENOMEM;
		}

		sfptpd_config_section_add(config, new);
		TRACE_L1("config: created crny implicit instance %s\n", new->name);
	}

	return 0;
}


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static void general_config_destroy(struct sfptpd_config_section *section)
{
	assert(section != NULL);
	assert(section->category == SFPTPD_CONFIG_CATEGORY_GENERAL);
	free(section);
}


static struct sfptpd_config_section *general_config_create(const char *name,
							   enum sfptpd_config_scope scope,
							   bool allows_instances,
							   const struct sfptpd_config_section *src)
{
	struct sfptpd_config_general *new;

	assert((src == NULL) || (src->category == SFPTPD_CONFIG_CATEGORY_GENERAL));

	new = (struct sfptpd_config_general *)calloc(1, sizeof(*new));
	if (new == NULL) {
		ERROR("failed to allocate memory for general configuration\n");
		return NULL;
	}

	/* If the source isn't null, copy the section contents. Otherwise,
	 * initialise with the default values. */
	if (src != NULL) {
		memcpy(new, src, sizeof(*new));
	} else {
		new->config_filename[0] = '\0';
		new->message_log = SFPTPD_DEFAULT_MESSAGE_LOG;
		new->stats_log = SFPTPD_DEFAULT_STATS_LOG;
		new->stats_log_filename[0] = '\0';
		new->trace_level = SFPTPD_DEFAULT_TRACE_LEVEL;
		sfptpd_strncpy(new->state_path, SFPTPD_DEFAULT_STATE_PATH, sizeof(new->state_path));
		sfptpd_strncpy(new->control_path, SFPTPD_DEFAULT_CONTROL_PATH, sizeof(new->control_path));

		new->clocks.sync_interval = SFPTPD_DEFAULT_SYNC_INTERVAL;
		new->clocks.control = SFPTPD_DEFAULT_CLOCK_CTRL;
		new->clocks.persistent_correction = SFPTPD_DEFAULT_PERSISTENT_CLOCK_CORRECTION;
		new->clocks.discipline_all = SFPTPD_DEFAULT_DISCIPLINE_ALL_CLOCKS;
		new->clocks.num_clocks = 0;
		new->epoch_guard = SFPTPD_DEFAULT_EPOCH_GUARD;

		new->non_sfc_nics = SFPTPD_DEFAULT_NON_SFC_NICS;
		new->assume_one_phc_per_nic = SFPTPD_DEFAULT_ASSUME_ONE_PHC_PER_NIC;
		new->test_mode = false;
		new->daemon = false;
		new->lock = true;
		new->rtc_adjust = SFPTPD_DEFAULT_RTC_ADJUST;

		new->timestamping.all = false;
		new->timestamping.disable_on_exit = true;
		new->timestamping.num_interfaces = 0;

		new->convergence_threshold = 0.0;
		new->initial_sync_instance[0] = '\0';
		new->selection_holdoff_interval = SFPTPD_DEFAULT_SELECTION_HOLDOFF_INTERVAL;

		new->pid_filter.kp = SFPTPD_DEFAULT_SERVO_K_PROPORTIONAL;
		new->pid_filter.ki = SFPTPD_DEFAULT_SERVO_K_INTEGRAL;

		new->selection_policy = sfptpd_default_selection_policy;
                memcpy(new->phc_diff_methods, sfptpd_default_phc_diff_methods, sizeof(sfptpd_default_phc_diff_methods));
		assert(sizeof new->phc_pps_method == sizeof sfptpd_default_pps_method);
		memcpy(new->phc_pps_method, sfptpd_default_pps_method, sizeof new->phc_pps_method);

		new->json_stats_filename[0] = '\0';
		new->json_remote_monitor_filename[0] = '\0';

		new->hotplug_detection = SFPTPD_DEFAULT_HOTPLUG_DETECTION;

		new->clustering_mode = SFPTPD_DEFAULT_CLUSTERING_MODE;
                new->clustering_guard_enabled = SFPTPD_DEFAULT_CLUSTERING_GUARD;
		new->clustering_guard_threshold = SFPTPD_DEFAULT_CLUSTERING_GUARD_THRESHOLD;
		new->clustering_discriminator_name[0] = '\0';
		new->clustering_discriminator_threshold = 0;
		new->clustering_score_without_discriminator = SFPTPD_DEFAULT_CLUSTERING_SCORE_ABSENT_DISCRIM;

		new->limit_freq_adj = 1.0E9;

		new->declared_sync_modules = 0;
	}

	sfptpd_config_section_init(&new->hdr, general_config_create,
				   general_config_destroy,
				   SFPTPD_CONFIG_CATEGORY_GENERAL,
				   scope, allows_instances, name);

	return &new->hdr;
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_general_config_init(struct sfptpd_config *config)
{
	struct sfptpd_config_general *new;
	assert(config != NULL);

	/* Initialise the general configuration section and add it to the
	 * configuration. */
	new = (struct sfptpd_config_general *)
		general_config_create(config_general_name,
				      SFPTPD_CONFIG_SCOPE_GLOBAL, false, NULL);
	if (new == NULL)
		return ENOMEM;

	/* Add the section to the configuration */
	sfptpd_config_section_add(config, &new->hdr);

	/* Register the general configuration options */
	sfptpd_config_register_options(&config_general_option_set);
	return 0;
}


struct sfptpd_config_general *sfptpd_general_config_get(struct sfptpd_config *config)
{
	struct sfptpd_config_section *section;
	assert(config != NULL);

	section = sfptpd_config_category_global(config, SFPTPD_CONFIG_CATEGORY_GENERAL);
	assert(section != NULL);
	return (struct sfptpd_config_general *)section;
}


void sfptpd_config_set_config_file(struct sfptpd_config *config,
				   char *filename)
{
	struct sfptpd_config_general *general = sfptpd_general_config_get(config);

	/* Take a copy of the config file name */
	sfptpd_strncpy(general->config_filename, filename,
		       sizeof(general->config_filename));
	TRACE_L4("using config file %s\n", general->config_filename);
}


void sfptpd_config_general_set_verbose(struct sfptpd_config *config)
{
	struct sfptpd_config_general *general = sfptpd_general_config_get(config);

	general->message_log = SFPTPD_MSG_LOG_TO_STDERR;
	general->stats_log = SFPTPD_STATS_LOG_TO_STDOUT;
	if (general->trace_level < 3)
		general->trace_level = 3;
	sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_SFPTPD,
				   general->trace_level);
}


/* fin */
