/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2025 Advanced Micro Devices, Inc. */

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
#include <endian.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <pwd.h>
#include <grp.h>

#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_misc.h"
#include "sfptpd_statistics.h"
#include "sfptpd_phc.h"
#include "sfptpd_crny_module.h"
#include "sfptpd_config_helpers.h"
#include "sfptpd_filter.h"


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
static int parse_user(struct sfptpd_config_section *section, const char *option,
		      unsigned int num_params, const char * const params[]);
static int parse_daemon(struct sfptpd_config_section *section, const char *option,
			unsigned int num_params, const char * const params[]);
static int parse_lock(struct sfptpd_config_section *section, const char *option,
		      unsigned int num_params, const char * const params[]);
static int parse_state_path(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[]);
static int parse_control_path(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[]);
static int parse_metrics_path(struct sfptpd_config_section *section, const char *option,
			      unsigned int num_params, const char * const params[]);
static int parse_run_dir(struct sfptpd_config_section *section, const char *option,
			 unsigned int num_params, const char * const params[]);
static int parse_access_mode(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[]);
static int parse_sync_interval(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[]);
static int parse_sync_threshold(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[]);
static int parse_clock_control(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[]);
static int parse_step_threshold(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[]);
static int parse_epoch_guard(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[]);
static int parse_clock_list(struct sfptpd_config_section *section, const char *option,
			    unsigned int num_params, const char * const params[]);
static int parse_clock_readonly(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[]);
static int parse_observe_readonly_clocks(struct sfptpd_config_section *section, const char *option,
					 unsigned int num_params, const char * const params[]);
static int parse_persistent_clock_correction(struct sfptpd_config_section *section, const char *option,
					     unsigned int num_params, const char * const params[]);
static int parse_non_solarflare_nics(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[]);
static int parse_assume_one_phc_per_nic(struct sfptpd_config_section *section, const char *option,
					unsigned int num_params, const char * const params[]);
static int parse_phc_dedup(struct sfptpd_config_section *section, const char *option,
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
static int parse_fir_filter_size(struct sfptpd_config_section *section, const char *option,
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
static int parse_reporting_intervals(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[]);
static int parse_netlink_rescan_interval(struct sfptpd_config_section *section, const char *option,
					 unsigned int num_params, const char * const params[]);
static int parse_netlink_coalesce_ms(struct sfptpd_config_section *section, const char *option,
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
static int parse_clock_display_fmts(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[]);
static int parse_unique_clockid_bits(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[]);
static int parse_legacy_clockids(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[]);
static int parse_initial_clock_correction(struct sfptpd_config_section *section, const char *option,
					  unsigned int num_params, const char * const params[]);
static int parse_openmetrics_unix(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[]);
static int parse_openmetrics_tcp(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[]);
static int parse_openmetrics_rt_stats_buf(struct sfptpd_config_section *section, const char *option,
					  unsigned int num_params, const char * const params[]);
static int parse_openmetrics_options(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[]);
static int parse_openmetrics_prefix(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[]);
static int parse_openmetrics_acl_allow(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[]);
static int parse_openmetrics_acl_deny(struct sfptpd_config_section *section, const char *option,
				      unsigned int num_params, const char * const params[]);
static int parse_openmetrics_acl_order(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[]);
static int parse_servo_log_all_samples(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[]);
static int parse_eligible_interface_types(struct sfptpd_config_section *section, const char *option,
					  unsigned int num_params, const char * const params[]);
static int parse_clock_adj_method(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[]);

static int validate_config(struct sfptpd_config_section *section);

static void destroy_interface_selection(struct sfptpd_config_interface_selection **selection);

static const sfptpd_config_option_t config_general_options[] =
{
	/* Generic config options */
	{"sync_module", "<freerun | ptp | pps | ntp | crny> [instance-names]",
		"Create instances of the specified sync module",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_sync_module},
	{"selection_policy", "<automatic | manual | manual-startup> [initial-instance]",
		"Use automatic (default), manual or manual followed by "
		"automatic sync instance selection",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_selection_policy},
	{"selection_policy_rules", "<manual | ext-constraints | state | no-alarms | user-priority | clustering | clock-class | total-accuracy | allan-variance | steps-removed>*",
		"Define the list of rules for the automatic selection policy",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_selection_policy_rules},
	{"phc_pps_methods", "<devpps | devptp>*",
		"Define the order of non-proprietary PPS methods to try",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_phc_pps_methods},
	{"selection_holdoff_interval", "NUMBER",
		"Specifies how long to wait after detecting a better instance "
		"before selecting it",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_selection_holdoff_interval,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_SELECTION_HOLDOFF_INTERVAL),
		.unit = "seconds"},
	{"message_log", "<syslog | stderr | filename>",
		"Specifies where to send messages generated by the application",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_message_log,
		.dfl = "By default messages are sent to stderr"},
	{"stats_log", "<off | stdout | filename>",
		"Specifies if and where to log statistics generated by the application",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_stats_log,
		.dfl = "By default statistics logging is disabled"},
	{"user", "USER [GROUP]",
		"Drop to the user and group named USER and GROUP retaining "
		"essential capabilities. Group defaults to USER's if not "
		"specified",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_user},
	{"daemon", "",
		"Run as a daemon.",
		0, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_daemon,
		.dfl = "Disabled by default"},
	{"lock", "<off | on>",
		"Specify whether to use a lock file to stop multiple simultaneous instances of the daemon",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_lock,
		.dfl = "Enabled by default"},
	{"state_path", "<path>",
		"Directory in which to store sfptpd state data",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_state_path,
		.dfl = SFPTPD_CONFIG_DFL_STR(SFPTPD_DEFAULT_STATE_PATH)},
	{"control_path", "<path>",
		"Path for Unix domain control socket",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_control_path,
		.dfl = SFPTPD_CONFIG_DFL_STR(SFPTPD_DEFAULT_CONTROL_PATH)},
	{"metrics_path", "<path>",
		"Path for Unix domain socket serving OpenMetrics",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_metrics_path,
		.dfl = SFPTPD_CONFIG_DFL_STR(SFPTPD_DEFAULT_METRICS_PATH)},
	{"run_dir", "<path>",
		"Path for run directory",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_run_dir,
		.dfl = SFPTPD_CONFIG_DFL_STR(SFPTPD_DEFAULT_RUN_DIR)},
	{"run_dir_mode", "MODE",
		"Specifies MODE with which to create run directory, subject to umask",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_access_mode,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_RUN_DIR_MODE)},
	{"state_dir_mode", "MODE",
		"Specifies MODE with which to create state directory, subject to umask",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_access_mode,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_STATE_DIR_MODE)},
	{"control_socket_mode", "MODE",
		"Specifies access MODE to give control socket, subject to umask",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_access_mode,
		.dfl = "By default, leave as created."},
	{"metrics_socket_mode", "MODE",
		"Specifies access MODE to give metrics socket, subject to umask",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_access_mode,
		.dfl = "By default, leave as created."},
	{"sync_interval", "NUMBER",
		"Specifies the interval in 2^NUMBER seconds at which the clocks "
		"are synchronized to the local reference clock, where NUMBER is "
		"in the range ["
		STRINGIFY(SFPTPD_MIN_SYNC_INTERVAL) ","
		STRINGIFY(SFPTPD_MAX_SYNC_INTERVAL) "]",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_sync_interval,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_SYNC_INTERVAL)},
	{"local_sync_threshold", "NUMBER",
		"Threshold in nanoseconds of the offset between the system clock and a NIC clock over a "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT)
		"s period to be considered in sync",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_sync_threshold,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT)},
	{"clock_control", "<slew-and-step | step-at-startup | no-step | no-adjust | step-forward | step-on-first-lock>",
		"Specifies how the clocks are controlled",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_clock_control,
		.dfl = "By default clocks are stepped and slewed as necessary"},
	{"step_threshold", "NUMBER",
		"Threshold in seconds of the offset between the clock and its reference clock for sfptpd to step",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_step_threshold,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_SERVO_CLOCK_STEP_THRESHOLD_S),
		.unit = "s"},
	{"epoch_guard", "<alarm-only | prevent-sync | correct-clock>",
		"Guards against propagation of times near the epoch",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_epoch_guard,
		.dfl = "The default is correct-clock"},
	{"clock_list", "[<name | mac-address | clock-id | ifname>]*",
		"Specifies the set of clocks that sfptpd should discipline",
		~0, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_clock_list,
		.dfl = "By default all clocks are disciplined"},
	{"clock_readonly", "[<name | mac-address | clock-id | ifname>]",
		"Specifies a set of clocks that sfptpd should never step or slew, under any circumstance. Use with care.",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_clock_readonly},
	{"observe_readonly_clocks", "<off | on>",
		"Specifies whether to observe read-only clocks with passive servos",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_observe_readonly_clocks,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_OBSERVE_READONLY_CLOCKS)},
	{"persistent_clock_correction", "<off | on>",
		"Specifies whether to used saved clock frequency corrections when disciplining clocks",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_persistent_clock_correction,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_PERSISTENT_CLOCK_CORRECTION)},
	{"non_solarflare_nics", "<off | on>",
		"Specify whether to use timestamping and hardware clock "
		"capabilities of non-Solarflare adapters",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_non_solarflare_nics,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_NON_SFC_NICS)},
	{"non_xilinx_nics", "<off | on>",
		"Alias for non_solarflace_nics",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_non_solarflare_nics,
		.hidden = true},
	{"assume_one_phc_per_nic", "<off | on>",
		"Specify whether multiple reported clock devices on a NIC "
		"should be assumed to represent the same underlying clock",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_assume_one_phc_per_nic,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_ASSUME_ONE_PHC_PER_NIC)},
	{"phc_dedup", "<off | on>",
		"Specify whether to identify duplicate PHC devices for the same clock",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_phc_dedup,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_PHC_DEDUP)},
	{"avoid_efx_ioctl", "<off | on>",
		"Specify whether to avoid private SIOCEFX ioctl for Solarflare adapters. "
		"This prevents use of the sync flag via Onload",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_avoid_efx_ioctl,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(false),
		},
	{"phc_diff_methods", "<sys-offset-precise | efx | pps | sys-offset-ext | sys-offset | read-time>*",
		"Define the list of PHC diff methods used",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_phc_diff_method_order},
	{"timestamping_interfaces", "[<name | mac-address | *>]",
		"Specifies set of interfaces on which general receive packet timestamping should be enabled",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_timestamping_interfaces},
	{"timestamping_disable_on_exit", "<off | on>",
		"Specifies whether timestamping should be disabled when daemon exits",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_timestamping_disable_on_exit,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_DISABLE_ON_EXIT)},
	{"pid_filter_p", "NUMBER",
		"Secondary servo PID filter proportional term coefficient",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pid_filter_kp,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_SERVO_K_PROPORTIONAL)},
	{"pid_filter_i", "NUMBER",
		"Secondary servo PID filter integral term coefficient",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pid_filter_ki,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_SERVO_K_INTEGRAL)},
	{"fir_filter_size", "NUMBER",
		"Number of data samples stored in the FIR filter. The "
		"valid range is [" STRINGIFY(SFPTPD_FIR_FILTER_STIFFNESS_MIN)
		"," STRINGIFY(SFPTPD_FIR_FILTER_STIFFNESS_MAX) "]. A value of "
		"1 means that the filter is off while higher values will "
		"reduce adaptability but increase stability. "
		"Default is one second's worth of samples.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_fir_filter_size},
	{"trace_level", "[<general | threading | bic | netlink | ntp | servo | clocks | most | all>] NUMBER",
		"Specifies a module (of 'general' if omitted) trace level from 0 (none) to 6 (excessive)",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_trace_level,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_TRACE_LEVEL)},
	{"test_mode", "",
		"Enables features to aid testing",
		0, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_test_mode,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_TEST_MODE),
		.hidden = true},
	{"json_stats", "<filename>",
		"Output realtime module statistics in JSON-lines format to this file (http://jsonlines.org)",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_json_stats,
		.dfl = "Disabled by default"},
	{"json_remote_monitor", "<filename>",
		"Write JSON lines to this file for data collected by the PTP "
		"remote monitor (which is DEPRECATED in favour of sfptpmon)",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_json_remote_monitor,
		.dfl = "Disabled by default"},
	{"hotplug_detection_mode", "<netlink | auto>",
		"obsolete option to control how interface and bond changes are "
		"detected. The option value is ignored and netlink used.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_hotplug_detection_mode},
	{"reporting_intervals", "<save_state|stats_log INTERVAL>*",
		"Specifies period between saving state files and/or writing "
		"stats log output",
		~2, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_reporting_intervals,
		.dfl = "Default is: \"save_state "
		STRINGIFY(SFPTPD_DEFAULT_STATE_SAVE_INTERVAL) " stats_log "
		STRINGIFY(SFPTPD_DEFAULT_STATISTICS_LOGGING_INTERVAL) "\""},
	{"netlink_rescan_interval", "NUMBER",
		"Specifies period between rescanning the link table with netlink. "
		"Periodic rescans are disabled with zero",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_netlink_rescan_interval,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_NETLINK_RESCAN_INTERVAL),
		.unit = "seconds"},
	{"netlink_coalesce_ms", "NUMBER",
		"Specifies period after a significant change is communicated "
		"by netlink to wait for further changes to avoid excessive "
		"perturbation. Coalescing is disabled with zero",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_netlink_coalesce_ms,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_NETLINK_COALESCE_MS),
		.unit =  "ms"},
	{"clustering", "discriminator <INSTANCE> <THRESHOLD> <NO_DISCRIMINATOR_SCORE>",
		"Implements clustering based on MODE. Currently only supports "
		"discriminator mode, which disqualifies sync instances that differ "
		"from discriminator INSTANCE in excess of THRESHOLD ns. INSTANCE must "
		"be a sync instance name. NO_DISCRIMINATOR_SCORE is the clustering "
                "score returned when no discriminator is available.",
		4, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_clustering},
	{"clustering_guard", "<off | on> <THRESHOLD>",
		"Specifies whether to turn on the clusterig guard feature, as well as "
                "the threshold for clustering score to be compared to.",
		2, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_clustering_guard_threshold},
	{"limit_freq_adj", "NUMBER",
		"Limit NIC clock frequency adjustment to the lesser of "
		"advertised capability and NUMBER ppb.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_limit_freq_adj},
	{"ignore_critical", "<no-ptp-clock | no-ptp-subsystem | clock-control-conflict>*",
		"Ignore certain critical warnings that would normally "
		"terminate execution but may be expected in some niche "
		"or diagnostic use cases.",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_ignore_critical},
	{"rtc_adjust", "<off | on>",
		"Specify whether to let the kernel sync the RTC clock",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_rtc_adjust,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_RTC_ADJUST)},
	{"clock_display_fmts", "SHORT-FMT LONG-FMT HWID-FMT FNAM-FMT",
		"Define formats for displaying clock properties, "
		"of max expansion "
		STRINGIFY(SFPTPD_CLOCK_SHORT_NAME_SIZE) ", "
		STRINGIFY(SFPTPD_CLOCK_FULL_NAME_SIZE) ", "
		STRINGIFY(SFPTPD_CLOCK_HW_ID_STRING_SIZE) " and "
		STRINGIFY(SFPTPD_CLOCK_HW_ID_STRING_SIZE) " respectively. ",
		4, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_clock_display_fmts,
		.dfl = "Default is "
			SFPTPD_DEFAULT_CLOCK_SHORT_FMT " "
			SFPTPD_DEFAULT_CLOCK_LONG_FMT " "
			SFPTPD_DEFAULT_CLOCK_HWID_FMT " "
			SFPTPD_DEFAULT_CLOCK_FNAM_FMT "."},
	{"unique_clockid_bits", "<OCTETS | pid | hostid | rand>",
		"Colon-delimited octets providing the unique bits that pad the "
		"LSBs of an EUI-64 clock identity constructed from an EUI-48 "
		"MAC address",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_unique_clockid_bits,
		.dfl = SFPTPD_CONFIG_DFL_STR(SFPTPD_DEFAULT_UNIQUE_CLOCKID_BITS)},
	{"legacy_clockids", "<off | on>",
		"Use legacy 1588-2008 clock ids of the form :::ff:fe:::",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_legacy_clockids,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(false)},
	{"initial_clock_correction", "<always | if-unset>",
		"When to apply an initial clock correction to NIC clocks",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_initial_clock_correction,
		.dfl = "Defaults to always"},
	{"openmetrics_unix", "<off | on>",
		"Whether to serve OpenMetrics exposition over socket in filesystem",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_openmetrics_unix,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_OPENMETRICS_UNIX)},
	{"openmetrics_tcp", "LISTEN-ADDR*",
		"Addresses to listen on to serve OpenMetrics exposition over TCP",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_openmetrics_tcp,
		.dfl = "Defaults to no TCP listener"},
	{"openmetrics_rt_stats_buf", "NUMBER",
		"NUMBER of real time stats entries to buffer for OpenMetrics",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_openmetrics_rt_stats_buf,
		.dfl = SFPTPD_CONFIG_DFL(SFPTPD_DEFAULT_OPENMETRICS_RT_STATS_BUF)},
	{"openmetrics_options", "[alarm-stateset]",
		"set openmetrics option flags",
		~0, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_openmetrics_options},
	{"openmetrics_prefix", "PREFIX",
		"set prefix string to be prepended to OpenMetrics family names",
		~0, SFPTPD_CONFIG_SCOPE_GLOBAL, parse_openmetrics_prefix,
		.dfl = SFPTPD_CONFIG_DFL_STR(SFPTPD_DEFAULT_OPENMETRICS_PREFIX)},
	{"openmetrics_acl_allow", "<ip-address-list>",
		"Access control allow list for metrics connections. The format "
		"is a series of network prefixes in a.b.c.d/x notation where "
		"a.b.c.d is the subnet and x is the prefix length. For single "
		"IP addresses, 32 or 128 should be specified for the length.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_openmetrics_acl_allow},
	{"openmetrics_acl_deny", "<ip-address-list>",
		"Access control deny list for metrics connections.",
		~1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_openmetrics_acl_deny},
	{"openmetrics_acl_order", "<allow-deny | deny-allow>",
		"Access control list evaluation order for metrics connections. "
		"Default allow-deny.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_openmetrics_acl_order},
	{"servo_log_all_samples", "<off | on>",
		"Specify whether to log every sample from secondary servos",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_servo_log_all_samples,
		.dfl = SFPTPD_CONFIG_DFL_BOOL(SFPTPD_DEFAULT_SERVO_LOG_ALL_SAMPLES)},
	{"eligible_interface_types", "([+|-]<$group|kind>[@prop|!prop]* )*",
		"Specify list of eligible interface types by group, if_kind "
		"and conditional properties",
		~1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_eligible_interface_types,
		.dfl = SFPTPD_CONFIG_DFL_STR(SFPTPD_DEFAULT_PHYSICAL_INTERFACES)},
	{"clock_adj_method", "<prefer-tickadj | prefer-freqadj>",
		"Specify whether tick length or frequency adjustment should "
		"be used for most of the frequency correction",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_clock_adj_method,
		.dfl = "prefer-tickadj"},
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


static int parse_user(struct sfptpd_config_section *section, const char *option,
		      unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	struct passwd pwd, *presult = NULL;
	struct group grp, *gresult = NULL;
	char *buf;
	size_t buf_sz;
	int n_groups;
	int tokens;
	long nid;
	int ret;
	int rc;

	assert(general != NULL);
	assert(num_params >= 1);

	if (num_params > 2)
		return EINVAL;

#ifndef HAVE_CAPS
	CFG_ERROR(section, "sfptpd must be built with libcap to enable "
		  "a non-root user to be configured\n");
	return EACCES;
#endif

	if (general->uid != 0 || general->gid != 0) {
		CFG_ERROR(section, "non-root user or group already configured - not overwriting\n");
		return EACCES;
	}

	buf_sz = MAX(sysconf(_SC_GETPW_R_SIZE_MAX), 1024);
	buf = malloc(buf_sz);
	assert(buf != NULL);

	rc = getpwnam_r(params[0], &pwd, buf, buf_sz, &presult);
	while (rc == ERANGE) {
		buf_sz <<= 1;
		buf = realloc(buf, buf_sz);
		rc = getpwnam_r(params[0], &pwd, buf, buf_sz, &presult);
	}
	if (rc == 0 && presult != &pwd)
		rc = ENOENT;
	if (rc == ENOENT || rc == ESRCH || rc == EBADF || rc == EPERM) {
		/* Look up user id if name did not resolve. */

		tokens = sscanf(params[0], "%ld", &nid);
		if (tokens == 1) {
			general->uid = nid;
			general->gid = 0;
		} else {
			CFG_ERROR(section, "user %s not known\n", params[0]);
			rc = ENOENT;
			goto finish;
		}
		rc = getpwuid_r(general->uid, &pwd, buf, buf_sz, &presult);
		if (rc == 0 && presult != &pwd)
			rc = ENOENT;
		if (rc == ENOENT || rc == ESRCH || rc == EBADF || rc == EPERM)
				pwd.pw_name = "(uid)";
	}
	if (rc == 0) {
		general->uid = pwd.pw_uid;
		general->gid = pwd.pw_gid;
	}

	TRACE_L3("configured to switch to user %s %d\n",
	     pwd.pw_name,
	     pwd.pw_uid);

	/* Look up group name if specified */
	if (num_params > 1) {
		rc = getgrnam_r(params[1], &grp, buf, buf_sz, &gresult);
		if (rc == ENOENT || rc == ESRCH || rc == EBADF || rc == EPERM || gresult == NULL) {
			tokens = sscanf(params[1], "%ld", &nid);
			if (tokens == 1) {
				general->gid = nid;
			} else {
				CFG_ERROR(section, "group %s not known\n", params[1]);
				rc = ENOENT;
				goto finish;
			}
		} else {
			general->gid = grp.gr_gid;
		}
	}

	/* Look up group id otherwise or if name did not resolve. */
	if (num_params <= 1 || gresult == NULL) {
		if (getgrgid_r(general->gid, &grp, buf, buf_sz, &gresult) != 0 || gresult == NULL)
			grp.gr_name = "(gid)";
	}
	if (rc == 0) {
		TRACE_L3("configured to switch to group %s %d\n",
		     grp.gr_name,
		     general->gid);
	}

	/* Fill out list of groups to join.
	 * Size the list first. Returns value of n_groups or -1.
	 * Expect -1 return code as there should be insufficient
	 * space for the >= 1 groups expected. */
	n_groups = 0;
	ret = getgrouplist(pwd.pw_name, general->gid, NULL, &n_groups);
	if (ret == -1) {
		general->num_groups = n_groups;
		general->groups = calloc(n_groups, sizeof(gid_t));
		if (general->groups == NULL) {
			rc = errno;
			CRITICAL("allocating groups list, %s\n", strerror(rc));
			goto finish;
		}

		ret = getgrouplist(pwd.pw_name, general->gid, general->groups, &n_groups);
		if (general->num_groups != n_groups) {
			CRITICAL("groups list size changed during startup\n");
			rc = EACCES;
			goto finish;
		}
	}
	/* No other case is expected from the initial call but if there is,
	 * the empty list is the right fallback. */
	TRACE_L5("found %d groups to join\n", general->num_groups);

finish:
	free(buf);
	return rc;
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


static int parse_metrics_path(struct sfptpd_config_section *section, const char *option,
			      unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	sfptpd_strncpy(general->metrics_path, params[0],
		       sizeof(general->metrics_path));

	return 0;
}


static int parse_run_dir(struct sfptpd_config_section *section, const char *option,
			 unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	sfptpd_strncpy(general->run_dir, params[0],
		       sizeof(general->run_dir));

	return 0;
}

static int parse_access_mode(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);
	int tokens;
	int mode;

	tokens = sscanf(params[0], "%o", &mode);
	if (tokens != 1)
		return EINVAL;

	if (!strcmp(option, "state_dir_mode"))
		general->state_dir_mode = mode;
	else if (!strcmp(option, "run_dir_mode"))
		general->run_dir_mode = mode;
	else if (!strcmp(option, "control_socket_mode"))
		general->control_socket_mode = mode;
	else if (!strcmp(option, "metrics_socket_mode"))
		general->metrics_socket_mode = mode;
	else
		return EINVAL;

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

static int parse_step_threshold(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	int tokens;
	long double threshold;
	assert(num_params == 1);
	tokens = sscanf(params[0], "%Lf", &threshold);
	if (tokens != 1)
		return EINVAL;

	if ((threshold < SFPTPD_SERVO_CLOCK_STEP_THRESHOLD_S_MIN) || (threshold > SFPTPD_SERVO_CLOCK_STEP_THRESHOLD_S_MAX)) {
			  CFG_ERROR(section, "step_threshold %s outside valid range ["
			  STRINGIFY(SFPTPD_SERVO_CLOCK_STEP_THRESHOLD_S_MIN) ","
			  STRINGIFY(SFPTPD_SERVO_CLOCK_STEP_THRESHOLD_S_MAX) "]\n",
			  params[0]);
		return ERANGE;
	}

	general->step_threshold = threshold * ONE_BILLION;
	return 0;
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

static int parse_initial_clock_correction(struct sfptpd_config_section *section, const char *option,
					  unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;

	assert(num_params == 1);

	if (strcmp(params[0], "always") == 0) {
		general->initial_clock_correction = SFPTPD_CLOCK_INITIAL_CORRECTION_ALWAYS;
	} else if (strcmp(params[0], "if-unset") == 0) {
		general->initial_clock_correction = SFPTPD_CLOCK_INITIAL_CORRECTION_IF_UNSET;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_openmetrics_unix(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[])
{
	int rc = 0;
	struct sfptpd_config_metrics *metrics = &((sfptpd_config_general_t *)section)->openmetrics;

	assert(num_params == 1);

	if (strcmp(params[0], "on") == 0) {
		metrics->unix = true;
	} else if (strcmp(params[0], "off") == 0) {
		metrics->unix = false;
	} else {
		rc = EINVAL;
	}

	return rc;
}

static int parse_openmetrics_tcp(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	struct sfptpd_config_metrics *metrics = &((sfptpd_config_general_t *)section)->openmetrics;
	int rc = 0;
	int i;

	metrics->tcp = calloc(num_params, sizeof *metrics->tcp);
	metrics->num_tcp_addrs = num_params;
	if (metrics->tcp == NULL)
		return errno;
	else
		metrics->num_tcp_addrs = num_params;

	for (i = 0; i < num_params; i++) {
		rc = sfptpd_config_parse_net_addr(metrics->tcp + i, params[i],
						  "metrics",
						  AF_UNSPEC, SOCK_STREAM, true,
						  SFPTPD_EXPORTER_PORT);
		if (rc < 0)
			return -rc;
	}
	return 0;
}

static int parse_openmetrics_rt_stats_buf(struct sfptpd_config_section *section, const char *option,
					  unsigned int num_params, const char * const params[])
{
	struct sfptpd_config_metrics *metrics = &((sfptpd_config_general_t *)section)->openmetrics;
	assert(num_params == 1);
	int tokens, count;
	const static int minimum = 16;

	tokens = sscanf(params[0], "%i", &count);
	if (tokens != 1)
		return EINVAL;

	if (count < minimum) {
		ERROR("config [%s]: %s must exceed %d\n",
		      section->name, option, minimum);
		return ERANGE;
	}

	metrics->rt_stats_buf = count;

	return 0;
}

static int parse_openmetrics_options(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	struct sfptpd_config_metrics *metrics = &((sfptpd_config_general_t *)section)->openmetrics;
	enum sfptpd_metrics_option opt;

	/* Zero out any defaults */
	metrics->flags = 0;

	/* Then set specified flags */
	while (num_params--) {
		for (opt = 0;
		     opt < SFPTPD_METRICS_NUM_OPTIONS &&
		     strcmp(params[num_params], sfptpd_metrics_option_names[opt]);
		     opt++);
		if (opt == SFPTPD_METRICS_NUM_OPTIONS) {
			ERROR("config [%s]: invalid %s option: %s\n",
			      section->name, option, params[num_params]);
			return EINVAL;
		}
		metrics->flags |= (1 << opt);
	}
	return 0;
}

static int parse_openmetrics_prefix(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	struct sfptpd_config_metrics *metrics = &((sfptpd_config_general_t *)section)->openmetrics;
	assert(num_params == 1);

	sfptpd_strncpy(metrics->family_prefix, params[0],
		       sizeof(metrics->family_prefix));

	return 0;
}

static int parse_openmetrics_acl_allow(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[])
{
	struct sfptpd_config_metrics *metrics = &((sfptpd_config_general_t *)section)->openmetrics;

	assert(num_params >= 1);

	if (metrics->acl.order == SFPTPD_ACL_ALLOW_ALL)
		metrics->acl.order = SFPTPD_ACL_ALLOW_DENY;

	return sfptpd_acl_table_create(&metrics->acl.allow,
				       "metrics allow", num_params, params);
}

static int parse_openmetrics_acl_deny(struct sfptpd_config_section *section, const char *option,
				      unsigned int num_params, const char * const params[])
{
	struct sfptpd_config_metrics *metrics = &((sfptpd_config_general_t *)section)->openmetrics;

	assert(num_params >= 1);

	if (metrics->acl.order == SFPTPD_ACL_ALLOW_ALL)
		metrics->acl.order = SFPTPD_ACL_ALLOW_DENY;

	return sfptpd_acl_table_create(&metrics->acl.deny,
				       "metrics deny", num_params, params);
}

static int parse_openmetrics_acl_order(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[])
{
	struct sfptpd_config_metrics *metrics = &((sfptpd_config_general_t *)section)->openmetrics;

	assert(num_params == 1);
	return sfptpd_config_parse_acl_order(&metrics->acl.order, params[0]);
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


static int parse_observe_readonly_clocks(struct sfptpd_config_section *section, const char *option,
					 unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->clocks.observe_readonly = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->clocks.observe_readonly = true;
	} else {
		return EINVAL;
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


static int parse_phc_dedup(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->phc_dedup = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->phc_dedup = true;
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

static int parse_fir_filter_size(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	int tokens, size;
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%u", &size);
	if (tokens != 1)
		return EINVAL;

	if ((size < SFPTPD_FIR_FILTER_STIFFNESS_MIN) ||
	    (size > SFPTPD_FIR_FILTER_STIFFNESS_MAX)) {
		CFG_ERROR(section, "fir_filter_size %s invalid. Expect range [%d,%d]\n",
		          params[0], SFPTPD_FIR_FILTER_STIFFNESS_MIN,
			  SFPTPD_FIR_FILTER_STIFFNESS_MAX);
		return ERANGE;
	}

	general->fir_filter_size = (unsigned int)size;
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

		if (strcmp(module_name, "all") == 0)
			general->threading_trace_level = trace_level;

		if (strcmp(module_name, "general") == 0)
			general->trace_level = trace_level;
		else if (strcmp(module_name, "threading") == 0)
			general->threading_trace_level = trace_level;
		else if (strcmp(module_name, "bic") == 0)
			general->bic_trace_level = trace_level;
		else if (strcmp(module_name, "netlink") == 0)
			general->netlink_trace_level = trace_level;
		else if (strcmp(module_name, "ntp") == 0)
			general->ntp_trace_level = trace_level;
		else if (strcmp(module_name, "servo") == 0)
			general->servo_trace_level = trace_level;
		else if (strcmp(module_name, "clocks") == 0)
			general->clocks_trace_level = trace_level;
		else if (strcmp(module_name, "most") == 0 || strcmp(module_name, "all") == 0) {
			general->trace_level = trace_level;
			general->bic_trace_level = trace_level;
			general->netlink_trace_level = trace_level;
			general->ntp_trace_level = trace_level;
			general->servo_trace_level = trace_level;
			general->clocks_trace_level = trace_level;
		} else {
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
	assert(num_params == 1);

	if (strcmp(params[0], "auto") != 0 &&
	    strcmp(params[0], "netlink") != 0)
		NOTICE("only the 'netlink' hotplug detection mode is supported, "
		       "other modes are ignored; the hotplug_detection_mode "
		       "is deprecated and may be removed in a future version.\n");
	return 0;
}


static int parse_reporting_intervals(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params >= 2);
	int tokens;
	float interval;
	int i;

	if ((num_params & 1) != 0)
		return EINVAL;

	for (i = 0; i < num_params; i+= 2) {
		tokens = sscanf(params[i + 1], "%f", &interval);
		if (tokens != 1)
			return EINVAL;

		if (interval < 0.0) {
			ERROR("config [%s]: %s %s interval must be non-negative\n",
			      section->name, params[i], option);
			return ERANGE;
		}

		if (!strcmp(params[i], "save_state")) {
			general->reporting_intervals.save_state = interval;
		} else if (!strcmp(params[i], "stats_log")) {
			general->reporting_intervals.stats_log = interval;
		} else {
			ERROR("config [%s]: no such %s key: %s\n",
			      section->name, option, params[i]);
			return EINVAL;
		}
	}

	return 0;
}


static int parse_netlink_rescan_interval(struct sfptpd_config_section *section, const char *option,
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

	general->netlink_rescan_interval = interval;

	return 0;
}


static int parse_netlink_coalesce_ms(struct sfptpd_config_section *section, const char *option,
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

	general->netlink_coalesce_ms = interval;

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


static int parse_clock_display_fmts(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 4);

	if (strlen(params[0]) >= sizeof general->clocks.format_short ||
	    strlen(params[1]) >= sizeof general->clocks.format_long ||
	    strlen(params[2]) >= sizeof general->clocks.format_hwid ||
	    strlen(params[3]) >= sizeof general->clocks.format_fnam)
		return ENOSPC;

	sfptpd_strncpy(general->clocks.format_short, params[0],
		       sizeof general->clocks.format_short);
	sfptpd_strncpy(general->clocks.format_long, params[1],
		       sizeof general->clocks.format_long);
	sfptpd_strncpy(general->clocks.format_hwid, params[2],
		       sizeof general->clocks.format_hwid);
	sfptpd_strncpy(general->clocks.format_fnam, params[3],
		       sizeof general->clocks.format_fnam);

	return 0;
}


static int parse_unique_clockid_bits(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);
	union {
		uint8_t o[8];
		uint32_t pid;
		long hostid;
		uint32_t rand;
	} d;
	size_t sz;

	if (!strcmp(params[0], "pid")) {
		d.pid = htobe32(getpid());
		sz = sizeof d.pid;
	} else if (!strcmp(params[0], "hostid")) {
		d.hostid = htobe64(gethostid());
		sz = sizeof d.hostid;
	} else if (!strcmp(params[0], "rand")) {
		srand48(getpid());
		d.rand = lrand48();
		sz = sizeof d.rand;
	} else {
		sz = sscanf(params[0], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			    &d.o[0], &d.o[1], &d.o[2], &d.o[3], &d.o[4], &d.o[5], &d.o[6], &d.o[7]);
		if (sz < 1)
			return EINVAL;
	}
	assert(sz <= 8);

	memcpy(general->unique_clockid_bits + 8 - sz, &d, sz);

	return 0;
}


static int parse_legacy_clockids(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->legacy_clockids = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->legacy_clockids = true;
	} else {
		return EINVAL;
	}

	return 0;
}


static int parse_servo_log_all_samples(struct sfptpd_config_section *section, const char *option,
				       unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "off") == 0) {
		general->servo_log_all_samples = false;
	} else if (strcmp(params[0], "on") == 0) {
		general->servo_log_all_samples = true;
	} else {
		return EINVAL;
	}

	return 0;
}


/* Parse the eligible interface types.
 *
 * There may be any number of selection groups specified in this
 * configuration, separated by spaces. If preceded by a '-' then the group
 * defines interface types that will be EXCLUDED. When an interface matches
 * more than one group, the rightmost definition prevails.
 *
 * Each group may optionally be preceded by a '+' sign. If the first group
 * in the list is not prefixed by '-' or '+' then the existing list is
 * replaced, otherwise it is modified.
 *
 * Each group begins with either an IFLA_INFO_KIND string provided by
 * rtnetlink(7) (e.g. "macvlan", "team", "bond", "veth" but not, notably,
 * for Ethernet interfaces) or a family of interface types as interpreted
 * by sfptpd into an 'enum sfptpd_link_type' value, prefixed by '$', e.g.
 * '$phys' for assumed physical interfaces.
 *
 * The match is then further qualified by any number of suffixed '!attr' or
 * '@attr' terms for negative or positive constraints respectively. The
 * available attributes are:
 *   - wireless: for wireless interfaces
 *   - ether:    for Ethernet interfaces
 *   - virtual:  for virtual interfaces
 */
static int parse_eligible_interface_types(struct sfptpd_config_section *section, const char *option,
					  unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	struct sfptpd_config_interface_selection *ss = general->eligible_interface_types;
	const char *separators = "@!";
	const char *ptr;
	char *next;
	char *word;
	int rc;
	int keep = 0;
	int i;

	assert(general != NULL);

	if (num_params && (*params[0] == '+' || *params[0] == '-')) {
		/* If the first selection begins with '+' or '-' we are
		 * adding to or subtracting from the existing/default list */

		/* Count the number of selections in the existing list */
		for (; ss && (ss[keep].link_type != SFPTPD_LINK_MAX || ss[keep].link_kind); keep++);

		ss = realloc(ss, (keep + num_params + 1) * sizeof *ss);
		if (ss == NULL) {
			rc = errno;
			destroy_interface_selection(&general->eligible_interface_types);
			return rc;
		}
		memset(ss + (keep * sizeof *ss), '\0', (num_params + 1) * sizeof *ss);
	} else {
		/* Otherwise replace the existing/default list. */

		destroy_interface_selection(&general->eligible_interface_types);
		ss = calloc(num_params + 1, sizeof(struct sfptpd_config_interface_selection));
		if (ss == NULL)
			return errno;
	}

	for (i = 0; i < num_params; i++) {
		struct sfptpd_config_interface_selection *s = ss + i + keep;

		ptr = params[i];
		if (*ptr == '-') {
			s->negative = true;
			ptr++;
		} else if (*ptr == '+') {
			ptr++;
		}

		next = strpbrk(ptr, separators);
		if (!(word = next ? strndup(ptr, next - ptr) : strdup(ptr))) {
			rc = errno;
			goto fail;
		}

		if (*ptr == '$') {
			s->link_type = sfptpd_link_type_from_str(word + 1);
			free(word);
		} else {
			s->link_type = SFPTPD_LINK_MAX;
			s->link_kind = word;
		}

		/* Process appended property constraints */
		while ((ptr = next)) {
			bool exclusion;
			enum sfptpd_interface_prop prop;

			/* All properties have a prefix charater; else they
			 * would not have been picked up as properties. */
			exclusion = (*ptr++ == '!');
			next = strpbrk(ptr, separators);
			if (!(word = next ? strndup(ptr, next - ptr) : strdup(ptr))) {
				rc = errno;
				goto fail;
			}
			prop = sfptpd_interface_prop_from_str(word);
			if (prop == SFPTPD_INTERFACE_PROP_MAX) {
				rc = EINVAL;
				goto fail;
			}
			if (exclusion)
				s->props_exclude |= (1 << prop);
			else
				s->props_require |= (1 << prop);
			free(word);
		}
	}

	/* Set sentinel */
	ss[keep + num_params].link_type = SFPTPD_LINK_MAX;
	general->eligible_interface_types = ss;
	return 0;
fail:
	destroy_interface_selection(&ss);
	return rc;
}


static int parse_clock_adj_method(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[])
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "prefer-tickadj") == 0) {
		general->clocks.adj_method = SFPTPD_CLOCK_PREFER_TICKADJ;
	} else if (strcmp(params[0], "prefer-freqadj") == 0) {
		general->clocks.adj_method = SFPTPD_CLOCK_PREFER_FREQADJ;
	} else if (strcmp(params[0], "legacy") == 0) {
		general->clocks.adj_method = SFPTPD_CLOCK_LEGACY_ADJ;
	} else {
		return EINVAL;
	}

	return 0;
}


static int validate_config(struct sfptpd_config_section *general)
{
	struct sfptpd_config *config = general->config;
	struct sfptpd_config_section *section, *new;

	/* Ensure an crny sync instance is declared */
	section = sfptpd_config_find(config, SFPTPD_CRNY_MODULE_NAME);
	assert(section != NULL);
	assert(section->scope != SFPTPD_CONFIG_SCOPE_INSTANCE);
	assert(section->ops.create != NULL);

	if ((((sfptpd_config_general_t *) general)->declared_sync_modules & (1 << SFPTPD_CONFIG_CATEGORY_CRNY)) == 0) {
		new = section->ops.create(NULL, SFPTPD_CONFIG_SCOPE_INSTANCE,
					  false, section);
		if (new == NULL) {
			CFG_ERROR(general, "failed to create implicit crny instance\n");
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

static void destroy_interface_selection(struct sfptpd_config_interface_selection **selection)
{
	struct sfptpd_config_interface_selection *intf;

	for (intf = *selection;
	     intf && (intf->link_type != SFPTPD_LINK_MAX || intf->link_kind);
	     intf++)
		free(intf->link_kind);
	free(*selection);
	*selection = NULL;
}

static void general_config_destroy(struct sfptpd_config_section *section)
{
	sfptpd_config_general_t *general = (sfptpd_config_general_t *) section;

	assert(section != NULL);
	assert(section->category == SFPTPD_CONFIG_CATEGORY_GENERAL);

	destroy_interface_selection(&general->eligible_interface_types);
	free(general->groups);
	free(general->openmetrics.tcp);
	sfptpd_acl_free(&general->openmetrics.acl);
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
		sfptpd_strncpy(new->metrics_path, SFPTPD_DEFAULT_METRICS_PATH, sizeof(new->metrics_path));
		sfptpd_strncpy(new->run_dir, SFPTPD_RUN_DIR, sizeof(new->run_dir));
		new->state_dir_mode = SFPTPD_DEFAULT_STATE_DIR_MODE;
		new->run_dir_mode = SFPTPD_DEFAULT_RUN_DIR_MODE;
		new->control_socket_mode = (mode_t) -1;
		new->metrics_socket_mode = (mode_t) -1;

		new->clocks.sync_interval = SFPTPD_DEFAULT_SYNC_INTERVAL;
		new->clocks.control = SFPTPD_DEFAULT_CLOCK_CTRL;
		new->clocks.persistent_correction = SFPTPD_DEFAULT_PERSISTENT_CLOCK_CORRECTION;
		new->clocks.discipline_all = SFPTPD_DEFAULT_DISCIPLINE_ALL_CLOCKS;
		new->clocks.num_clocks = 0;
		new->clocks.no_initial_correction = false;
		new->clocks.observe_readonly = SFPTPD_DEFAULT_OBSERVE_READONLY_CLOCKS;
		new->clocks.adj_method = SFPTPD_DEFAULT_CLOCK_ADJ_METHOD;
		new->epoch_guard = SFPTPD_DEFAULT_EPOCH_GUARD;
		new->initial_clock_correction = SFPTPD_DEFAULT_INITIAL_CLOCK_CORRECTION;

		new->non_sfc_nics = SFPTPD_DEFAULT_NON_SFC_NICS;
		new->assume_one_phc_per_nic = SFPTPD_DEFAULT_ASSUME_ONE_PHC_PER_NIC;
		new->phc_dedup = SFPTPD_DEFAULT_PHC_DEDUP;
		new->test_mode = false;
		new->daemon = false;
		new->lock = true;
		new->rtc_adjust = SFPTPD_DEFAULT_RTC_ADJUST;
		new->openmetrics.unix = SFPTPD_DEFAULT_OPENMETRICS_UNIX;
		new->openmetrics.rt_stats_buf = SFPTPD_DEFAULT_OPENMETRICS_RT_STATS_BUF;
		new->openmetrics.acl.order = SFPTPD_ACL_ALLOW_ALL;
		sfptpd_strncpy(new->openmetrics.family_prefix, SFPTPD_DEFAULT_OPENMETRICS_PREFIX, sizeof(new->openmetrics.family_prefix));

		new->timestamping.all = false;
		new->timestamping.disable_on_exit = SFPTPD_DEFAULT_DISABLE_ON_EXIT;
		new->timestamping.num_interfaces = 0;

		new->convergence_threshold = 0.0;
		new->step_threshold = SFPTPD_DEFAULT_STEP_THRESHOLD_NS;
		new->initial_sync_instance[0] = '\0';
		new->selection_holdoff_interval = SFPTPD_DEFAULT_SELECTION_HOLDOFF_INTERVAL;
		new->netlink_rescan_interval = SFPTPD_DEFAULT_NETLINK_RESCAN_INTERVAL;
		new->netlink_coalesce_ms = SFPTPD_DEFAULT_NETLINK_COALESCE_MS;

		new->pid_filter.kp = SFPTPD_DEFAULT_SERVO_K_PROPORTIONAL;
		new->pid_filter.ki = SFPTPD_DEFAULT_SERVO_K_INTEGRAL;
		new->fir_filter_size = 0; /* 0 indicates to use default computed size */

		new->selection_policy = sfptpd_default_selection_policy;
                memcpy(new->phc_diff_methods, sfptpd_default_phc_diff_methods, sizeof(sfptpd_default_phc_diff_methods));
		assert(sizeof new->phc_pps_method == sizeof sfptpd_default_pps_method);
		memcpy(new->phc_pps_method, sfptpd_default_pps_method, sizeof new->phc_pps_method);

		new->json_stats_filename[0] = '\0';
		new->json_remote_monitor_filename[0] = '\0';

		new->clustering_mode = SFPTPD_DEFAULT_CLUSTERING_MODE;
                new->clustering_guard_enabled = SFPTPD_DEFAULT_CLUSTERING_GUARD;
		new->clustering_guard_threshold = SFPTPD_DEFAULT_CLUSTERING_GUARD_THRESHOLD;
		new->clustering_discriminator_name[0] = '\0';
		new->clustering_discriminator_threshold = 0;
		new->clustering_score_without_discriminator = SFPTPD_DEFAULT_CLUSTERING_SCORE_ABSENT_DISCRIM;

		new->limit_freq_adj = 1.0E9;

		new->reporting_intervals.save_state = SFPTPD_DEFAULT_STATE_SAVE_INTERVAL;
		new->reporting_intervals.stats_log = SFPTPD_DEFAULT_STATISTICS_LOGGING_INTERVAL;

		sfptpd_strncpy(new->clocks.format_short, SFPTPD_DEFAULT_CLOCK_SHORT_FMT, sizeof new->clocks.format_short);
		sfptpd_strncpy(new->clocks.format_long, SFPTPD_DEFAULT_CLOCK_LONG_FMT, sizeof new->clocks.format_long);
		sfptpd_strncpy(new->clocks.format_hwid, SFPTPD_DEFAULT_CLOCK_HWID_FMT, sizeof new->clocks.format_hwid);
		sfptpd_strncpy(new->clocks.format_fnam, SFPTPD_DEFAULT_CLOCK_FNAM_FMT, sizeof new->clocks.format_fnam);
		new->legacy_clockids = false;
		new->declared_sync_modules = 0;

		new->servo_log_all_samples = SFPTPD_DEFAULT_SERVO_LOG_ALL_SAMPLES;

		{
			char *default_str = strdup(SFPTPD_DEFAULT_PHYSICAL_INTERFACES);
			char *tokens[SFPTPD_CONFIG_TOKENS_MAX];
			int num_tokens;
			int rc;

			num_tokens = tokenize(default_str, SFPTPD_CONFIG_TOKENS_MAX, tokens);
			rc = parse_eligible_interface_types((struct sfptpd_config_section *) new,
							    NULL, num_tokens, (const char *const *)tokens);
			assert(rc == 0);
			free(default_str);
		}
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


void sfptpd_config_set_priv_helper(struct sfptpd_config *config,
				   char *path)
{
	struct sfptpd_config_general *general = sfptpd_general_config_get(config);

	/* Take a copy of the priv helper path */
	sfptpd_strncpy(general->priv_helper_path,
		       path ? path : SFPTPD_DEFAULT_PRIV_HELPER_PATH,
		       sizeof(general->priv_helper_path));
	TRACE_L4("using privileged helper %s\n", general->priv_helper_path);
}


void sfptpd_config_general_set_console_logging(struct sfptpd_config *config)
{
	struct sfptpd_config_general *general = sfptpd_general_config_get(config);

	general->message_log = SFPTPD_MSG_LOG_TO_STDERR;
	general->stats_log = SFPTPD_STATS_LOG_TO_STDOUT;
}


void sfptpd_config_general_set_verbose(struct sfptpd_config *config,
				       int verbosity)
{
	struct sfptpd_config_general *general = sfptpd_general_config_get(config);

	sfptpd_config_general_set_console_logging(config);

	if (verbosity >= 1) {
		if (general->trace_level < 3)
			general->trace_level = 3;
		if (general->netlink_trace_level < 1)
			general->netlink_trace_level = 1;
		if (general->ntp_trace_level < 1)
			general->ntp_trace_level = 1;
		if (general->clocks_trace_level < 2)
			general->clocks_trace_level = 2;
	}

	if (verbosity >= 2) {
		general->trace_level = 6;
		general->netlink_trace_level = 6;
		general->ntp_trace_level = 6;
		general->clocks_trace_level = 6;
		general->servo_trace_level = 6;
		general->bic_trace_level = 6;
		if (general->threading_trace_level < 1)
			general->servo_trace_level = 1;
	}

	if (verbosity >= 3) {
		general->threading_trace_level = 6;
	}

	sfptpd_log_set_trace_level(SFPTPD_COMPONENT_ID_SFPTPD,
				   general->trace_level);
}


int sfptpd_config_general_set_user(struct sfptpd_config *config,
				   const char *user, const char *group)
{
	struct sfptpd_config_section *general = (struct sfptpd_config_section *) sfptpd_general_config_get(config);
	const char *options[2] = { user, group };

	assert(user);

	return parse_user(general, "user", group ? 2 : 1, options);
}

void sfptpd_config_general_set_daemon(struct sfptpd_config *config,
				     bool daemon)
{
	struct sfptpd_config_general *general = sfptpd_general_config_get(config);

	if (general->daemon && !daemon)
		WARNING("overriding 'daemon' from config file with '--no-daemon' from command line\n");

	general->daemon = daemon;
}


/* fin */
