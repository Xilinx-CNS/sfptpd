/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2025 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_GENERAL_CONFIG_H
#define _SFPTPD_GENERAL_CONFIG_H

#include <stdio.h>
#include <net/if.h>
#include <limits.h>

#include <sfptpd_config.h>
#include <sfptpd_logging.h>
#include "sfptpd_bic.h"
#include "sfptpd_phc.h"
#include "sfptpd_metrics.h"


/****************************************************************************
 * General Configuration
 ****************************************************************************/

/** Default configuration values */
#define SFPTPD_DEFAULT_MESSAGE_LOG                 (SFPTPD_MSG_LOG_TO_STDERR)
#define SFPTPD_DEFAULT_STATS_LOG                   (SFPTPD_STATS_LOG_OFF)
#define SFPTPD_DEFAULT_STATE_PATH                  SFPTPD_STATE_PATH
#define SFPTPD_DEFAULT_CONTROL_PATH                SFPTPD_CONTROL_SOCKET_PATH
#define SFPTPD_DEFAULT_METRICS_PATH                SFPTPD_METRICS_SOCKET_PATH
#define SFPTPD_DEFAULT_RUN_DIR                     SFPTPD_RUN_DIR
#define SFPTPD_DEFAULT_RUN_DIR_MODE		   0770
#define SFPTPD_DEFAULT_STATE_DIR_MODE		   0777
#define SFPTPD_DEFAULT_TRACE_LEVEL                 0
#define SFPTPD_DEFAULT_SYNC_INTERVAL               -4
#define SFPTPD_DEFAULT_CLOCK_CTRL                  (SFPTPD_CLOCK_CTRL_SLEW_AND_STEP)
#define SFPTPD_DEFAULT_STEP_THRESHOLD_NS           (SFPTPD_SERVO_CLOCK_STEP_THRESHOLD_S * ONE_BILLION)
#define SFPTPD_DEFAULT_EPOCH_GUARD                 (SFPTPD_EPOCH_GUARD_CORRECT_CLOCK)
#define SFPTPD_DEFAULT_INITIAL_CLOCK_CORRECTION    (SFPTPD_CLOCK_INITIAL_CORRECTION_ALWAYS)
#define SFPTPD_DEFAULT_CLUSTERING_MODE             (SFPTPD_CLUSTERING_DISABLED)
#define SFPTPD_DEFAULT_CLUSTERING_SCORE_ABSENT_DISCRIM 1
#define SFPTPD_DEFAULT_CLUSTERING_GUARD            (false)
#define SFPTPD_DEFAULT_CLUSTERING_GUARD_THRESHOLD  1
#define SFPTPD_DEFAULT_PERSISTENT_CLOCK_CORRECTION (true)
#define SFPTPD_DEFAULT_DISABLE_ON_EXIT             (true)
#define SFPTPD_DEFAULT_DISCIPLINE_ALL_CLOCKS       (true)
#define SFPTPD_DEFAULT_NON_SFC_NICS                (false)
#define SFPTPD_DEFAULT_ASSUME_ONE_PHC_PER_NIC      (false)
#define SFPTPD_DEFAULT_PHC_DEDUP                   (false)
#define SFPTPD_DEFAULT_TEST_MODE                   (false)
#define SFPTPD_DEFAULT_RTC_ADJUST                  (true)
#define SFPTPD_DEFAULT_OPENMETRICS_UNIX            (true)
#define SFPTPD_DEFAULT_OPENMETRICS_RT_STATS_BUF    256
#define SFPTPD_DEFAULT_OPENMETRICS_FLAGS           0
#define SFPTPD_DEFAULT_OPENMETRICS_PREFIX          ""
#define SFPTPD_DEFAULT_SELECTION_HOLDOFF_INTERVAL  10
#define SFPTPD_DEFAULT_NETLINK_RESCAN_INTERVAL     31
#define SFPTPD_DEFAULT_NETLINK_COALESCE_MS         50
#define SFPTPD_DEFAULT_SERVO_K_PROPORTIONAL        0.4
#define SFPTPD_DEFAULT_SERVO_K_INTEGRAL            0.03
#define SFPTPD_DEFAULT_SERVO_K_DIFFERENTIAL        0.0
#define SFPTPD_DEFAULT_CLOCK_SHORT_FMT		   "phc%P"
#define SFPTPD_DEFAULT_CLOCK_LONG_FMT		   "phc%P(%I)"
#define SFPTPD_DEFAULT_CLOCK_HWID_FMT		   "%C:"
#define SFPTPD_DEFAULT_CLOCK_FNAM_FMT		   "%C:"
#define SFPTPD_DEFAULT_UNIQUE_CLOCKID_BITS         "00:00"

/** Statistics logging interval in seconds */
#define SFPTPD_DEFAULT_STATISTICS_LOGGING_INTERVAL 1

/** State save interval in seconds */
#define SFPTPD_DEFAULT_STATE_SAVE_INTERVAL 60

/** Maximum size of MAC address string */
#define SFPTPD_CONFIG_MAC_STRING_MAX   (24)

/** Maximum size of format string */
#define SFPTPD_CONFIG_FMT_STR_MAX      (16)

/** Sync interval minimum and maximum values */
#define SFPTPD_MAX_SYNC_INTERVAL       3
#define SFPTPD_MIN_SYNC_INTERVAL       -5

/** Message logging options */
enum sfptpd_msg_log_config {
	SFPTPD_MSG_LOG_TO_SYSLOG,
	SFPTPD_MSG_LOG_TO_STDERR,
	SFPTPD_MSG_LOG_TO_FILE
};

/** Stats logging options */
enum sfptpd_stats_log_config {
	SFPTPD_STATS_LOG_OFF,
	SFPTPD_STATS_LOG_TO_STDOUT,
	SFPTPD_STATS_LOG_TO_FILE
};

/** Clock control options */
enum sfptpd_clock_ctrl {
	SFPTPD_CLOCK_CTRL_SLEW_AND_STEP,
	SFPTPD_CLOCK_CTRL_STEP_AT_STARTUP,
	SFPTPD_CLOCK_CTRL_NO_STEP,
	SFPTPD_CLOCK_CTRL_NO_ADJUST,
        SFPTPD_CLOCK_CTRL_STEP_FORWARD,
	SFPTPD_CLOCK_CTRL_STEP_ON_FIRST_LOCK
};

/** Epoch guard options */
enum sfptpd_epoch_guard_config {
	SFPTPD_EPOCH_GUARD_ALARM_ONLY,
	SFPTPD_EPOCH_GUARD_PREVENT_SYNC,
	SFPTPD_EPOCH_GUARD_CORRECT_CLOCK
};

/** Initial clock correction options */
enum sfptpd_clock_initial_correction {
	SFPTPD_CLOCK_INITIAL_CORRECTION_ALWAYS,
	SFPTPD_CLOCK_INITIAL_CORRECTION_IF_UNSET,
};

enum clock_config_state {
	CLOCK_OPTION_NOT_APPLIED = 0,
	CLOCK_OPTION_APPLIED,
	CLOCK_OPTION_ALREADY_APPLIED,
};

/* Critical errors that can be set not to terminate execution */
enum sfptpd_critical_error {
	SFPTPD_CRITICAL_NO_PTP_CLOCK = 0,
	SFPTPD_CRITICAL_NO_PTP_SUBSYSTEM,
	SFPTPD_CRITICAL_CLOCK_CONTROL_CONFLICT,
	SFPTPD_CRITICAL_MAX,
};


/** struct sfptpd_config_clocks - sfptpd clock configuration
 * @sync_interval: Interval in 2^number seconds at which the clocks are
 * synchronized
 * @control: Limits how the clocks can be adjusted
 * @persistent_correction: Indicates whether saved clock corrections are
 * used when disciplining clocks.
 * @discipline_all: discipline all clocks
 * @num_clocks: number of clocks to discipline
 * @clocks: array of clocks to discipline
 */
typedef struct sfptpd_config_clocks {
	int sync_interval;
	enum sfptpd_clock_ctrl control;
	bool persistent_correction;
	bool no_initial_correction;
	bool discipline_all;
	unsigned int num_clocks;
	char clocks[SFPTPD_CONFIG_TOKENS_MAX][SFPTPD_CONFIG_MAC_STRING_MAX];
	unsigned int num_readonly_clocks;
	char readonly_clocks[SFPTPD_CONFIG_TOKENS_MAX][SFPTPD_CONFIG_MAC_STRING_MAX];
	char format_short[SFPTPD_CONFIG_FMT_STR_MAX];
	char format_long[SFPTPD_CONFIG_FMT_STR_MAX];
	char format_hwid[SFPTPD_CONFIG_FMT_STR_MAX];
	char format_fnam[SFPTPD_CONFIG_FMT_STR_MAX];

	/* Mutable state to keep track of whether the config options have been applied.
           CLOCK_OPTION_NOT_APPLIED (0) means not applied because the clock couldn't be found,
           CLOCK_OPTION_APPLIED means applied,
           CLOCK_OPTION_ALREADY_APPLIED means redundant (clock has already been made readonly/disciplined).
        */
	enum clock_config_state readonly_clocks_applied[SFPTPD_CONFIG_TOKENS_MAX];
	enum clock_config_state clock_list_applied[SFPTPD_CONFIG_TOKENS_MAX];
} sfptpd_config_clocks_t;

/** struct sfptpd_config_timestamping - sfptpd timestamping configuration
 * @all: enable timestamping on all interfaces that support it
 * @disable_on_exit: disable timestamping on exit
 * @num_interfaces: number of interfaces
 * @interfaces: Array of interfaces for which timestamping required
 */
typedef struct sfptpd_config_timestamping {
	bool all;
	bool disable_on_exit;
	unsigned int num_interfaces;
	char interfaces[SFPTPD_CONFIG_TOKENS_MAX][SFPTPD_CONFIG_MAC_STRING_MAX];
} sfptpd_config_timestamping_t;

/** Openmetrics config
 * @unix: whether to listen on AF_UNIX socket for OpenMetrics queries
 * @rt_stats_buf: number of entries to store in RT stats circular buffer
 * @flags: flags for OpenMetrics features
 * @family_prefix: prefix string for OpenMetrics families
 */
struct sfptpd_config_metrics {
	bool unix;
	unsigned int rt_stats_buf;
	sfptpd_metrics_flags_t flags;
	char family_prefix[32];
};

/** struct sfptpd_config_general - sfptpd general configuration
 * @hdr: Configuration section common header
 * @config_filename: Path of configuration file
 * @priv_helper_path: Path to privileged helper
 * @message_log: Target for logged messages
 * @message_log_filename: Path of log file for message logging
 * @stats_log: Target for logged statistics
 * @stats_log_filename: Path of log file for statistics logging
 * @trace_level: Debug trace level
 * @clocks: Clock configuration
 * @non_sfc_nics: Use non-Solarflare adapters
 * @test_mode: Indicates features to facilitate testing are enabled
 * @daemon: Run as a daemon
 * @lock: Use a lock file to lock access to the clocks
 * @rtc_adjust: Allow kernel to update hardware RTC when sys clock in sync
 * @timestamping: Timestamping configuration
 * @convergence_threshold: Convergence threshold in ns
 * @step_threshold: Step threshold in ns
 * @selection_policy: Sync instance selection rules & strategy
 * @pps_methods: PHC PPS methods
 * @initial_sync_instance: When selecting instances manually, the name of the initial sync instance
 * @selection_holdoff_interval: Interval to wait after detecting a better instance
 * before selecting it
 * @reporting_intervals: Intervals between reporting from engine
 * @netlink_rescan_interval: Interval between rescanning interface with netlink
 * @pid_filter.kp: Secondary servo PID filter proportional term coefficient
 * @pid_filter.ki: Secondary servo PID filter integral term coefficient
 * @initial_clock_correction: When to apply an initial clock correction
 */
typedef struct sfptpd_config_general {
	sfptpd_config_section_t hdr;
	char config_filename[PATH_MAX];
	char priv_helper_path[PATH_MAX];
	enum sfptpd_msg_log_config message_log;
	char message_log_filename[PATH_MAX];
	enum sfptpd_stats_log_config stats_log;
	char stats_log_filename[PATH_MAX];
	unsigned int trace_level;
	unsigned int threading_trace_level;
	unsigned int bic_trace_level;
	unsigned int netlink_trace_level;
	unsigned int ntp_trace_level;
	unsigned int servo_trace_level;
	unsigned int clocks_trace_level;
	sfptpd_config_clocks_t clocks;
	bool non_sfc_nics;
	bool assume_one_phc_per_nic;
	bool phc_dedup;
	bool avoid_efx;
	bool test_mode;
	bool daemon;
	bool lock;
	bool rtc_adjust;
	uid_t uid;
	gid_t gid;
	gid_t *groups;
	int num_groups;
	char state_path[PATH_MAX];
	char control_path[PATH_MAX];
	char metrics_path[PATH_MAX];
	mode_t run_dir_mode;
	mode_t state_dir_mode;
	sfptpd_config_timestamping_t timestamping;
	long double convergence_threshold;
	long double step_threshold;
	char initial_sync_instance[SFPTPD_CONFIG_SECTION_NAME_MAX];
	unsigned int selection_holdoff_interval;
	struct {
		float save_state;
		float stats_log;
	} reporting_intervals;
	unsigned int netlink_rescan_interval;
	unsigned int netlink_coalesce_ms;
	struct {
		long double kp;
		long double ki;
	} pid_filter;
	struct sfptpd_selection_policy selection_policy;
	sfptpd_phc_pps_method_t phc_pps_method[SFPTPD_PPS_METHOD_MAX + 1];
	char json_stats_filename[PATH_MAX];
	char json_remote_monitor_filename[PATH_MAX];
	enum sfptpd_epoch_guard_config epoch_guard;
	enum sfptpd_clock_initial_correction initial_clock_correction;
	enum sfptpd_clustering_mode clustering_mode;
	enum sfptpd_phc_diff_method phc_diff_methods[SFPTPD_DIFF_METHOD_MAX+1];
	char clustering_discriminator_name[SFPTPD_CONFIG_SECTION_NAME_MAX];
	long double clustering_discriminator_threshold;
        bool clustering_guard_enabled;
	int clustering_guard_threshold;
	int clustering_score_without_discriminator;
	long double limit_freq_adj;
	bool ignore_critical[SFPTPD_CRITICAL_MAX];
	unsigned long declared_sync_modules;
	uint8_t unique_clockid_bits[8];
	bool legacy_clockids;
	char run_dir[PATH_MAX];
	struct sfptpd_config_metrics openmetrics;
} sfptpd_config_general_t;

static_assert(sizeof ((sfptpd_config_general_t *) 0)->declared_sync_modules * 8 >= SFPTPD_CONFIG_CATEGORY_MAX,
	      "bitfield supports number of sync modules");


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create and initialise the general module configuration options. This will
 * create the general configuration and set the values to defaults. The
 * function also registers the general config options.
 * @param config  Pointer to the configuration structure
 * @return 0 on success or an errno otherwise
 */
int sfptpd_general_config_init(struct sfptpd_config *config);

/** Get the general configuration
 * @param config  Pointer to configuration
 * @return  A pointer to the general configuration
 */
struct sfptpd_config_general *sfptpd_general_config_get(struct sfptpd_config *config);

/** Set the configuration filename
 * @param config  Pointer to configuration
 * @param filename  Config filename
 */
void sfptpd_config_set_config_file(struct sfptpd_config *config,
				   char *filename);

/** Set the path to the privileged helper
 * @param config  Pointer to configuration
 * @param path  Path
 */
void sfptpd_config_set_priv_helper(struct sfptpd_config *config,
				   char *path);

/** Direct all output to the console.
 * @param config  Pointer to configuration
 */
void sfptpd_config_general_set_console_logging(struct sfptpd_config *config);

/** Enable verbose logging and direct all output to the console.
 * @param verbosity Amount of general verbosity to add.
 * @param config  Pointer to configuration
 */
void sfptpd_config_general_set_verbose(struct sfptpd_config *config,
				       int verbosity);

/** Specify a user and group to run sfptpd as.
 * @param user  User to run as
 * @param group  Group to run as or run as user's principle group if NULL.
 * @return  An error code or 0 on success
 */
int sfptpd_config_general_set_user(struct sfptpd_config *config,
				   const char *user, const char *group);

/** Override daemon setting.
 * @param config  Pointer to configuration
 * @param daemon  Whether to run as a daemon
 */
void sfptpd_config_general_set_daemon(struct sfptpd_config *config,
				      bool daemon);

#endif /* _SFPTPD_GENERAL_CONFIG_H */
