/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_servo.c
 * @brief  Servo module for synchronizing local clocks
 */

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <math.h>
#include <assert.h>

#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_servo.h"
#include "sfptpd_clock.h"
#include "sfptpd_constants.h"
#include "sfptpd_time.h"
#include "sfptpd_statistics.h"
#include "sfptpd_filter.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_engine.h"


/****************************************************************************
 * Types, Structures & Defines
 ****************************************************************************/

struct sfptpd_servo {
	/* Instance name, used for logging. */
	char servo_name[SFPTPD_CONFIG_SECTION_NAME_MAX];

	/* Handles of master and slave clocks */
	struct sfptpd_clock *master;
	struct sfptpd_clock *slave;

	/* Clock control configuration */
	enum sfptpd_clock_ctrl clock_ctrl;

	/* Epoch guard configuration */
	enum sfptpd_epoch_guard_config epoch_guard;

	/* FIR filter used to filter the raw clock deltas */
	sfptpd_fir_filter_t fir_filter;

	/* PID filter used to calculate the frequency corrections */
	sfptpd_pid_filter_t pid_filter;

	/* Historical frequency correction in parts-per-billion. Used as
	 * a constant adjustment in addition to the dynamic frequency 
	 * adjustment */
	long double freq_correction_ppb;

	/* Calculated frequency adjustment in parts-per-billion. */
	long double freq_adjust_ppb;

	/* Maximum frequency adjustment supported by slave clock */
	long double freq_adjust_max;

	/* Calculated offset from master in ns */
	long double offset_from_master_ns;

	/* Boolean indicating that servo synchronize operation has been
	 * executed at least once. Used to limit clock stepping to first
	 * update if required. */
	bool active;

        /* Boolean indicating that servo synchronize operation has been
	 * executed after LRC lock. Used to limit clock stepping to first
	 * lock for step-on-first-lock. See SWPTP-1032 for more info. */
        bool stepped_after_lrc_locked;

	/* Boolean indicating whether we consider the slave clock to be
	 * synchonized to the master */
	bool synchronized;

	/* State of synchronisation failure tracking */
	enum {
		STATE_OK = 0,
		STATE_FAILED,
		STATE_ALARMED,
	} state;

	/* Time of first sync failure in run */
	struct timespec sync_failures_begin;

	/* Alarms */
	sfptpd_sync_module_alarms_t alarms;

	/* Convergence measure */
	struct sfptpd_stats_convergence convergence;
};



/****************************************************************************
 * Constants
 ****************************************************************************/



/****************************************************************************
 * Local Functions
 ****************************************************************************/


/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct sfptpd_servo *sfptpd_servo_create(struct sfptpd_config *config, int idx)
{
	struct sfptpd_servo *servo;
	unsigned int stiffness;
	struct sfptpd_config_general *general_config = sfptpd_general_config_get(config);
	long double threshold;

	servo = (struct sfptpd_servo *)calloc(1, sizeof(*servo));
	if (servo == NULL) {
		ERROR("failed to allocate memory for clock servo. Exiting...\n");
		return NULL;
	}

	/* This is used mainly to identify the servo in logging output. */
	snprintf(servo->servo_name, SFPTPD_CONFIG_SECTION_NAME_MAX,
			 "servo%d", idx);

	servo->master = NULL;
	servo->slave = NULL;

	servo->clock_ctrl = general_config->clocks.control;
	servo->epoch_guard = general_config->epoch_guard;

	/* Work out the FIR stiffness to use based on how often we will
	 * synchronize the clocks. For large sync intervals we effectively
	 * don't use the FIR filter (stiffness = 1). */
	stiffness = (unsigned int)powl(2, (long double)-general_config->clocks.sync_interval);
	if (stiffness < SFPTPD_SERVO_FILTER_STIFFNESS_MIN)
		stiffness = SFPTPD_SERVO_FILTER_STIFFNESS_MIN;
	else if (stiffness > SFPTPD_SERVO_FILTER_STIFFNESS_MAX)
		stiffness = SFPTPD_SERVO_FILTER_STIFFNESS_MAX;

	/* Initialise the PID and FIR filters */
	sfptpd_fir_filter_init(&servo->fir_filter, stiffness);
	sfptpd_pid_filter_init(&servo->pid_filter,
			       general_config->pid_filter.kp,
			       general_config->pid_filter.ki,
			       SFPTPD_DEFAULT_SERVO_K_DIFFERENTIAL,
			       powl(2, (long double)general_config->clocks.sync_interval));

	/* Constant correction to frequency */
	servo->freq_correction_ppb = 0.0;

	/* Clear active flag */
	servo->active = false;

	/* Clear been_locked flag */
	servo->stepped_after_lrc_locked = false;

	/* Clear alarms */
	servo->alarms = 0;

	/* Initialise the convergence measure */
	servo->synchronized = false;
	sfptpd_stats_convergence_init(&servo->convergence);

	/* Sets an appropriate convergence threshold.
	   Check if overriden by user. */
	threshold = general_config->convergence_threshold;

	/* Otherwise use the default */
	if (threshold == 0) {
		threshold = SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT;
	}

	sfptpd_stats_convergence_set_max_offset(&servo->convergence, threshold);

	/* Reset the servo filters */
	sfptpd_servo_reset(servo);

	TRACE_L4("%s: created successfully\n", servo->servo_name);
	return servo;
}


void sfptpd_servo_destroy(struct sfptpd_servo *servo)
{
	assert(servo != NULL);

	/* Free the servo */
	free(servo);
}


void sfptpd_servo_reset(struct sfptpd_servo *servo)
{
	assert(servo != NULL);

	sfptpd_fir_filter_reset(&servo->fir_filter);
	sfptpd_pid_filter_reset(&servo->pid_filter);

	servo->freq_adjust_ppb = servo->freq_correction_ppb;
	servo->offset_from_master_ns = 0.0;

	TRACE_L4("%s: reset filters\n", servo->servo_name);
}


void sfptpd_servo_set_clocks(struct sfptpd_servo *servo, struct sfptpd_clock *master, struct sfptpd_clock *slave)
{
	int rc;
	assert(servo != NULL);
	assert(master != NULL);
	assert(slave != NULL);

	if ((servo->master != master) || (servo->slave != slave)) {
		servo->master = master;
		servo->slave = slave;

		/* Use the current frequency correction of the new slave clock.
		 * This will have been loaded at startup or will be 0 if
		 * clock correction off. In the case of a bond failover this
		 * will be the last good value that was saved. Also get the 
		 * maximum frequency adjustment supported by the clock */
		servo->freq_correction_ppb = sfptpd_clock_get_freq_correction(slave);
		servo->freq_adjust_max = sfptpd_clock_get_max_frequency_adjustment(servo->slave);
		
		/* Configure the PID filter max integral term to match the max frequency
		 * adjust of the slave clock */
		sfptpd_pid_filter_set_i_term_max(&servo->pid_filter, servo->freq_adjust_max);

		/* Set the clock frequency to the default value */
		rc = sfptpd_clock_adjust_frequency(servo->slave,
						   servo->freq_correction_ppb);
		if (rc != 0) {
			SYNC_MODULE_ALARM_SET(servo->alarms, CLOCK_CTRL_FAILURE);
			WARNING("%s: failed to adjust frequency of clock %s, error %s\n",
				servo->servo_name, sfptpd_clock_get_long_name(servo->slave),
				strerror(rc));
		} else {
			SYNC_MODULE_ALARM_CLEAR(servo->alarms, CLOCK_CTRL_FAILURE);
		}

		/* Reset the servo filters */
		sfptpd_servo_reset(servo);

		TRACE_L2("%s: set clocks to master %s, slave %s\n",
			 servo->servo_name, sfptpd_clock_get_short_name(master),
			 sfptpd_clock_get_short_name(slave));
	}
}


int sfptpd_servo_step_clock(struct sfptpd_servo *servo, struct timespec *offset)
{
	int rc;
	struct timespec zero = {.tv_sec = 0, .tv_nsec = 0};
	
	assert(servo != NULL);
	assert(servo->slave != NULL);
	assert(offset != NULL);

	/* We actually need to step the clock backwards by the specified offset */
	sfptpd_time_subtract(offset, &zero, offset);
    
	/* Step the slave clock by the specified offset */
	rc = sfptpd_clock_adjust_time(servo->slave, offset);
	if (rc != 0) {
		SYNC_MODULE_ALARM_SET(servo->alarms, CLOCK_CTRL_FAILURE);
		WARNING("%s: failed to adjust offset of clock %s, error %s\n",
			servo->servo_name,
			sfptpd_clock_get_long_name(servo->slave), strerror(rc));
	} else {
		SYNC_MODULE_ALARM_CLEAR(servo->alarms, CLOCK_CTRL_FAILURE);
	}

	/* Update the frequency correction for the slave clock */
	servo->freq_correction_ppb = sfptpd_clock_get_freq_correction(servo->slave);
    
	/* Set the clock frequency back to the last good value. */
	rc = sfptpd_clock_adjust_frequency(servo->slave, servo->freq_correction_ppb);
	if (rc != 0) {
		SYNC_MODULE_ALARM_SET(servo->alarms, CLOCK_CTRL_FAILURE);
		WARNING("%s: failed to adjust frequency of clock %s, error %s\n",
			servo->servo_name,
			sfptpd_clock_get_long_name(servo->slave), strerror(rc));
	}

	/* Reset the filters and calculated adjustments */
	sfptpd_servo_reset(servo);

	return rc;
}


static int do_servo_synchronize(struct sfptpd_engine *engine, struct sfptpd_servo *servo, struct timespec *time)
{
	struct timespec diff;
	long double mean, diff_ns;
	int rc;

	assert(servo != NULL);
	assert(servo->master != NULL);
	assert(servo->slave != NULL);
	assert(time != NULL);

	/* Get the time difference between the two clocks */
	rc = sfptpd_clock_compare(servo->slave, servo->master, &diff);
	if (rc != 0) {
		TRACE_L4("%s: failed to compare clocks %s and %s, error %s\n",
			 servo->servo_name,
			 sfptpd_clock_get_short_name(servo->slave),
			 sfptpd_clock_get_short_name(servo->master), strerror(rc));
		return rc;
	}

	/* If the difference is greater than the limit, step the clock. */
	diff_ns = sfptpd_time_timespec_to_float_ns(&diff);

	TRACE_L6("%s: difference between master and slave = " SFPTPD_FORMAT_FLOAT "\n",
		 servo->servo_name, diff_ns);

	/* Check to see if the NIC time is less than 115 days since the epoch.
	 * If so then the NIC has reset. In this case we need to raise an alarm. */
	struct timespec curtime;
	rc = sfptpd_clock_get_time(servo->master, &curtime);
	if (rc != 0) {
		TRACE_L4("%s: failed to get time from clock %s, error %s\n",
			servo->servo_name,
			sfptpd_clock_get_short_name(servo->master), strerror(rc));
		return rc;
	}
	long double curtime_ns = sfptpd_time_timespec_to_float_ns(&curtime);
	TRACE_L6("%s: reference clock timestamp in ns: " SFPTPD_FORMAT_FLOAT "\n",
		servo->servo_name, curtime_ns);
	if (curtime_ns < 1e16 || curtime_ns > (0xFFFC0000 * 1e9)) {
		if (!SYNC_MODULE_ALARM_TEST(servo->alarms, CLOCK_NEAR_EPOCH)) {
			SYNC_MODULE_ALARM_SET(servo->alarms, CLOCK_NEAR_EPOCH);

			if (servo->epoch_guard != SFPTPD_EPOCH_GUARD_CORRECT_CLOCK) {
				servo->offset_from_master_ns = diff_ns;
			}

			sfptpd_engine_post_rt_stats_simple(engine, servo);

			sfptpd_clock_stats_record_epoch_alarm(servo->slave, 1);
			WARNING("%s: reference clock %s near epoch\n", servo->servo_name,
				sfptpd_clock_get_long_name(servo->master));
		}
		/* Mark the servo as active, just to be safe */
		servo->active = true;
		/* if epoch_guard is set to alarm only then skip this section */
		if (servo->epoch_guard != SFPTPD_EPOCH_GUARD_ALARM_ONLY) {
			if (servo->epoch_guard == SFPTPD_EPOCH_GUARD_PREVENT_SYNC) {
				servo->offset_from_master_ns = diff_ns;
				return EAGAIN;
			}
			else if (servo->epoch_guard == SFPTPD_EPOCH_GUARD_CORRECT_CLOCK) {
				/* set the NIC clock to system clock */
				WARNING("%s: correcting master clock %s to system time\n", servo->servo_name,
					sfptpd_clock_get_long_name(servo->master));
				sfptpd_clock_correct_new(servo->master);
				/* After we set the LRC's time to system time, return early so that we don't propagate the error */
				return EAGAIN;
			}
		}
	} else {
		/* Once the reference clock time is no longer near epoch, we can
		 * clear the alarm. */
		SYNC_MODULE_ALARM_CLEAR(servo->alarms, CLOCK_NEAR_EPOCH);
	}

	/* Check to see if the slave NIC's time is near epoch.
	 * If so then the NIC has reset. In this case we just print a warning. */
        struct timespec slavetime;
	rc = sfptpd_clock_get_time(servo->slave, &slavetime);
	long double slavetime_ns = sfptpd_time_timespec_to_float_ns(&slavetime);
	if (slavetime_ns < 1e16 || slavetime_ns > (0xFFFC0000 * 1e9)) {
		if (!SYNC_MODULE_ALARM_TEST(servo->alarms, CLOCK_NEAR_EPOCH)) {
			SYNC_MODULE_ALARM_SET(servo->alarms, CLOCK_NEAR_EPOCH);

			sfptpd_engine_post_rt_stats_simple(engine, servo);

			sfptpd_clock_stats_record_epoch_alarm(servo->slave, 1);
			WARNING("%s: slave clock %s near epoch\n", servo->servo_name,
				sfptpd_clock_get_long_name(servo->slave));
		}
		/* Mark the servo as active, just to be safe */
		servo->active = true;
		/* if epoch_guard is not set to correct clock then don't */
		if (servo->epoch_guard == SFPTPD_EPOCH_GUARD_CORRECT_CLOCK) {
                        /* set the NIC clock to system clock */
                        WARNING("%s: correcting slave clock %s to system time\n", servo->servo_name,
                                sfptpd_clock_get_long_name(servo->slave));
                        sfptpd_clock_correct_new(servo->slave);
                        /* After we set the slave clock's time to system time, 
                         * return early so that we don't propagate the error */
                        return EAGAIN;
		}
	} else {
		/* Once the slave clock time is no longer near epoch, we can
		 * clear the flag. */
		SYNC_MODULE_ALARM_CLEAR(servo->alarms, CLOCK_NEAR_EPOCH);
	}

	/* If clock stepping is enabled and the difference between the master
	 * and slave clocks is larger than the step threshold then step the 
	 * clock */
        /* Explanation of the step-on-first-lock setting (SWPTP-1032):
           There are 2 scenarios:
                1. lrc_been_locked is true BEFORE sfptpd_servo_synchronize
                        is first called. This is highly unlikely. 
                2. lrc_been_locked is false when sfptpd_servo_synchronize
                        is first called. This is the typical scenario. 
           In the first scenario, the LRC has already been stepped to the GM by
           the time this function is called. This means we don't need to step the
           slave a second time. This case is handled by having this function 
           automatically set the stepped_after_lrc_locked flag to true when this
           function is called and lrc_been_locked is true. When the lrc_been_locked
           flag is true, this function will not step the slave. 
           In the second scenario, the LRC has NOT been stepped to the GM by the
           time this function is first called. This means we DO need to step-or-slew
           the slave AGAIN when the LRC is stepped. As long as the lrc_been_locked 
           flag is false, the stepped_after_lrc_locked flag will not be set to true. 
           So the stepped_after_lrc_locked flag will remain false until the 
           lrc_been_locked flag becomes true. And the only time lrc_been_locked 
           becomes true is when the LRC is stepped. At the point where the LRC is 
           stepped, immediately afterwards the lrc_been_locked flag will be set to 
           true. After that, when this function is called again, the lrc_been_locked 
           flag will be true and the stepped_after_lrc_locked flag will be false. In 
           that case the slave will be slew-or-stepped to the LRC for the second time, 
           and the stepped_after_lrc_locked flag will become true, meaning the slave 
           will not be stepped after it was slew-or-stepped for the second time. 
        */
	if ((servo->clock_ctrl == SFPTPD_CLOCK_CTRL_SLEW_AND_STEP) ||
	    ((servo->clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_AT_STARTUP || 
                servo->clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_ON_FIRST_LOCK) && !servo->active) || 
	    ((servo->clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_ON_FIRST_LOCK) && 
                !servo->stepped_after_lrc_locked && sfptpd_clock_get_been_locked(servo->master)) || 
            ((servo->clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_FORWARD) && (diff_ns < 0))) {
		if ((diff_ns <= -SFPTPD_SERVO_CLOCK_STEP_THRESHOLD) ||
		    (diff_ns >= SFPTPD_SERVO_CLOCK_STEP_THRESHOLD)) {
			/* Step the clock and return */
			rc = sfptpd_servo_step_clock(servo, &diff);

			/* Mark the servo as active */
                        servo->active = true;
                        if (sfptpd_clock_get_been_locked(servo->master)){
                                servo->stepped_after_lrc_locked = true;
                        }
			return rc;
		}
	}

	/* Add the new sample to the filter and get back the filtered delta */
	mean = sfptpd_fir_filter_update(&servo->fir_filter, diff_ns);

	TRACE_L6("%s, mean difference = %0.3Lf\n", servo->servo_name, mean);

	/* Store the filtered offset from master */
	servo->offset_from_master_ns = mean;

	/* Update the PID filter and get back the frequency adjustment */
	servo->freq_adjust_ppb = servo->freq_correction_ppb
		+ sfptpd_pid_filter_update(&servo->pid_filter, mean, time);

	/* Saturate the frequency adjustment */
	if (servo->freq_adjust_ppb > servo->freq_adjust_max)
		servo->freq_adjust_ppb = servo->freq_adjust_max;
	else if (servo->freq_adjust_ppb < -servo->freq_adjust_max)
		servo->freq_adjust_ppb = -servo->freq_adjust_max;

	/* Adjust the clock frequency using the calculated adjustment */
	rc = sfptpd_clock_adjust_frequency(servo->slave, servo->freq_adjust_ppb);
	if (rc != 0) {
		SYNC_MODULE_ALARM_SET(servo->alarms, CLOCK_CTRL_FAILURE);
		WARNING("%s: failed to adjust clock %s, error %s\n", servo->servo_name,
			sfptpd_clock_get_long_name(servo->slave), strerror(rc));
	} else {
		SYNC_MODULE_ALARM_CLEAR(servo->alarms, CLOCK_CTRL_FAILURE);
	}

	/* Update the convergence measure */
	servo->synchronized = sfptpd_stats_convergence_update(&servo->convergence,
							      time->tv_sec, mean);

	/* Log offset and synchronized stats with clock object */
	sfptpd_clock_stats_record_offset(servo->slave, mean, servo->synchronized);

	/* Mark the servo as active */
	servo->active = true;
        if (sfptpd_clock_get_been_locked(servo->master)){
                servo->stepped_after_lrc_locked = true;
        }

	TRACE_L5("%s, clock %s: ofm = %0.3Lf (%0.3Lf), freq-adj = %0.3Lf, "
		 "in-sync = %d, p = %0.3Lf, i = %0.3Lf\n",
		 servo->servo_name, sfptpd_clock_get_short_name(servo->slave),
		 mean, diff_ns, servo->freq_adjust_ppb, servo->synchronized,
		 sfptpd_pid_filter_get_p_term(&servo->pid_filter),
		 sfptpd_pid_filter_get_i_term(&servo->pid_filter));


	return rc;
}


int sfptpd_servo_synchronize(struct sfptpd_engine *engine, struct sfptpd_servo *servo, struct timespec *time)
{
	struct timespec elapsed;
	struct timespec now;
	sfptpd_time_t elapsed_s;
	int rc;

	rc = do_servo_synchronize(engine, servo, time);

	switch (servo->state) {
	case STATE_OK:
		if (rc != 0) {
			servo->state = STATE_FAILED;
			clock_gettime(CLOCK_MONOTONIC, &servo->sync_failures_begin);
		}
		break;
	case STATE_FAILED:
		if (rc == 0) {
			servo->state = STATE_OK;
		} else {
			clock_gettime(CLOCK_MONOTONIC, &now);
			sfptpd_time_subtract(&elapsed, &now, &servo->sync_failures_begin);
			elapsed_s = sfptpd_time_timespec_to_float_s(&elapsed);

	                if (elapsed_s >= SFPTPD_SUSTAINED_SYNC_FAILURE_PERIOD) {

				/* Raise the alarm */
				servo->state = STATE_ALARMED;
				SYNC_MODULE_ALARM_SET(servo->alarms, SUSTAINED_SYNC_FAILURE);

				/* Update the convergence measure */
				servo->synchronized = false;

				/* Log invalid offset and synchronized stats with clock object */
				sfptpd_clock_stats_record_offset(servo->slave, 0.0, servo->synchronized);
			}
		}
		break;
	case STATE_ALARMED:
		if (rc == 0) {
			servo->state = STATE_OK;
			SYNC_MODULE_ALARM_CLEAR(servo->alarms, SUSTAINED_SYNC_FAILURE);
		}
		break;
	}

	return rc;
}

void sfptpd_servo_get_offset_from_master(struct sfptpd_servo *servo, struct timespec *offset)
{
	assert(servo != NULL);
	assert(offset != NULL);

	sfptpd_time_float_ns_to_timespec(servo->offset_from_master_ns, offset);
}


struct sfptpd_servo_stats sfptpd_servo_get_stats(struct sfptpd_servo *servo)
{
	struct sfptpd_servo_stats stats;
	stats.servo_name = servo->servo_name;
	stats.clock_master = servo->master;
	stats.clock_slave = servo->slave;
	stats.disciplining = sfptpd_clock_is_writable(servo->slave);
	stats.blocked = sfptpd_clock_is_blocked(servo->slave);
	stats.offset = servo->offset_from_master_ns;
	stats.freq_adj = servo->freq_adjust_ppb;
	stats.in_sync = servo->synchronized;
	stats.alarms = servo->alarms;
	stats.p_term = sfptpd_pid_filter_get_p_term(&servo->pid_filter);
	stats.i_term = sfptpd_pid_filter_get_i_term(&servo->pid_filter);
	return stats;
}


void sfptpd_servo_update_sync_status(struct sfptpd_servo *servo)
{
	struct sfptpd_clock *clock;
	/* Update the NIC with the current sync status. If the slave clock is
	 * system clock, update the NIC clock that is the master to this. If the
	 * slave clock is a NIC clock, just update the NIC clock */
	if (servo->slave == sfptpd_clock_get_system_clock())
		clock = servo->master;
	else
		clock = servo->slave;

	sfptpd_clock_set_sync_status(clock, servo->synchronized,
				     SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT);
}


void sfptpd_servo_save_state(struct sfptpd_servo *servo)
{
	char alarms[256];

	assert(servo != NULL);
	assert(servo->master != NULL);
	assert(servo->slave != NULL);

	sfptpd_sync_module_alarms_text(servo->alarms, alarms, sizeof(alarms));

	sfptpd_log_write_state(servo->slave, NULL,
			       "clock-name: %s\n"
			       "clock-id: %s\n"
			       "state: local-slave\n"
			       "alarms: %s\n"
			       "reference-clock-name: %s\n"
			       "reference-clock-id: %s\n"
			       "offset-from-reference: " SFPTPD_FORMAT_FLOAT "\n"
			       "freq-adjustment-ppb: " SFPTPD_FORMAT_FLOAT "\n"
			       "in-sync: %d\n"
			       "p-term: " SFPTPD_FORMAT_FLOAT "\n"
			       "i-term: " SFPTPD_FORMAT_FLOAT "\n"
			       "diff-method: %s/%s\n",
			       sfptpd_clock_get_long_name(servo->slave),
			       sfptpd_clock_get_hw_id_string(servo->slave),
			       alarms,
			       sfptpd_clock_get_long_name(servo->master),
			       sfptpd_clock_get_hw_id_string(servo->master),
			       servo->offset_from_master_ns,
			       servo->freq_adjust_ppb,
			       servo->synchronized,
			       sfptpd_pid_filter_get_p_term(&servo->pid_filter),
			       sfptpd_pid_filter_get_i_term(&servo->pid_filter),
			       sfptpd_clock_get_diff_method(servo->slave),
			       sfptpd_clock_get_diff_method(servo->master));
	
	/* If the clock is currently considered to be in sync, save the 
	 * frequency adjustment */
	if (servo->synchronized) {
		sfptpd_clock_save_freq_correction(servo->slave, servo->freq_adjust_ppb);
	}
}


void sfptpd_servo_stats_end_period(struct sfptpd_servo *servo,
				   struct timespec *time)
{
	assert(servo != NULL);
	assert(servo->slave != NULL);
	assert(time != NULL);

	sfptpd_clock_stats_end_period(servo->slave, time);
}


void sfptpd_servo_write_topology_offset(struct sfptpd_servo *servo,
					FILE *stream)
{
	assert(servo != NULL);
	assert(servo->slave != NULL);
	assert(stream != NULL);

	sfptpd_log_topology_write_field(stream, false, SFPTPD_FORMAT_TOPOLOGY_FLOAT,
					servo->offset_from_master_ns);
}


void sfptpd_servo_write_topology_clock_name(struct sfptpd_servo *servo,
					    FILE *stream)
{
	assert(servo != NULL);
	assert(servo->slave != NULL);
	assert(stream != NULL);

	sfptpd_log_topology_write_field(stream, false,
					sfptpd_clock_get_long_name(servo->slave));
}


void sfptpd_servo_write_topology_clock_hw_id(struct sfptpd_servo *servo,
					     FILE *stream)
{
	assert(servo != NULL);
	assert(servo->slave != NULL);
	assert(stream != NULL);

	sfptpd_log_topology_write_field(stream, false,
					sfptpd_clock_get_hw_id_string(servo->slave));
}

sfptpd_sync_module_alarms_t sfptpd_servo_get_alarms(struct sfptpd_servo *servo,
						    const char **servo_name)
{
	assert(servo != NULL);

	if (servo_name)
		*servo_name = servo->servo_name;

	return servo->alarms;
}

/* fin */
