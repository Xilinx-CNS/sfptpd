/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_pps_module.c
 * @brief  PPS Synchronization Module
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <poll.h>

#include "sfptpd_app.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_pps_module.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_thread.h"
#include "sfptpd_clock.h"
#include "sfptpd_interface.h"
#include "sfptpd_statistics.h"
#include "sfptpd_engine.h"
#include "sfptpd_time.h"
#include "sfptpd_filter.h"
#include "sfptpd_multicast.h"


/****************************************************************************
 * Types
 ****************************************************************************/

#define PPS_POLL_TIMER_ID (0)
#define PPS_POLL_INTERVAL_NS (250000000)

#define PPS_NOTCH_FILTER_MID_POINT (1.0e9)
#define PPS_NOTCH_FILTER_WIDTH (1.0e8)

#define PPS_REQUIRED_GOOD_PERIODS (3)

#define PPS_CLOCK_STEP_THRESHOLD (500000000.0)

enum pps_stats_ids {
	PPS_STATS_ID_OFFSET,
	PPS_STATS_ID_PERIOD,
	PPS_STATS_ID_FREQ_ADJ,
	PPS_STATS_ID_SYNCHRONIZED,
	PPS_STATS_ID_CLOCK_STEPS,
	PPS_STATS_ID_NO_SIGNAL_ERRORS,
	PPS_STATS_ID_SEQ_NUM_ERRORS,
	PPS_STATS_ID_TIME_OF_DAY_ERRORS,
	PPS_STATS_ID_BAD_SIGNAL_ERRORS,
	PPS_STATS_ID_OUTLIERS
};

struct sfptpd_pps_instance;

typedef struct sfptpd_pps_module {
	/* Pointer to sync-engine */
	struct sfptpd_engine *engine;

	/* Linked list of instances */
	struct sfptpd_pps_instance *instances;

	/* Time of day provided by third party source sync module e.g. NTP */
	struct {
		/* Handle of sync module */
		struct sfptpd_sync_instance_info source;

		/* Next poll time */
		struct sfptpd_timespec next_poll_time;

		/* State of the sync module */
		sfptpd_sync_instance_status_t status;
	} time_of_day;

	bool timers_started;
} pps_module_t;


struct sfptpd_pps_instance {
	/* Pointer to the pps configuration */
	struct sfptpd_pps_module_config *config;

	/* Handle of the local reference clock */
	struct sfptpd_clock *clock;

	/* Which elements of the PPS instance are enabled */
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;

	/* Maximum frequency adjustment supported by slave clock */
	long double freq_adjust_max;

	/* PPS module state */
	sfptpd_sync_module_state_t state;

	/* PPS alarms */
	sfptpd_sync_module_alarms_t alarms;

	/* Monotonic time of last PPS event */
	struct sfptpd_timespec last_pps_time;

	/* Monotonic time of PPS module start */
	struct sfptpd_timespec instance_started_time;
	bool instance_has_started;
	bool pps_pulse_check_timer_expired;

	/* Time reported in PPS event */
	struct sfptpd_timespec pps_timestamp;

	/* PPS event sequence number */
	uint32_t pps_seq_num;

	/* fd to poll */
	int poll_fd;

	/* Time of day offset */
	struct sfptpd_timespec tod_offset;

	/* Notch filter used to detect bad PPS periods */
	sfptpd_notch_filter_t notch_filter;

	/* Peirce filter used to detect and reject outliers */
	struct sfptpd_peirce_filter *outlier_filter;

	/* FIR filter used to filter the raw PPS data */
	sfptpd_fir_filter_t fir_filter;

	/* PID filter used to calculate the frequency corrections */
	sfptpd_pid_filter_t pid_filter;

	/* Convergence measure */
	struct sfptpd_stats_convergence convergence;

	/* Calculated offset from master in ns */
	long double offset_from_master_ns;

	/* Base frequency correction, this is loaded from the freq-correction file,
	 * and is used as the zero-point for the PID controller. Doing this allows
	 * us to converge quicker. */
	long double freq_adjust_base;

	/* Calculated frequency adjustment in parts-per-billion. */
	long double freq_adjust_ppb;

	/* Boolean indicating that servo synchronize operation has been
	 * executed at least once. Used to limit clock stepping to first
	 * update if required. */
	bool servo_active;

	/* Calculated PPS period */
	long double pps_period_ns;

	/* Boolean indicating whether we consider the slave clock to be
	 * synchonized to the master */
	bool synchronized;

	/* PPS module previous state */
	sfptpd_sync_module_state_t prev_state;

	/* PPS alarms previous state */
	uint32_t prev_alarms;

	/* Shared stats data - accessed by thread and engine contexts */
	struct sfptpd_stats_collection stats;

	/* Count of consecutive good PPS periods */
	uint64_t consecutive_good_periods;

	/* Clustering evaluator */
	struct sfptpd_clustering_evaluator clustering_evaluator;

	/* Clustering score */
	int clustering_score;

	/* Previous clustering score */
	int prev_clustering_score;

	/* Counters to facilitate long term stats collection */
	struct {
		/* Count of number of clock steps */
		unsigned int clock_steps;

		/* Count of sequence number errors */
		unsigned int seq_num_errors;

		/* Count of number of bad PPS periods */
		unsigned int bad_signal_errors;

		/* Count of number of outliers detected/rejected */
		unsigned int outliers;
	} counters;

	/* Data associated with test modes */
	struct {
		/* Bogus PPS event generation enabled */
		bool bogus_pps_events;
	} test;

	/* Pointer to next instance in linked list */
	struct sfptpd_pps_instance *next;
};


/****************************************************************************
 * Constants
 ****************************************************************************/

static const struct sfptpd_stats_collection_defn pps_stats_defns[] =
{
	{PPS_STATS_ID_OFFSET,             SFPTPD_STATS_TYPE_RANGE, "offset-from-master", "ns", 3},
	{PPS_STATS_ID_PERIOD,             SFPTPD_STATS_TYPE_RANGE, "pps-period", "ns", 0},
	{PPS_STATS_ID_FREQ_ADJ,           SFPTPD_STATS_TYPE_RANGE, "freq-adjustment", "ppb", 3},
	{PPS_STATS_ID_SYNCHRONIZED,       SFPTPD_STATS_TYPE_COUNT, "synchronized"},
	{PPS_STATS_ID_CLOCK_STEPS,        SFPTPD_STATS_TYPE_COUNT, "clock-steps"},
	{PPS_STATS_ID_SEQ_NUM_ERRORS,     SFPTPD_STATS_TYPE_COUNT, "sequence-number-errors"},
	{PPS_STATS_ID_NO_SIGNAL_ERRORS,   SFPTPD_STATS_TYPE_COUNT, "no-pps-signal-errors"},
	{PPS_STATS_ID_TIME_OF_DAY_ERRORS, SFPTPD_STATS_TYPE_COUNT, "time-of-day-errors"},
	{PPS_STATS_ID_BAD_SIGNAL_ERRORS,  SFPTPD_STATS_TYPE_COUNT, "bad-pps-signal-errors"},
	{PPS_STATS_ID_OUTLIERS,           SFPTPD_STATS_TYPE_COUNT, "outliers-rejected"}
};

static const struct sfptpd_timespec pps_timeout_interval = {60, 0};
static const struct sfptpd_timespec pps_pulse_timeout_interval = {8, 0};
static const struct sfptpd_timespec pps_alarm_interval = {1, 100000000};


/****************************************************************************
 * Function prototypes
 ****************************************************************************/



/****************************************************************************
 * Configuration
 ****************************************************************************/

static int parse_interface(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	assert(num_params == 1);

	sfptpd_strncpy(pps->interface_name, params[0], sizeof(pps->interface_name));

	return 0;
}

static int parse_pps_delay(struct sfptpd_config_section *section, const char *option,
			   unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	assert(num_params == 1);
	int tokens;

	tokens = sscanf(params[0], "%Lf", &(pps->propagation_delay));
	if (tokens != 1)
		return EINVAL;

	return 0;
}


static int parse_priority(struct sfptpd_config_section *section, const char *option,
			  unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	int tokens, priority;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%u", &priority);
	if (tokens != 1)
		return EINVAL;

	pps->priority = (unsigned int)priority;
	return 0;
}


static int parse_sync_threshold(struct sfptpd_config_section *section, const char *option,
				unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	int tokens;
	long double threshold;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%Lf", &threshold);
	if (tokens != 1)
		return EINVAL;

	pps->convergence_threshold = threshold;
	return 0;
}


static int parse_time_of_day(struct sfptpd_config_section *section, const char *option,
			     unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	assert(num_params == 1);

	if (strlen(params[0]) >= SFPTPD_CONFIG_SECTION_NAME_MAX) {
		CFG_ERROR(section, "instance name %s too long\n",
		          params[0]);
		return ERANGE;
	}

	strcpy(pps->tod_name, params[0]);
	return 0;
}


static int parse_master_clock_class(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "locked") == 0) {
		pps->master_clock_class = SFPTPD_CLOCK_CLASS_LOCKED;
	} else if (strcmp(params[0], "holdover") == 0) {
		pps->master_clock_class = SFPTPD_CLOCK_CLASS_HOLDOVER;
	} else if (strcmp(params[0], "freerunning") == 0) {
		pps->master_clock_class = SFPTPD_CLOCK_CLASS_FREERUNNING;
	} else {
		rc = EINVAL;
	}

	return rc;
}


static int parse_master_time_source(struct sfptpd_config_section *section, const char *option,
				    unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "atomic") == 0) {
		pps->master_time_source = SFPTPD_TIME_SOURCE_ATOMIC_CLOCK;
	} else if (strcmp(params[0], "gps") == 0) {
		pps->master_time_source = SFPTPD_TIME_SOURCE_GPS;
	} else if (strcmp(params[0], "ptp") == 0) {
		pps->master_time_source = SFPTPD_TIME_SOURCE_PTP;
	} else if (strcmp(params[0], "ntp") == 0) {
		pps->master_time_source = SFPTPD_TIME_SOURCE_NTP;
	} else if (strcmp(params[0], "oscillator") == 0) {
		pps->master_time_source = SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;
	} else {
		rc = EINVAL;
	}

	return rc;
}


static int parse_master_accuracy(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	int tokens;
	assert(num_params == 1);

	if (strcmp(params[0], "unknown") == 0) {
		pps->master_accuracy = INFINITY;
		return 0;
	}

	tokens = sscanf(params[0], "%Lf", &(pps->master_accuracy));
	if (tokens != 1)
		return EINVAL;

	return 0;
}


static int parse_master_traceability(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	int rc = 0;
	int param;
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;

	pps->master_time_traceable = false;
	pps->master_freq_traceable = false;

	for (param = 0; param < num_params; param++) {
		if (strcmp(params[param], "time") == 0) {
			pps->master_time_traceable = true;
		} else if (strcmp(params[param], "freq") == 0) {
			pps->master_freq_traceable = true;
		} else {
			rc = EINVAL;
		}
	}

	return rc;
}


static int parse_steps_removed(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	int tokens, steps_removed;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%u", &steps_removed);
	if (tokens != 1)
		return EINVAL;

	pps->steps_removed = (unsigned int)steps_removed;
	return 0;
}


static int parse_pid_filter_kp(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
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

	pps->pid_filter.kp = kp;
	return 0;
}


static int parse_pid_filter_ki(struct sfptpd_config_section *section, const char *option,
			       unsigned int num_params, const char * const params[])
{
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
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

	pps->pid_filter.ki = ki;
	return 0;
}


static int parse_outlier_filter_type(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	int rc = 0;
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	assert(num_params == 1);

	if (strcmp(params[0], "disabled") == 0) {
		pps->outlier_filter.enabled = false;
	} else if (strcmp(params[0], "std-dev") == 0) {
		pps->outlier_filter.enabled = true;
	} else {
		rc = EINVAL;
	}

	return rc;
}


static int parse_outlier_filter_size(struct sfptpd_config_section *section, const char *option,
				     unsigned int num_params, const char * const params[])
{
	int tokens, size;
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	assert(num_params == 1);

	tokens = sscanf(params[0], "%u", &size);
	if (tokens != 1)
		return EINVAL;

	if ((size < SFPTPD_PEIRCE_FILTER_SAMPLES_MIN) ||
	    (size > SFPTPD_PEIRCE_FILTER_SAMPLES_MAX)) {
		CFG_ERROR(section, "outlier_filter_size %s invalid. Expect range [%d,%d]\n",
		          params[0], SFPTPD_PEIRCE_FILTER_SAMPLES_MIN,
			  SFPTPD_PEIRCE_FILTER_SAMPLES_MAX);
		return ERANGE;
	}

	pps->outlier_filter.size = (unsigned int)size;
	return 0;
}


static int parse_outlier_adaption(struct sfptpd_config_section *section, const char *option,
				  unsigned int num_params, const char * const params[])
{
	int tokens;
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
	assert(pps != NULL);

	tokens = sscanf(params[0], "%Lf", &(pps->outlier_filter.adaption));
	if (tokens != 1)
		return EINVAL;

	if ((pps->outlier_filter.adaption < 0.0) ||
	    (pps->outlier_filter.adaption > 1.0)) {
		CFG_ERROR(section, "outlier_filter_adaption %s invalid. Expect range [0,1]\n",
		          params[0]);
		return ERANGE;
	}

	return 0;
}


static int parse_fir_filter_size(struct sfptpd_config_section *section, const char *option,
				 unsigned int num_params, const char * const params[])
{
	int tokens, size;
	sfptpd_pps_module_config_t *pps = (sfptpd_pps_module_config_t *)section;
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

	pps->fir_filter_size = (unsigned int)size;
	return 0;
}




static const sfptpd_config_option_t pps_config_options[] =
{
	{"interface", "interface-name",
		"Specifies the name of the interface that PPS should use",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_interface},
	{"priority", "<NUMBER>",
		"Relative priority of sync module instance. Smaller values have higher "
		"priority. The default " STRINGIFY(SFPTPD_DEFAULT_PRIORITY) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_priority},
	{"sync_threshold", "<NUMBER>",
		"Threshold in nanoseconds of the offset from the clock source over a "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT)
		"s period to be considered in sync (converged). The default is "
		STRINGIFY(SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_sync_threshold},
	{"time_of_day", "<SYNC-INSTANCE>",
		"Sync instance to use for the time-of-day source. The default"
		"is the explicit or implicit ntp sync instance.",
		1, SFPTPD_CONFIG_SCOPE_GLOBAL,
		parse_time_of_day},
	{"master_clock_class", "<locked | holdover | freerunning>",
		"Master clock class. Default value for PPS is locked.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_master_clock_class},
	{"master_time_source", "<atomic | gps | ptp | ntp | oscillator>",
		"Master time source. Default value for PPS is GPS.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_master_time_source},
	{"master_accuracy", "<NUMBER | unknown>",
		"Master clock accuracy in ns or unknown. Default value for PPS is unknown.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_master_accuracy},
	{"master_traceability", "<time | freq>*",
		"Traceability of master time and frequency. Default for PPS is both.",
		~0, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_master_traceability},
	{"steps_removed", "<NUMBER>",
		"Number of steps between grandmaster and local clock. Default "
		"value for PPS is 1.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_steps_removed},
	{"pps_delay", "NUMBER",
		"PPS propagation delay in nanoseconds.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pps_delay},
	{"pid_filter_p", "NUMBER",
		"PID filter proportional term coefficient. Default value is "
		STRINGIFY(SFPTPD_PPS_DEFAULT_PID_FILTER_KP) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pid_filter_kp},
	{"pid_filter_i", "NUMBER",
		"PID filter integral term coefficient. Default value is "
		STRINGIFY(SFPTPD_PPS_DEFAULT_PID_FILTER_KI) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_pid_filter_ki},
	{"outlier_filter_type", "<disabled | std-dev>",
		"Specifies filter type to use to reject outliers. Default is "
		"std-dev i.e. based on a sample's distance from the mean "
		"expressed as a number of standard deviations.",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_outlier_filter_type},
	{"outlier_filter_size", "NUMBER",
		"Number of data samples stored in the filter. For std-dev type "
		"the valid range is ["
		STRINGIFY(SFPTPD_PEIRCE_FILTER_SAMPLES_MIN) ","
		STRINGIFY(SFPTPD_PEIRCE_FILTER_SAMPLES_MAX) "] and the default is "
		STRINGIFY(SFPTPD_PPS_DEFAULT_OUTLIER_FILTER_SIZE) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_outlier_filter_size},
	{"outlier_filter_adaption", "NUMBER",
		"Controls how outliers are fed into the filter, specified in "
		"the range [0,1]. A value of 0 means that outliers are not fed "
		"into filter (not recommended) whereas a value of 1 means that "
		"each outlier is fed into the filter unchanged. Values between "
		"result in a portion of the value being fed in. Default is "
		STRINGIFY(SFPTPD_PPS_DEFAULT_OUTLIER_FILTER_ADAPTION) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_outlier_adaption},
	{"fir_filter_size", "NUMBER",
		"Number of data samples stored in the FIR filter. The "
		"valid range is [" STRINGIFY(SFPTPD_FIR_FILTER_STIFFNESS_MIN)
		"," STRINGIFY(SFPTPD_FIR_FILTER_STIFFNESS_MAX) "]. A value of "
		"1 means that the filter is off while higher values will "
		"reduce the adaptability of PPS but increase its stability. "
		"Default is " STRINGIFY(SFPTPD_PPS_DEFAULT_FIR_FILTER_SIZE) ".",
		1, SFPTPD_CONFIG_SCOPE_INSTANCE,
		parse_fir_filter_size},

};

static const sfptpd_config_option_set_t pps_config_option_set =
{
	.description = "PPS Configuration File Options",
	.category = SFPTPD_CONFIG_CATEGORY_PPS,
	.num_options = sizeof(pps_config_options)/sizeof(pps_config_options[0]),
	.options = pps_config_options
};


/****************************************************************************
 * Internal Functions
 ****************************************************************************/

const char *pps_state_text(sfptpd_sync_module_state_t state, unsigned int alarms)
{
	static const char *states_text[SYNC_MODULE_STATE_MAX] = {
		"pps-listening",	/* SYNC_MODULE_STATE_LISTENING */
		"pps-slave",		/* SYNC_MODULE_STATE_SLAVE */
		"pps-faulty",		/* SYNC_MODULE_STATE_MASTER */
		"pps-faulty",		/* SYNC_MODULE_STATE_PASSIVE */
		"pps-faulty",		/* SYNC_MODULE_STATE_DISABLED */
		"pps-faulty",		/* SYNC_MODULE_STATE_FAULTY */
		"pps-faulty",		/* SYNC_MODULE_STATE_SELECTION */
	};

	assert(state < SYNC_MODULE_STATE_MAX);

	if ((state == SYNC_MODULE_STATE_SLAVE) && (alarms != 0))
		return "pps-slave-alarm";

	return states_text[state];
}


static int pps_test_mode_bogus_event(pps_module_t *pps,
				     struct sfptpd_pps_instance *instance,
				     unsigned int *seq_num,
				     struct sfptpd_timespec *time)
{
	assert(pps != NULL);
	assert(seq_num != NULL);
	assert(time != NULL);
	assert(instance->test.bogus_pps_events);

	if ((rand() & 0xf) == 0) {
		*seq_num = 12345678;
		*time = instance->pps_timestamp;
		time->nsec += ((rand() * rand()) % 1000000000);
		sfptpd_time_normalise(time);
		return 0;
	}

	return EAGAIN;
}


static void pps_servo_reset(pps_module_t *pps,
			    struct sfptpd_pps_instance *instance)
{
	assert(pps != NULL);
	assert(instance != NULL);

	sfptpd_fir_filter_reset(&instance->fir_filter);
	sfptpd_pid_filter_reset(&instance->pid_filter);

	instance->freq_adjust_base = sfptpd_clock_get_freq_correction(instance->clock);
	instance->freq_adjust_ppb = instance->freq_adjust_base;
	instance->offset_from_master_ns = 0.0;

	sfptpd_time_zero(&pps->time_of_day.status.offset_from_master);
	sfptpd_time_zero(&instance->pps_timestamp);

	instance->pps_period_ns = 0.0;

	TRACE_L4("pps %s: reset servo filters\n",
		 SFPTPD_CONFIG_GET_NAME(instance->config));
}


static void pps_servo_step_clock(pps_module_t *pps,
				 struct sfptpd_pps_instance *instance,
				 struct sfptpd_timespec *offset)
{
	int rc;
	struct sfptpd_timespec zero = sfptpd_time_null();

	assert(pps != NULL);
	assert(offset != NULL);

	/* We actually need to step the clock backwards by the specified offset */
	sfptpd_time_subtract(offset, &zero, offset);

	/* Step the slave clock by the specified offset */
	rc = sfptpd_clock_adjust_time(instance->clock, offset);
	if (rc != 0) {
		WARNING("pps %s: failed to adjust offset of clock %s, error %s\n",
		        SFPTPD_CONFIG_GET_NAME(instance->config),
			sfptpd_clock_get_long_name(instance->clock), strerror(rc));
	}

	/* Get the current frequency correction for the slave clock and set the
	 * clock frequency back to the last good value. */
	rc = sfptpd_clock_adjust_frequency(instance->clock,
			sfptpd_clock_get_freq_correction(instance->clock));
	if (rc != 0) {
		WARNING("pps %s: failed to adjust frequency of clock %s, error %s\n",
		        SFPTPD_CONFIG_GET_NAME(instance->config),
			sfptpd_clock_get_long_name(instance->clock), strerror(rc));
	}

	/* Reset the filters and calculated adjustments */
	pps_servo_reset(pps, instance);

	/* Tell the sync module that the clock has been stepped */
	sfptpd_sync_module_step_clock(pps->time_of_day.source.module,
				      pps->time_of_day.source.handle,
				      &zero);
}


static void pps_servo_update(pps_module_t *pps,
			     struct sfptpd_pps_instance *instance,
			     struct sfptpd_timespec *pps_timestamp,
			     struct sfptpd_timespec *time_of_day)
{
	int rc;
	sfptpd_time_t diff_ns, mean;
	struct sfptpd_config_general *general_config;
	enum sfptpd_clock_ctrl clock_ctrl;
	struct sfptpd_timespec diff;

	assert(pps != NULL);
	assert(pps_timestamp != NULL);
	assert(time_of_day != NULL);

	general_config = sfptpd_general_config_get(SFPTPD_CONFIG_TOP_LEVEL(instance->config));
	clock_ctrl = general_config->clocks.control;

	/* The seconds is the time of day rounded to the nearest second */
	diff.sec = time_of_day->sec;
	if (time_of_day->nsec >= 500000000)
		diff.sec += 1;

	/* The nanosecond value comes from the PPS timestamp */
	diff.nsec = pps_timestamp->nsec;
	if (diff.nsec >= 500000000)
		diff.sec -= 1;
	diff.nsec_frac = 0;

	diff_ns = sfptpd_time_timespec_to_float_ns(&diff);

	/* Subtract the PPS propagation delay from the difference between the remote
	 * PPS source and local time to account for PPS cable and distribution
	 * delays. */
	diff_ns -= instance->config->propagation_delay;

	TRACE_L6("pps %s: offset = %0.3Lf\n", SFPTPD_CONFIG_GET_NAME(instance->config), diff_ns);

	/* If clock stepping is enabled and the difference between the master
	 * and slave clocks is larger than the step threshold then step the
	 * clock */
	if ((clock_ctrl == SFPTPD_CLOCK_CTRL_SLEW_AND_STEP) ||
	    ((clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_AT_STARTUP) && !instance->servo_active) ||
	    ((clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_FORWARD) && (diff_ns < 0))) {
		if ((diff_ns <= -PPS_CLOCK_STEP_THRESHOLD) ||
		    (diff_ns >= PPS_CLOCK_STEP_THRESHOLD)) {
			if (instance->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) {
				/* Step the clock and return */
				(void)pps_servo_step_clock(pps, instance, &diff);

				/* Mark the servo as active */
				instance->counters.clock_steps++;
				instance->servo_active = true;
			}

			return;
		}
	}

	/* Add the new sample to the filter and get back the filtered delta */
	mean = sfptpd_fir_filter_update(&instance->fir_filter, diff_ns);

	TRACE_L6("pps %s: mean difference = %0.3Lf\n", SFPTPD_CONFIG_GET_NAME(instance->config), mean);

	/* Store the filtered offset from master */
	instance->offset_from_master_ns = mean;

	/* If we are not currently controlling the clock, the frequency
	 * adjustment is the saved value. If we are controlling the clock then
	 * we apply the output of the PID filter to this value. */
	instance->freq_adjust_ppb = instance->freq_adjust_base;

	if (instance->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) {
		instance->freq_adjust_ppb
			+= sfptpd_pid_filter_update(&instance->pid_filter, mean, NULL);

		/* Saturate the frequency adjustment */
		if (instance->freq_adjust_ppb > instance->freq_adjust_max)
			instance->freq_adjust_ppb = instance->freq_adjust_max;
		else if (instance->freq_adjust_ppb < -instance->freq_adjust_max)
			instance->freq_adjust_ppb = -instance->freq_adjust_max;

		/* Adjust the clock frequency using the calculated adjustment */
		rc = sfptpd_clock_adjust_frequency(instance->clock, instance->freq_adjust_ppb);
		if (rc != 0) {
			WARNING("pps %s: failed to adjust clock %s, error %s\n",
				SFPTPD_CONFIG_GET_NAME(instance->config), 
				sfptpd_clock_get_long_name(instance->clock), strerror(rc));
		}

		/* Mark the pps servo as active */
		instance->servo_active = true;
	}
}


static struct sfptpd_pps_instance *pps_find_instance_by_clock(pps_module_t *pps,
							      struct sfptpd_clock *clock) {
	struct sfptpd_pps_instance *instance;

	/* Walk linked list, looking for the clock */
	for (instance = pps->instances;
	     instance && instance->clock != clock;
	     instance = instance->next);

	return instance;
}


static bool pps_is_instance_in_list(pps_module_t *pps,
				    struct sfptpd_pps_instance *instance) {
	struct sfptpd_pps_instance *ptr;

	assert(instance);

	/* Walk linked list, looking for the clock */
	for (ptr = pps->instances;
	     ptr && ptr != instance;
	     ptr = ptr->next);

	return (ptr == NULL) ? false : true;
}


/* Finalise the contents of an instance. The instance itself will be
   freed with the list containing it. */
static void pps_destroy_instance(pps_module_t *pps,
				 struct sfptpd_pps_instance *instance) {

	assert(pps != NULL);
	assert(instance != NULL);

	if (instance->poll_fd != -1) {
	        sfptpd_thread_user_fd_remove(instance->poll_fd);
		instance->poll_fd = -1;
	}

	/* Disable PPS events in the driver */
	if (instance->clock != NULL) {
		(void)sfptpd_clock_pps_disable(instance->clock);
		instance->clock = NULL;
	}

	if (instance->outlier_filter != NULL) {
		sfptpd_peirce_filter_destroy(instance->outlier_filter);
		instance->outlier_filter = NULL;
	}

	sfptpd_stats_collection_free(&instance->stats);
}


static void pps_destroy_instances(pps_module_t *pps) {
	struct sfptpd_pps_instance *instance;
	struct sfptpd_pps_instance *next;

	next = pps->instances;
	pps->instances = NULL;

	for (instance = next; instance; instance = next) {
		next = instance->next;
		pps_destroy_instance(pps, instance);
		free(instance);
	}
}


static int pps_create_instances(struct sfptpd_config *config,
				pps_module_t *pps)
{
	sfptpd_pps_module_config_t *instance_config;
	struct sfptpd_pps_instance *instance, **instance_ptr;

	assert(config != NULL);
	assert(pps != NULL);
	assert(pps->instances == NULL);

	/* Prepare linked list */
	instance_ptr = &pps->instances;

	/* Setting up initial state: find the first instance configuration */
	instance_config = (struct sfptpd_pps_module_config *)
		sfptpd_config_category_first_instance(config,
						      SFPTPD_CONFIG_CATEGORY_PPS);

	/* Loop round available instance configurations */
	while (instance_config) {
		INFO("pps %s: creating sync-instance\n", SFPTPD_CONFIG_GET_NAME(instance_config));

		instance = (struct sfptpd_pps_instance *)calloc(1, sizeof *instance);
		if (instance == NULL) {
			CRITICAL("pps %s: failed to allocate sync instance memory\n",
				 SFPTPD_CONFIG_GET_NAME(instance_config));
			pps_destroy_instances(pps);
			return ENOMEM;
		}

		/* Populate instance state */
		instance->config = instance_config;
		instance->clustering_evaluator.calc_fn = sfptpd_engine_calculate_clustering_score;
		instance->clustering_evaluator.private = pps->engine;
		instance->clustering_evaluator.instance_name = SFPTPD_CONFIG_GET_NAME(instance_config);
		instance->poll_fd = -1;

		/* Append to linked list */
		*instance_ptr = instance;
		instance_ptr = &instance->next;

		TRACE_L3("pps %s: instance is %p\n", SFPTPD_CONFIG_GET_NAME(instance_config), instance);

		/* Get next configuration, if present */
		instance_config = (struct sfptpd_pps_module_config *)
			sfptpd_config_category_next_instance(&instance_config->hdr);
	}

	return 0;
}


static int pps_drain_events(pps_module_t *pps, struct sfptpd_pps_instance *instance)
{
	int i;
	int rc = EAGAIN;
	uint32_t seq_num;
	struct sfptpd_timespec time;
	const int max_drain = 1000;

	assert(pps != NULL);
	assert(instance != NULL);

	struct pollfd pfd;

	for (i = 0; rc == EAGAIN && i < max_drain; i++) {
		pfd.fd = instance->poll_fd;
		pfd.events = POLLIN;
		rc = poll(&pfd, 1, 1);
		if (rc < 0) {
			rc = errno;
		} else if (rc > 0 && (pfd.revents & POLLIN)) {
			rc = sfptpd_clock_pps_get(instance->clock, &seq_num, &time);
			if (rc == 0)
				rc = EAGAIN;
		} else {
			rc = 0;
		}
	}

	if (rc != 0) {
		ERROR("pps %s: draining PPS events: %s\n",
		      SFPTPD_CONFIG_GET_NAME(instance->config),
		      strerror(rc));
	} else if (i == max_drain) {
		WARNING("pps %s: gave up after draining %d PPS events\n",
		      SFPTPD_CONFIG_GET_NAME(instance->config),
		      max_drain);
	} else if (i != 0) {
		INFO("pps %s: swallowed %d PPS events\n",
		      SFPTPD_CONFIG_GET_NAME(instance->config),
		     i);
	}

	return rc;
}


static int pps_configure_clock(pps_module_t *pps,
			       struct sfptpd_pps_instance *instance,
			       struct sfptpd_pps_module_config *config)
{
	struct sfptpd_clock *clock;
	struct sfptpd_interface *interface;
	struct sfptpd_pps_instance *other_instance;
	long double freq_correction_ppb;
	struct sfptpd_config_general *general_config;
	int rc;

	assert(pps != NULL);
	assert(instance != NULL);
	assert(config != NULL);

	general_config = sfptpd_general_config_get(SFPTPD_CONFIG_TOP_LEVEL(instance->config));
	assert(general_config != NULL);

	/* Make sure that the user has specified an interface */
	if (config->interface_name[0] == '\0') {
		ERROR("pps %s: no interface specified\n",
		      SFPTPD_CONFIG_GET_NAME(config));
		return EINVAL;
	}

	/* Find the specified interface */
	interface = sfptpd_interface_find_by_name(config->interface_name);
	if (interface == NULL) {
		ERROR("pps %s: couldn't find interface %s\n",
		      SFPTPD_CONFIG_GET_NAME(config),
		      config->interface_name);
		return ENODEV;
	}

	/* Check that the interface supports PPS */
	if (!sfptpd_interface_supports_pps(interface)) {
		ERROR("pps %s: interface %s doesn't support PPS\n",
		      SFPTPD_CONFIG_GET_NAME(config),
		      config->interface_name);
		return ENODEV;
	}

	/* Get the PTP clock based on the specified interface. */
	clock = sfptpd_interface_get_clock(interface);
	assert((clock != NULL) &&
	       (clock != sfptpd_clock_get_system_clock()));

	/* Check if the clock is in use in another instance */
	other_instance = pps_find_instance_by_clock(pps, clock);
	if (other_instance) {
		ERROR("pps %s: clock on nic %s is already in use for instance %s\n",
		      SFPTPD_CONFIG_GET_NAME(config),
		      config->interface_name,
		      other_instance->config->hdr.name);
		return EBUSY;
	}

	/* Check that the clock is specified in the list of clocks to be
	 * disciplined */
	if (!sfptpd_clock_get_discipline(clock)) {
		ERROR("pps %s: clock %s is not configured to be disciplined\n",
		      SFPTPD_CONFIG_GET_NAME(config), sfptpd_clock_get_long_name(clock));
		if (general_config->ignore_critical[SFPTPD_CRITICAL_NO_PTP_CLOCK])
			NOTICE("ptp: ignoring critical error by configuration\n");
		else {
			NOTICE("configure \"ignore_critical: no-ptp-clock\" to allow sfptpd to start in spite of this condition\n");
			return EPERM;
		}
	}

	INFO("pps %s: local reference clock is %s\n",
	     SFPTPD_CONFIG_GET_NAME(config), sfptpd_clock_get_long_name(clock));

	/* Get the current frequency correction and the maximum permitted 
	 * frequency adjustment for this clock */
	freq_correction_ppb = sfptpd_clock_get_freq_correction(clock);
	instance->freq_adjust_max = sfptpd_clock_get_max_frequency_adjustment(clock);

	/* Configure the PID filter max integral term to match the max frequency
	 * adjust of the slave clock */
	sfptpd_pid_filter_set_i_term_max(&instance->pid_filter, instance->freq_adjust_max);

	/* Set the clock frequency to the default value */
	rc = sfptpd_clock_adjust_frequency(clock, freq_correction_ppb);
	if (rc != 0) {
		WARNING("pps %s: failed to adjust frequency of clock %s, error %s\n",
			SFPTPD_CONFIG_GET_NAME(config),
			sfptpd_clock_get_long_name(clock), strerror(rc));
		return rc;
	}

	/* To make sure the firmware is in a good state, disable then enable
	 * the PPS events */
	(void)sfptpd_clock_pps_disable(clock);

	/* Enable PPS events in the driver */
	rc = sfptpd_clock_pps_enable(clock);
	if (rc != 0) {
		ERROR("pps %s: failed to enable PPS input for interface %s, %s\n",
		      SFPTPD_CONFIG_GET_NAME(config),
		      config->interface_name, strerror(rc));
		return EIO;
	}

	/* Store the clock */
	instance->clock = clock;

	return 0;
}


static void pps_convergence_init(pps_module_t *pps,
				 struct sfptpd_pps_instance *instance)
{
	long double threshold;

	assert(pps != NULL);
	assert(instance != NULL);

	/* Initialise the convergence measure. */
	instance->synchronized = false;
	sfptpd_stats_convergence_init(&instance->convergence);

	/* Sets an appropriate convergence threshold.
	   Check if overriden by user. */
	threshold = instance->config->convergence_threshold;

	/* Otherwise use the default */
	if (threshold == 0) {
		threshold = SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT;
	}

	sfptpd_stats_convergence_set_max_offset(&instance->convergence, threshold);
}


static void pps_convergence_update(pps_module_t *pps,
				   struct sfptpd_pps_instance *instance)
{
	struct sfptpd_timespec time;
	int rc;
	assert(pps != NULL);
	assert(instance != NULL);

	rc = sfclock_gettime(CLOCK_MONOTONIC, &time);
	if (rc < 0) {
		ERROR("pps %s: failed to get monotonic time, %s\n",
                      SFPTPD_CONFIG_GET_NAME(instance->config), strerror(errno));
	}

	/* If not in the slave state or we failed to get the time for some
	 * reason, reset the convergence measure. */
	if ((rc < 0) || (instance->state != SYNC_MODULE_STATE_SLAVE)) {
		instance->synchronized = false;
		sfptpd_stats_convergence_reset(&instance->convergence);
	} else if ((instance->alarms != 0) ||
		   ((instance->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0)) {
		/* If one or more alarms is triggered or timestamp processing
		 * is disabled, we consider the slave to be unsynchronized.
		 * However, don't reset the convergence measure as it is
		 * probably a temporary situation. */
	} else {
		/* Update the synchronized state based on the current offset
		 * from master */
		instance->synchronized = sfptpd_stats_convergence_update(&instance->convergence,
								    time.sec,
								    instance->offset_from_master_ns);
	}
}


static int pps_stats_init(pps_module_t *pps,
			  struct sfptpd_pps_instance *instance)
{
	int rc;

	assert(pps != NULL);
	assert(instance != NULL);

	instance->counters.clock_steps = 0;
	instance->counters.seq_num_errors = 0;
	instance->counters.bad_signal_errors = 0;
	instance->counters.outliers = 0;

	/* Create the statistics collection */
	rc = sfptpd_stats_collection_create(&instance->stats, "pps",
					    sizeof(pps_stats_defns)/sizeof(pps_stats_defns[0]),
					    pps_stats_defns);
	return rc;
}


static void pps_stats_update(pps_module_t *pps,
			     struct sfptpd_pps_instance *instance)
{
	struct sfptpd_stats_collection *stats;
	struct sfptpd_timespec now;
	int cond;
	assert(pps != NULL);

	stats = &instance->stats;
	sfptpd_clock_get_time(sfptpd_clock_get_system_clock(), &now);

	/* Offset, frequency correction, one-way-delay */
	sfptpd_stats_collection_update_range(stats, PPS_STATS_ID_OFFSET,
					     instance->offset_from_master_ns,
					     now,
					     instance->state == SYNC_MODULE_STATE_SLAVE);
	sfptpd_stats_collection_update_range(stats, PPS_STATS_ID_FREQ_ADJ,
					     instance->freq_adjust_ppb,
					     now,
					     instance->state == SYNC_MODULE_STATE_SLAVE);
	sfptpd_stats_collection_update_count(stats, PPS_STATS_ID_SYNCHRONIZED,
					     instance->synchronized? 1: 0);

	/* If the period is non-zero, record it */
	if (instance->pps_period_ns > 0) {
		sfptpd_stats_collection_update_range(stats, PPS_STATS_ID_PERIOD,
						     instance->pps_period_ns,
						     now,
						     instance->state == SYNC_MODULE_STATE_SLAVE);
	}

	sfptpd_stats_collection_update_count(stats, PPS_STATS_ID_CLOCK_STEPS,
					     instance->counters.clock_steps);
	instance->counters.clock_steps = 0;

	cond = (SYNC_MODULE_ALARM_TEST(instance->prev_alarms, PPS_NO_SIGNAL) &&
		!SYNC_MODULE_ALARM_TEST(instance->alarms, PPS_NO_SIGNAL));
	sfptpd_stats_collection_update_count(stats, PPS_STATS_ID_NO_SIGNAL_ERRORS,
					     cond? 1: 0);

	sfptpd_stats_collection_update_count(stats, PPS_STATS_ID_SEQ_NUM_ERRORS,
					     instance->counters.seq_num_errors);
	instance->counters.seq_num_errors = 0;

	cond = (SYNC_MODULE_ALARM_TEST(instance->prev_alarms, NO_TIME_OF_DAY) &&
		!SYNC_MODULE_ALARM_TEST(instance->alarms, NO_TIME_OF_DAY));
	sfptpd_stats_collection_update_count(stats, PPS_STATS_ID_TIME_OF_DAY_ERRORS,
					     cond? 1: 0);

	sfptpd_stats_collection_update_count(stats, PPS_STATS_ID_BAD_SIGNAL_ERRORS,
					     instance->counters.bad_signal_errors);
	instance->counters.bad_signal_errors = 0;

	sfptpd_stats_collection_update_count(stats, PPS_STATS_ID_OUTLIERS,
					     instance->counters.outliers);
	instance->counters.outliers = 0;
}


static void pps_state_machine_reset(pps_module_t *pps,
				    struct sfptpd_pps_instance *instance)
{
	assert(pps != NULL);
	assert(instance != NULL);

	instance->state = SYNC_MODULE_STATE_LISTENING;
	instance->prev_state = SYNC_MODULE_STATE_LISTENING;
	instance->alarms = 0;
	instance->prev_alarms = 0;
	instance->consecutive_good_periods = 0;
	sfptpd_time_zero(&instance->pps_timestamp);
	instance->pps_seq_num = 0;
	instance->pps_period_ns = 0.0;
	if (instance->outlier_filter != NULL)
		sfptpd_peirce_filter_reset(instance->outlier_filter);
}


static void pps_on_no_pps_event(pps_module_t *pps,
				struct sfptpd_pps_instance *instance)
{
	struct sfptpd_timespec time_now, interval;
	assert(pps != NULL);

	switch (instance->state) {
	case SYNC_MODULE_STATE_LISTENING:
		/* We're already in the listening state so there's nothing to
		 * do here */
		break;

	case SYNC_MODULE_STATE_SLAVE:
		/* Check how long it has been since the last PPS event */
		(void)sfclock_gettime(CLOCK_MONOTONIC, &time_now);
		sfptpd_time_subtract(&interval, &time_now, &instance->last_pps_time);

		/* We check two intervals. After a short time (just over a
		 * second) we go to the alarm state. After a longer period
		 * (some number of seconds) we return to the listening
		 * state. */
		if (sfptpd_time_is_greater_or_equal(&interval, &pps_timeout_interval)) {
			ERROR("pps %s: no event after %ld seconds. Changing to listening state.\n",
			      SFPTPD_CONFIG_GET_NAME(instance->config), pps_timeout_interval.sec);
			pps_state_machine_reset(pps, instance);
		} else if (sfptpd_time_is_greater_or_equal(&interval, &pps_alarm_interval) &&
			   !SYNC_MODULE_ALARM_TEST(instance->alarms, PPS_NO_SIGNAL)) {
			WARNING("pps %s: failed to receive PPS event for sequence number %d\n",
				SFPTPD_CONFIG_GET_NAME(instance->config), (uint32_t)(instance->pps_seq_num + 1));
			SYNC_MODULE_ALARM_SET(instance->alarms, PPS_NO_SIGNAL);
		}
		break;

	case SYNC_MODULE_STATE_FAULTY:
		/* The interface seems to have started working again. Go to the
		 * listening state */
		pps_state_machine_reset(pps, instance);
		break;

	default:
		assert(false);
		break;
	}
}


static void pps_on_pps_error(pps_module_t *pps,
			     struct sfptpd_pps_instance *instance,
			     int rc)
{
	assert(pps != NULL);
	assert(rc != 0);

	switch (instance->state) {
	case SYNC_MODULE_STATE_LISTENING:
	case SYNC_MODULE_STATE_SLAVE:
		/* The interface has stopped working. Go to the faulty state! */
		CRITICAL("pps %s: interface error, %s\n",
                         SFPTPD_CONFIG_GET_NAME(instance->config), strerror(rc));
		pps_state_machine_reset(pps, instance);
		instance->state = SYNC_MODULE_STATE_FAULTY;
		break;

	case SYNC_MODULE_STATE_FAULTY:
		/* Nothing to do here */
		break;

	default:
		assert(false);
		break;
	}
}


static void pps_send_rt_stats_update(pps_module_t *pps, struct sfptpd_log_time time)
{
	struct sfptpd_pps_instance *instance;

	assert(pps != NULL);

	for (instance = pps->instances; instance != NULL; instance = instance->next) {
		if (instance->state == SYNC_MODULE_STATE_SLAVE) {
			sfptpd_engine_post_rt_stats(pps->engine,
				&time,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				"pps",
				NULL,
				instance->clock,
				(instance->ctrl_flags & SYNC_MODULE_SELECTED),
				false,
				instance->synchronized,
				instance->alarms,
				STATS_KEY_OFFSET, instance->offset_from_master_ns,
				STATS_KEY_FREQ_ADJ, instance->freq_adjust_ppb,
				STATS_KEY_P_TERM, sfptpd_pid_filter_get_p_term(&instance->pid_filter),
				STATS_KEY_I_TERM, sfptpd_pid_filter_get_i_term(&instance->pid_filter),
				STATS_KEY_END);
		}
	}
}


static void pps_send_clustering_input(struct sfptpd_pps_module *pps,
				      struct sfptpd_pps_instance *instance)
{
	assert(instance != NULL);

	if (instance->ctrl_flags & SYNC_MODULE_CLUSTERING_DETERMINANT) {
		sfptpd_time_t offset = instance->offset_from_master_ns;

		sfptpd_engine_clustering_input(pps->engine,
					       SFPTPD_CONFIG_GET_NAME(instance->config),
					       instance->clock,
					       offset,
					       finitel(offset) && offset != 0.0L &&
					       instance->state == SYNC_MODULE_STATE_SLAVE);
	}
}


static void pps_on_pps_event(pps_module_t *pps,
			     struct sfptpd_pps_instance *instance,
			     uint32_t seq_num,
			     struct sfptpd_timespec *time)
{
	struct sfptpd_timespec period;
	int rc = 0;

	assert(pps != NULL);
	assert(time != NULL);

	switch (instance->state) {
	case SYNC_MODULE_STATE_FAULTY:
	case SYNC_MODULE_STATE_LISTENING:
		/* Change to the slave state */
		instance->state = SYNC_MODULE_STATE_SLAVE;
		instance->pps_period_ns = 0.0;
		break;

	case SYNC_MODULE_STATE_SLAVE:
		/* Clear the no signal alarm */
		SYNC_MODULE_ALARM_CLEAR(instance->alarms, PPS_NO_SIGNAL);

		/* Check that the sequence number has incremented.
		   Not all PPS event retrieval mechanisms have a sequence number
		   concept: signal this with a UINT32_MAX value.
		 */
		if (seq_num != UINT32_MAX &&
		    seq_num != (uint32_t)(instance->pps_seq_num + 1)) {
			WARNING("pps %s: sequence number discontinuity %d -> %d\n",
				SFPTPD_CONFIG_GET_NAME(instance->config), 
				instance->pps_seq_num, seq_num);
			SYNC_MODULE_ALARM_SET(instance->alarms, PPS_SEQ_NUM_ERROR);
			instance->counters.seq_num_errors++;
		} else {
			SYNC_MODULE_ALARM_CLEAR(instance->alarms, PPS_SEQ_NUM_ERROR);
		}

		/* If we timestamp processing is disabled go no further!!! */
		if ((instance->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0)
			break;

		/* If the previous PPS time is valid (i.e. non zero),
		 * calculate the PPS period */
		if (instance->pps_timestamp.sec != 0) {
			sfptpd_time_subtract(&period, time, &instance->pps_timestamp);

			instance->pps_period_ns = sfptpd_time_timespec_to_float_ns(&period);

			/* If we have a period then apply a notch filter to
			 * detect and eliminate bad PPS pulses */
			if (sfptpd_notch_filter_update(&instance->notch_filter,
						       instance->pps_period_ns) != 0) {
				WARNING("pps %s: bad signal- pps period = %Lf\n",
					SFPTPD_CONFIG_GET_NAME(instance->config), 
					instance->pps_period_ns);
				SYNC_MODULE_ALARM_SET(instance->alarms, PPS_BAD_SIGNAL);
				instance->counters.bad_signal_errors++;
				instance->consecutive_good_periods = 0;
			} else {
				instance->consecutive_good_periods++;
			}
		}

		/* We only execute the PPS servo if we have had enough
		 * consecutive good PPS periods to trust the PPS events */
		if (instance->consecutive_good_periods >= PPS_REQUIRED_GOOD_PERIODS) {
			if (instance->consecutive_good_periods == PPS_REQUIRED_GOOD_PERIODS) {
				INFO("pps %s: received first %d consecutive good PPS events\n",
				     SFPTPD_CONFIG_GET_NAME(instance->config), 
				     PPS_REQUIRED_GOOD_PERIODS + 1);
			}

			SYNC_MODULE_ALARM_CLEAR(instance->alarms, PPS_BAD_SIGNAL);

			/* Apply the outlier filter. If the sample is detected
			 * as an outlier then we do not adjust the clock */
			if (instance->outlier_filter != NULL) {
				rc = sfptpd_peirce_filter_update(instance->outlier_filter,
								 instance->pps_period_ns);
				if (rc != 0) {
					TRACE_L3("pps %s: outlier detected- period %0.3Lf\n",
						 SFPTPD_CONFIG_GET_NAME(instance->config), 
						 instance->pps_period_ns);
					/* Update the outliers count */
					instance->counters.outliers++;
				}
			}

			if (rc == 0) {
				pps_servo_update(pps, instance, time,
						 &pps->time_of_day.status.offset_from_master);

				/* Send updated stats and clustering input to engine */
				struct sfptpd_log_time log_time;
				sfptpd_log_get_time(&log_time);
				pps_send_clustering_input(pps, instance);
				pps_send_rt_stats_update(pps, log_time);

				/* Calculate clustering score */
				instance->clustering_score =
					instance->clustering_evaluator.calc_fn(
						&instance->clustering_evaluator,
						instance->offset_from_master_ns,
						instance->clock);
			}
		}
		break;

	default:
		assert(false);
		break;
	}

	/* Record the sequence number of the PPS event and the monotonic time
	 * that it has occurred in all cases. However, we only record the
	 * timestamp itself if timestamp processing is enabled. */
	instance->pps_seq_num = seq_num;
	(void)sfclock_gettime(CLOCK_MONOTONIC, &instance->last_pps_time);
	if (instance->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING)
		instance->pps_timestamp = *time;
}


static int pps_time_of_day_init(pps_module_t *pps)
{
	assert(pps != NULL);

	/* Get the handle of the time-of-day module */
	if (pps->instances->config->tod_name[0] != '\0') {
		const struct sfptpd_sync_instance_info *info;
		info = sfptpd_engine_get_sync_instance_by_name(
			pps->engine,
			pps->instances->config->tod_name);
		if (info)
			pps->time_of_day.source = *info;
	} else {
		pps->time_of_day.source.module = sfptpd_engine_get_ntp_module(
			pps->engine);
		pps->time_of_day.source.handle = NULL;
		pps->time_of_day.source.name = "auto";
	}

	if (pps->time_of_day.source.module == NULL) {
		TRACE_L4("pps: no sync module for time-of-day; will try again later\n");
		return ENOENT;
	}

	(void)sfclock_gettime(CLOCK_MONOTONIC, &pps->time_of_day.next_poll_time);
	pps->time_of_day.status.state = SYNC_MODULE_STATE_LISTENING;
	sfptpd_time_zero(&pps->time_of_day.status.offset_from_master);

	return 0;
}


static void pps_time_of_day_poll(pps_module_t *pps,
				 struct sfptpd_pps_instance *instance)
{
	struct sfptpd_timespec time_now, time_left;
	struct sfptpd_timespec system_to_nic;
	int rc;

	assert(pps != NULL);

	/* Check whether it's time to poll for time of day again */
	(void)sfclock_gettime(CLOCK_MONOTONIC, &time_now);
	sfptpd_time_subtract(&time_left, &pps->time_of_day.next_poll_time, &time_now);
	if (time_left.sec >= 0)
		return;

	pps->time_of_day.next_poll_time.sec += 1;

	if (pps->time_of_day.source.module == NULL) {

		/* If we failed to get the time of day sync module before,
		   look for it again. */
		rc = pps_time_of_day_init(pps);

		assert((rc == 0 && pps->time_of_day.source.module != NULL) ||
		       rc == ENOENT);
	}

	if (pps->time_of_day.source.module != NULL) {

		/* Get the offset from the sync module. If the offset is valid (non zero)
		 * then work out the offset from the master to our NIC.
		 * NOTE there is an assumption that the offset here is from the master to
		 * the system clock- true for NTP but not true generally. */
		rc = sfptpd_sync_module_get_status(pps->time_of_day.source.module,
						   pps->time_of_day.source.handle,
						   &pps->time_of_day.status);
		if (rc == 0 && !sfptpd_time_is_zero(&pps->time_of_day.status.offset_from_master)) {
			rc = sfptpd_clock_compare(instance->clock,
						  sfptpd_clock_get_system_clock(),
						  &system_to_nic);
			if (rc == 0) {
				TRACE_L5("pps %s: ntp->sys " SFPTPD_FORMAT_FLOAT
					 ", sys->nic " SFPTPD_FORMAT_FLOAT "\n",
					 SFPTPD_CONFIG_GET_NAME(instance->config),
					 sfptpd_time_timespec_to_float_ns(&pps->time_of_day.status.offset_from_master),
					 sfptpd_time_timespec_to_float_ns(&system_to_nic));

				sfptpd_time_add(&pps->time_of_day.status.offset_from_master,
						&pps->time_of_day.status.offset_from_master,
						&system_to_nic);
			}
		}
	}

	/* If the state of the time of day module is not slave then we don't
	 * have access to a time of day- sound the alarm! */
	if ((pps->time_of_day.status.state == SYNC_MODULE_STATE_SLAVE) ||
	    (pps->time_of_day.status.state == SYNC_MODULE_STATE_SELECTION)) {
		SYNC_MODULE_ALARM_CLEAR(instance->alarms, NO_TIME_OF_DAY);
	} else if (!SYNC_MODULE_ALARM_TEST(instance->alarms, NO_TIME_OF_DAY)) {
		WARNING("pps %s: time-of-day module error\n", 
			SFPTPD_CONFIG_GET_NAME(instance->config));
		SYNC_MODULE_ALARM_SET(instance->alarms, NO_TIME_OF_DAY);
	}

	TRACE_L5("pps %s: time-of-day state %d, offset " SFPTPD_FORMAT_FLOAT "\n",
		 SFPTPD_CONFIG_GET_NAME(instance->config),
		 pps->time_of_day.status.state,
		 sfptpd_time_timespec_to_float_ns(&pps->time_of_day.status.offset_from_master));
}


static int pps_do_poll(pps_module_t *pps, struct sfptpd_pps_instance *instance)
{
	int rc;
	uint32_t seq_num;
	struct sfptpd_timespec time;
	bool state_changed;
	struct sfptpd_pps_module_config *config;

	assert(pps != NULL);
	assert(instance != NULL);

	/* Get the next PPS event */
	rc = sfptpd_clock_pps_get(instance->clock, &seq_num, &time);

	/* If bogus PPS event test mode is enabled and we didn't get
	 * a PPS event, randomly generate one */
	if (instance->test.bogus_pps_events && (rc == EAGAIN))
		rc = pps_test_mode_bogus_event(pps, instance, &seq_num, &time);

	if (rc == EAGAIN) {
		pps_on_no_pps_event(pps, instance);
	} else if (rc != 0) {
		pps_on_pps_error(pps, instance, rc);
	} else {
		pps_on_pps_event(pps, instance, seq_num, &time);
	}

	/* Poll for time of day. */
	pps_time_of_day_poll(pps, instance);

	/* Update the convergence criteria */
	pps_convergence_update(pps, instance);

	state_changed = false;
	if ((instance->state != instance->prev_state) ||
	    ((instance->state == SYNC_MODULE_STATE_SLAVE) &&
	     ((instance->alarms == 0) != (instance->prev_alarms == 0)))) {
		state_changed = true;
		INFO("pps %s: state changed from %s to %s\n",
		     SFPTPD_CONFIG_GET_NAME(instance->config),
		     pps_state_text(instance->prev_state, instance->prev_alarms),
		     pps_state_text(instance->state, instance->alarms));
	}

	if (instance->clustering_score != instance->prev_clustering_score) {
		state_changed = true;
		INFO("%s: clustering score changed %d -> %d\n",
		     SFPTPD_CONFIG_GET_NAME(instance->config),
		     instance->prev_clustering_score,
		     instance->clustering_score);
	}

	/* Update historical stats */
	pps_stats_update(pps, instance);

	/* Update the snapshot of previous state */
	instance->prev_state = instance->state;
	instance->prev_alarms = instance->alarms;
	instance->prev_clustering_score = instance->clustering_score;

	/* If the state has changed, send an event to the sync engine */
	if (state_changed) {
		sfptpd_sync_instance_status_t status = { 0 };

		config = instance->config;

		status.state = instance->state;
		status.alarms = instance->alarms;
		status.clock = instance->clock;
		status.local_accuracy = SFPTPD_ACCURACY_PPS;
		status.master.clock_id = SFPTPD_CLOCK_ID_UNINITIALISED;
		status.clustering_score = instance->clustering_score;

		if (instance->state == SYNC_MODULE_STATE_SLAVE) {
			sfptpd_time_float_ns_to_timespec(instance->offset_from_master_ns,
							 &status.offset_from_master);

			status.user_priority = config->priority;
			status.master.remote_clock = true;
			status.master.clock_class = config->master_clock_class;
			status.master.time_source = config->master_time_source;
			status.master.accuracy = config->master_accuracy;
			status.master.allan_variance = NAN;
			status.master.time_traceable = config->master_time_traceable;
			status.master.freq_traceable = config->master_freq_traceable;
			status.master.steps_removed = config->steps_removed;
		} else {
			sfptpd_time_zero(&status.offset_from_master);
			status.user_priority = instance->config->priority;
			status.master.remote_clock = false;
			status.master.clock_class = SFPTPD_CLOCK_CLASS_FREERUNNING;
			status.master.time_source = SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;
			status.master.accuracy = INFINITY;
			status.master.allan_variance = NAN;
			status.master.time_traceable = false;
			status.master.freq_traceable = false;
			status.master.steps_removed = 0;
		}

		sfptpd_engine_sync_instance_state_changed(pps->engine,
							  sfptpd_thread_self(),
							  (struct sfptpd_sync_instance *) instance,
							  &status);
	}

	return rc;
}


static void pps_on_timer(void *user_context, unsigned int id)
{
	int rc;
	struct sfptpd_timespec current_time, interval;
	pps_module_t *pps = (pps_module_t *)user_context;
	struct sfptpd_pps_instance *instance;

	assert(pps != NULL);

	for(instance = pps->instances; instance != NULL; instance = instance->next) {

		/* If the PPS pulse check timer hasn't started yet, start it. */
		if (!instance->instance_has_started) {
			instance->instance_has_started = true;
			sfclock_gettime(CLOCK_MONOTONIC, &instance->instance_started_time);
		} else if (!instance->pps_pulse_check_timer_expired) {

			/* Check if timer has expired. */
			sfclock_gettime(CLOCK_MONOTONIC, &current_time);
			sfptpd_time_subtract(&interval, &current_time, &instance->instance_started_time);
			/* If timer has expired, then check if we haven't see 4 good pulses. */
			if (sfptpd_time_is_greater_or_equal(&interval, &pps_pulse_timeout_interval)) {
				instance->pps_pulse_check_timer_expired = true;
				if (instance->consecutive_good_periods < PPS_REQUIRED_GOOD_PERIODS) {
					WARNING("pps %s: did not see %d consecutive good PPS events after %ld seconds.\n",
						SFPTPD_CONFIG_GET_NAME(instance->config),
						PPS_REQUIRED_GOOD_PERIODS + 1,
						pps_pulse_timeout_interval.sec);
					SYNC_MODULE_ALARM_SET(instance->alarms, PPS_NO_SIGNAL);
				}
			}
		}

		/* Repeat until we run out of PPS events */
		do {
			if (instance->poll_fd == -1) {
				rc = pps_do_poll(pps, instance);
			} else {
				/* Allow the time since last PPS event to be
				   measured. */
				pps_on_no_pps_event(pps, instance);
				rc = EAGAIN;
			}
		} while (rc == 0);
	}
}


static void pps_on_user_fds(void *context, unsigned int num_fds, int fds[])
{
	int i;
	pps_module_t *pps = (pps_module_t *)context;
	struct sfptpd_pps_instance *instance;

	assert(pps != NULL);

	for(i = 0; i < num_fds; i++) {
		for(instance = pps->instances; instance != NULL; instance = instance->next) {
			if (instance->poll_fd == fds[i]) {
				pps_do_poll(pps, instance);
			}
		}
	}
}


static void pps_on_get_status(pps_module_t *pps, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_pps_instance *instance;
	struct sfptpd_sync_instance_status *status;

	assert(pps != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_pps_instance *) msg->u.get_status_req.instance_handle;
	assert(instance);
	assert(pps_is_instance_in_list(pps, instance));

	status = &msg->u.get_status_resp.status;
	status->state = instance->state;
	status->alarms = instance->alarms;
	status->clock = instance->clock;
	status->local_accuracy = SFPTPD_ACCURACY_PPS;
	status->master.clock_id = SFPTPD_CLOCK_ID_UNINITIALISED;
	status->clustering_score = instance->clustering_score;

	/* The offset is only valid in the slave state */
	if (instance->state == SYNC_MODULE_STATE_SLAVE) {
		sfptpd_time_float_ns_to_timespec(instance->offset_from_master_ns,
						 &status->offset_from_master);

		status->user_priority = instance->config->priority;
		status->master.remote_clock = true;
		status->master.clock_class = instance->config->master_clock_class;
		status->master.time_source = instance->config->master_time_source;
		status->master.accuracy = instance->config->master_accuracy;
		status->master.allan_variance = NAN;
		status->master.time_traceable = instance->config->master_time_traceable;
		status->master.freq_traceable = instance->config->master_freq_traceable;
		status->master.steps_removed = instance->config->steps_removed;
	} else {
		sfptpd_time_zero(&status->offset_from_master);
		status->user_priority = instance->config->priority;
		status->master.remote_clock = false;
		status->master.clock_class = SFPTPD_CLOCK_CLASS_FREERUNNING;
		status->master.time_source = SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR;
		status->master.accuracy = INFINITY;
		status->master.allan_variance = NAN;
		status->master.time_traceable = false;
		status->master.freq_traceable = false;
		status->master.steps_removed = 0;
	}

	SFPTPD_MSG_REPLY(msg);
}


static void pps_on_control(pps_module_t *pps,
			   sfptpd_sync_module_msg_t *msg)
{
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;
	struct sfptpd_pps_instance *instance;

	assert(pps != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_pps_instance *) msg->u.control_req.instance_handle;
	assert(instance);

	ctrl_flags = instance->ctrl_flags;
	ctrl_flags &= ~msg->u.control_req.mask;
	ctrl_flags |= (msg->u.control_req.flags & msg->u.control_req.mask);

	/* If clock control is being disabled, reset just the PID filter- the
	 * timestamps will still be processed. */
	if (((instance->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) != 0) &&
	    ((ctrl_flags & SYNC_MODULE_CLOCK_CTRL) == 0)) {
		sfptpd_pid_filter_reset(&instance->pid_filter);
	}

	/* If timestamp processing is being disabled, reset the whole servo. */
	if (((instance->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) != 0) &&
	    ((ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0)) {
		/* Reset the timestamp. Leave everything else alone as
		 * typically this is used as a temporary measure e.g. when
		 * stepping the clocks. */
		sfptpd_time_zero(&instance->pps_timestamp);
	}

	/* Record the new control flags */
	instance->ctrl_flags = ctrl_flags;

	SFPTPD_MSG_REPLY(msg);
}


static void pps_on_step_clock(pps_module_t *pps, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_pps_instance *instance;

	assert(pps != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_pps_instance *) msg->u.step_clock_req.instance_handle;
	assert(instance != NULL);

	/* Step the clock and reset the servo */
	(void)pps_servo_step_clock(pps, instance, &msg->u.step_clock_req.offset);

	SFPTPD_MSG_REPLY(msg);
}


static void pps_on_log_stats(pps_module_t *pps, sfptpd_sync_module_msg_t *msg)
{
	assert(pps != NULL);
	assert(msg != NULL);

	pps_send_rt_stats_update(pps, msg->u.log_stats_req.time);

	SFPTPD_MSG_FREE(msg);
}


static void pps_on_save_state(pps_module_t *pps, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_pps_instance *instance;
	char alarms[256], flags[256];

	assert(pps != NULL);
	assert(msg != NULL);

	for (instance = pps->instances; instance != NULL; instance = instance->next) {

		sfptpd_sync_module_alarms_text(instance->alarms, alarms, sizeof(alarms));
		sfptpd_sync_module_ctrl_flags_text(instance->ctrl_flags, flags, sizeof(flags));

		if (instance->state == SYNC_MODULE_STATE_SLAVE) {
			sfptpd_log_write_state(instance->clock,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				"instance: %s\n"
				"clock-name: %s\n"
				"clock-id: %s\n"
				"state: %s\n"
				"alarms: %s\n"
				"control-flags: %s\n"
				"interface: %s\n"
				"offset-from-master: " SFPTPD_FORMAT_FLOAT "\n"
				"freq-adjustment-ppb: " SFPTPD_FORMAT_FLOAT "\n"
				"in-sync: %d\n"
				"clustering-score: %d\n"
				"diff-method: %s\n"
				"pps-method: %s\n",
				SFPTPD_CONFIG_GET_NAME(instance->config),
				sfptpd_clock_get_long_name(instance->clock),
				sfptpd_clock_get_hw_id_string(instance->clock),
				pps_state_text(instance->state, instance->alarms),
				alarms, flags,
				instance->config->interface_name,
				instance->offset_from_master_ns,
				instance->freq_adjust_ppb,
				instance->synchronized,
				instance->clustering_score,
				sfptpd_clock_get_diff_method(instance->clock),
				sfptpd_clock_get_pps_method(instance->clock));
		} else {
			sfptpd_log_write_state(instance->clock,
				SFPTPD_CONFIG_GET_NAME(instance->config),
				"instance: %s\n"
				"clock-name: %s\n"
				"clock-id: %s\n"
				"state: %s\n"
				"alarms: %s\n"
				"control-flags: %s\n"
				"interface: %s\n"
				"freq-adjustment-ppb: " SFPTPD_FORMAT_FLOAT "\n",
				SFPTPD_CONFIG_GET_NAME(instance->config),
				sfptpd_clock_get_long_name(instance->clock),
				sfptpd_clock_get_hw_id_string(instance->clock),
				pps_state_text(instance->state, instance->alarms),
				alarms, flags,
				instance->config->interface_name,
				instance->freq_adjust_ppb);
		}

		/* If we consider the clock to be in sync, save the frequency adjustment */
		if (instance->synchronized &&
		    (instance->ctrl_flags & SYNC_MODULE_CLOCK_CTRL)) {
			(void)sfptpd_clock_save_freq_correction(instance->clock,
								instance->freq_adjust_ppb);
		}
	}

	SFPTPD_MSG_FREE(msg);
}


static void pps_on_write_topology(pps_module_t *pps, sfptpd_sync_module_msg_t *msg)
{
	FILE *stream;
	char alarms[256];
	struct sfptpd_pps_instance *instance;

	assert(pps != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_pps_instance *) msg->u.write_topology_req.instance_handle;
	stream = msg->u.write_topology_req.stream;

	assert(instance);
	assert(pps_is_instance_in_list(pps, instance));

	/* This should only be called on selected instances */
	assert(instance->ctrl_flags & SYNC_MODULE_SELECTED);

	fprintf(stream,
		"====================\n"
		"state: %s\n",
		pps_state_text(instance->state, instance->alarms));

	if (instance->alarms != 0) {
		sfptpd_sync_module_alarms_text(instance->alarms, alarms, sizeof(alarms));
		fprintf(stream, "alarms: %s\n", alarms);
	}

	fprintf(stream,
		"interface: %s\n"
		"timestamping: hw\n"
		"time-of-day: %s\n"
		"====================\n\n",
		instance->config->interface_name,
		pps->time_of_day.source.module ? pps->time_of_day.source.name : "none");

	sfptpd_log_topology_write_field(stream, true, "pps");

	switch (instance->state) {
	case SYNC_MODULE_STATE_LISTENING:
		sfptpd_log_topology_write_1to1_connector(stream, false, false, "?");
		break;

	case SYNC_MODULE_STATE_SLAVE:
		sfptpd_log_topology_write_1to1_connector(stream, false, true,
							 SFPTPD_FORMAT_TOPOLOGY_FLOAT,
							 instance->offset_from_master_ns);
		break;

	default:
		sfptpd_log_topology_write_1to1_connector(stream, false, false, "X");
		break;
	}

	sfptpd_log_topology_write_field(stream, true, sfptpd_clock_get_long_name(instance->clock));
	sfptpd_log_topology_write_field(stream, true, sfptpd_clock_get_hw_id_string(instance->clock));

	SFPTPD_MSG_REPLY(msg);
}


static void pps_on_stats_end_period(pps_module_t *pps,
				    sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_pps_instance *instance;

	assert(pps != NULL);
	assert(msg != NULL);

	for (instance = pps->instances; instance != NULL; instance = instance->next) {
		sfptpd_stats_collection_end_period(&instance->stats,
						   &msg->u.stats_end_period_req.time);

		/* Write the historical statistics to file */
		sfptpd_stats_collection_dump(&instance->stats, instance->clock,
									 SFPTPD_CONFIG_GET_NAME(instance->config));
	}

	SFPTPD_MSG_FREE(msg);
}


static void pps_on_test_mode(pps_module_t *pps, sfptpd_sync_module_msg_t *msg)
{
	struct sfptpd_pps_instance *instance;
	
	assert(pps != NULL);
	assert(msg != NULL);

	instance = (struct sfptpd_pps_instance *)msg->u.test_mode_req.instance_handle;
	assert(instance);
	assert(pps_is_instance_in_list(pps, instance));

	switch (msg->u.test_mode_req.id) {
	case SFPTPD_TEST_ID_BOGUS_PPS_EVENTS:
		/* Toggle on/off bogus PPS event generation */
		instance->test.bogus_pps_events = !instance->test.bogus_pps_events;
		NOTICE("pps %s: test-mode bogus pps events: %sabled\n",
		       SFPTPD_CONFIG_GET_NAME(instance->config),
		       instance->test.bogus_pps_events? "en": "dis");
		break;

	default:
		break;
	}

	SFPTPD_MSG_FREE(msg);
}


static int pps_start_instance(pps_module_t *pps,
			      struct sfptpd_pps_instance *instance) {
	int rc = 0;
	struct sfptpd_pps_module_config *config;

	config = instance->config;
	assert(config != NULL);

	/* Initial control flags. All instances start de-selected and with
	 * clock control disabled but with timestamp processing enabled. */
	instance->ctrl_flags = SYNC_MODULE_CTRL_FLAGS_DEFAULT;

	/* Initialize the PPS pulse check variables. */
	instance->instance_has_started = false;
	instance->pps_pulse_check_timer_expired = false;

	/* Initialise the sync module convergence and stats */
	pps_convergence_init(pps, instance);

	rc = pps_stats_init(pps, instance);
	if (rc != 0) {
		CRITICAL("pps %s: failed to create PPS stats\n",
			 SFPTPD_CONFIG_GET_NAME(config));
		goto fail;
	}

	/* Initialise the FIR and PID filters */
	sfptpd_notch_filter_init(&instance->notch_filter,
				 PPS_NOTCH_FILTER_MID_POINT,
				 PPS_NOTCH_FILTER_WIDTH);

	sfptpd_fir_filter_init(&instance->fir_filter, config->fir_filter_size);

	sfptpd_pid_filter_init(&instance->pid_filter,
			       config->pid_filter.kp, config->pid_filter.ki,
			       0.0, 1.0);

	/* Create the Peirce outlier filter */
	if (config->outlier_filter.enabled) {
		instance->outlier_filter =
			sfptpd_peirce_filter_create(config->outlier_filter.size,
						    config->outlier_filter.adaption);
		if (instance->outlier_filter == NULL) {
			CRITICAL("pps %s: failed to create outlier filter\n",
				 SFPTPD_CONFIG_GET_NAME(instance->config));
			goto fail;
		}
	}

	/* Determine and configure the clock */
	rc = pps_configure_clock(pps, instance, config);
	if (rc != 0) {
		CRITICAL("pps %s: failed to configure local reference clock\n",
			 SFPTPD_CONFIG_GET_NAME(instance->config));
		goto fail;
	}

	/* Initialise the state machine, the clock servo and the shared state */
	pps_state_machine_reset(pps, instance);
	pps_servo_reset(pps, instance);

	/* Reset PPS statistics */
	sfptpd_stats_reset_pps_statistics(sfptpd_clock_get_primary_interface(instance->clock));

 fail:
	return rc;
}


static void pps_on_run(pps_module_t *pps)
{
	struct sfptpd_pps_instance *instance;
	struct sfptpd_timespec interval;
	int rc;

	assert(pps->timers_started == false);

	sfptpd_time_from_ns(&interval, PPS_POLL_INTERVAL_NS);

	/* If PPS event retrieval blocks then:
	   1. record fd for use with epoll()
	   2. drain any queued events now
	 */

	for (instance = pps->instances; instance; instance = instance->next) {
		instance->poll_fd = sfptpd_clock_pps_get_fd(instance->clock);
		if (instance->poll_fd != -1) {
			pps_drain_events(pps, instance);
			rc = sfptpd_thread_user_fd_add(instance->poll_fd, true, false);
		}
	}


	rc = sfptpd_thread_timer_start(PPS_POLL_TIMER_ID,
				       true, false, &interval);
	if (rc != 0) {
		CRITICAL("pps: failed to start poll timer, %s\n", strerror(rc));

		/* We can't carry on in this case */
		sfptpd_thread_exit(rc);
	}

	pps->timers_started = true;
}


static void on_servo_pid_adjust(pps_module_t *pps,
				sfptpd_servo_msg_t *msg)
{
	struct sfptpd_pps_instance *instance;

	assert(pps != NULL);
	assert(msg != NULL);

	if (!(msg->u.pid_adjust.servo_type_mask & SFPTPD_SERVO_TYPE_PPS))
		return;

	for (instance = pps->instances; instance; instance = instance->next) {
		sfptpd_pid_filter_adjust(&instance->pid_filter,
					 msg->u.pid_adjust.kp,
					 msg->u.pid_adjust.ki,
					 msg->u.pid_adjust.kd,
					 msg->u.pid_adjust.reset);

		TRACE_L4("%s: adjust pid filter\n",
			SFPTPD_CONFIG_GET_NAME(instance->config));
	}

	SFPTPD_MSG_FREE(msg);
}


static int pps_on_startup(void *context)
{
	pps_module_t *pps = (pps_module_t *)context;
	struct sfptpd_pps_instance *instance;
	int rc;

	assert(pps != NULL);

	rc = sfptpd_multicast_subscribe(SFPTPD_SERVO_MSG_PID_ADJUST);
	if (rc != 0) {
		CRITICAL("failed to subscribe to servo message multicasts, %s\n",
			 strerror(rc));
		return rc;
	}

	for (instance = pps->instances; instance; instance = instance->next) {
		rc = pps_start_instance(pps, instance);
		if (rc != 0) goto fail;
	}

	/* Create a timer which will be used to poll for PPS events */
	rc = sfptpd_thread_timer_create(PPS_POLL_TIMER_ID, CLOCK_MONOTONIC,
					pps_on_timer, pps);
	if (rc != 0) {
		CRITICAL("pps: failed to create poll timer, %s\n", strerror(rc));
		goto fail;
	}

	/* Initialise the time of day support */
	rc = pps_time_of_day_init(pps);
	if (rc != 0 && rc != ENOENT)
		goto fail;

	return 0;

fail:
	pps_destroy_instances(pps);
	return rc;
}


static void pps_on_shutdown(void *context)
{
	pps_module_t *pps = (pps_module_t *)context;
	assert(pps != NULL);

	sfptpd_multicast_unsubscribe(SFPTPD_SERVO_MSG_PID_ADJUST);

	pps_destroy_instances(pps);

	/* Delete the sync module instance */
	free(pps);
}


static void pps_on_message(void *context, struct sfptpd_msg_hdr *hdr)
{
	pps_module_t *pps = (pps_module_t *)context;
	sfptpd_sync_module_msg_t *msg = (sfptpd_sync_module_msg_t *)hdr;

	assert(pps != NULL);
	assert(msg != NULL);

	switch (SFPTPD_MSG_GET_ID(msg)) {
	case SFPTPD_APP_MSG_RUN:
		pps_on_run(pps);
		SFPTPD_MSG_FREE(msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_GET_STATUS:
		pps_on_get_status(pps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_CONTROL:
		pps_on_control(pps, msg);
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
		pps_on_step_clock(pps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_LOG_STATS:
		pps_on_log_stats(pps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_SAVE_STATE:
		pps_on_save_state(pps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_WRITE_TOPOLOGY:
		pps_on_write_topology(pps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_STATS_END_PERIOD:
		pps_on_stats_end_period(pps, msg);
		break;

	case SFPTPD_SYNC_MODULE_MSG_TEST_MODE:
		pps_on_test_mode(pps, msg);
		break;

	case SFPTPD_SERVO_MSG_PID_ADJUST:
		on_servo_pid_adjust(pps, (sfptpd_servo_msg_t *) msg);
		break;

	default:
		WARNING("pps: received unexpected message, id %d\n",
			sfptpd_msg_get_id(hdr));
		SFPTPD_MSG_FREE(msg);
	}
}


static const struct sfptpd_thread_ops pps_thread_ops = 
{
	pps_on_startup,
	pps_on_shutdown,
	pps_on_message,
	pps_on_user_fds
};


static void pps_config_destroy(struct sfptpd_config_section *section)
{
	assert(section != NULL);
	assert(section->category == SFPTPD_CONFIG_CATEGORY_PPS);
	free(section);
}


static struct sfptpd_config_section *pps_config_create(const char *name,
						       enum sfptpd_config_scope scope,
						       bool allows_instances,
						       const struct sfptpd_config_section *src)
{
	struct sfptpd_pps_module_config *new;

	assert((src == NULL) || (src->category == SFPTPD_CONFIG_CATEGORY_PPS));

	new = (struct sfptpd_pps_module_config *)calloc(1, sizeof(*new));
	if (new == NULL) {
		ERROR("pps %s: failed to allocate memory for PPS configuration\n", name);
		return NULL;
	}

	/* If the source isn't null, copy the section contents. Otherwise,
	 * initialise with the default values. */
	if (src != NULL) {
		memcpy(new, src, sizeof(*new));
	} else {
		/* Set default values for PPS configuration */
		new->interface_name[0] = '\0';
		new->priority = SFPTPD_DEFAULT_PRIORITY;
		new->convergence_threshold = 0.0;
		new->master_clock_class = SFPTPD_PPS_DEFAULT_CLOCK_CLASS;
		new->master_time_source = SFPTPD_PPS_DEFAULT_TIME_SOURCE;
		new->master_accuracy = SFPTPD_PPS_DEFAULT_ACCURACY;
		new->master_time_traceable = SFPTPD_PPS_DEFAULT_TIME_TRACEABLE;
		new->master_freq_traceable = SFPTPD_PPS_DEFAULT_FREQ_TRACEABLE;
		new->steps_removed = SFPTPD_PPS_DEFAULT_STEPS_REMOVED;
		new->propagation_delay = 0.0;

		new->pid_filter.kp = SFPTPD_PPS_DEFAULT_PID_FILTER_KP;
		new->pid_filter.ki = SFPTPD_PPS_DEFAULT_PID_FILTER_KI;
		new->outlier_filter.enabled = SFPTPD_PPS_DEFAULT_OUTLIER_FILTER_ENABLED;
		new->outlier_filter.size = SFPTPD_PPS_DEFAULT_OUTLIER_FILTER_SIZE;
		new->outlier_filter.adaption = SFPTPD_PPS_DEFAULT_OUTLIER_FILTER_ADAPTION;
		new->fir_filter_size = SFPTPD_PPS_DEFAULT_FIR_FILTER_SIZE;
	}

	SFPTPD_CONFIG_SECTION_INIT(new, pps_config_create, pps_config_destroy,
				   SFPTPD_CONFIG_CATEGORY_PPS,
				   scope, allows_instances, name);

	return &new->hdr;
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_pps_module_config_init(struct sfptpd_config *config)
{
	struct sfptpd_pps_module_config *new;
	assert(config != NULL);

	new = (struct sfptpd_pps_module_config *)
		pps_config_create(SFPTPD_PPS_MODULE_NAME,
				  SFPTPD_CONFIG_SCOPE_GLOBAL, true, NULL);
	if (new == NULL)
		return ENOMEM;

	/* Add the configuration */
	SFPTPD_CONFIG_SECTION_ADD(config, new);

	/* Register the configuration options */
	sfptpd_config_register_options(&pps_config_option_set);

	return 0;
}


struct sfptpd_pps_module_config *sfptpd_pps_module_get_config(struct sfptpd_config *config)
{
	return (struct sfptpd_pps_module_config *)
		sfptpd_config_category_global(config, SFPTPD_CONFIG_CATEGORY_PPS);
}


void sfptpd_pps_module_set_default_interface(struct sfptpd_config *config,
					     const char *interface_name)
{
	struct sfptpd_pps_module_config *pps;
	assert(config != NULL);
	assert(interface_name != NULL);

	pps = sfptpd_pps_module_get_config(config);
	assert(pps != NULL);

	sfptpd_strncpy(pps->interface_name, interface_name, sizeof(pps->interface_name));
}


sfptpd_time_t sfptpd_pps_module_config_get_propagation_delay(struct sfptpd_config *config,
							     struct sfptpd_clock *clock)
{
	struct sfptpd_config_section *s;
	struct sfptpd_pps_module_config *pps, *global;
	struct sfptpd_interface *interface;
	assert(config != NULL);
	assert(clock != NULL);

	/* Get the PPS global configuration and then search the configuration
	 * instances for a PPS instance using the same clock If we  find this,
	 * return the PPS propagation delay specified. Otherwise, return the
	 * PPS propagation delay specified in the global PPS configuration. */
	global = sfptpd_pps_module_get_config(config);
	assert(global != NULL);
	for (s = sfptpd_config_category_first_instance(config, SFPTPD_CONFIG_CATEGORY_PPS);
	     s != NULL;
	     s = sfptpd_config_category_next_instance(s)) {
		pps = (struct sfptpd_pps_module_config *)s;

		interface = sfptpd_interface_find_by_name(pps->interface_name);
		if ((interface != NULL) &&
		    (sfptpd_interface_get_clock(interface) == clock)) {
			return pps->propagation_delay;
		}
	}

	return global->propagation_delay;
}


int sfptpd_pps_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries,
			     const struct sfptpd_link_table *link_table,
			     bool *link_subscribers)
{
	pps_module_t *pps;
	struct sfptpd_pps_instance *instance;
	int rc;

	assert(config != NULL);
	assert(engine != NULL);
	assert(sync_module != NULL);

	TRACE_L3("pps: creating sync-module\n");

	*sync_module = NULL;
	pps = (pps_module_t *)calloc(1, sizeof(*pps));
	if (pps == NULL) {
		CRITICAL("pps: failed to allocate sync module memory\n");
		return ENOMEM;
	}

	/* Keep a handle to the sync engine */
	pps->engine = engine;

	/* Create all the sync instances */
	rc = pps_create_instances(config, pps);
	if (rc != 0) {
		goto fail;
	}

	/* Create the sync module thread- the thread start up routine will
	 * carry out the rest of the initialisation. */
	rc = sfptpd_thread_create("pps", &pps_thread_ops, pps, sync_module);
	if (rc != 0) {
		free(pps);
		return rc;
	}

	/* If a buffer has been provided, populate the instance information */
	if (instances_info_buffer != NULL) {
		memset(instances_info_buffer, 0,
		       instances_info_entries * sizeof(*instances_info_buffer));

		for (instance = pps->instances;
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

 fail:
	free(pps);
	return rc;
}


/* fin */
