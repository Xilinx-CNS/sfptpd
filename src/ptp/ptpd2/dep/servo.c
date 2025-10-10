/*-
 * Copyright (c) 2019      Xilinx, Inc.
 * Copyright (c) 2014-2018 Solarflare Communications Inc.
 * Copyright (c) 2013      Harlan Stenn,
 *                         George N. Neville-Neil,
 *                         Wojciech Owczarek
 *                         Solarflare Communications Inc.
 * Copyright (c) 2011-2012 George V. Neville-Neil,
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Wojciech Owczarek,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen,
 *                         Inaqui Delgado,
 *                         Rick Ratzel,
 *                         National Instruments.
 *                         Solarflare Communications Inc.
 * Copyright (c) 2009-2010 George V. Neville-Neil, 
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen
 *
 * Copyright (c) 2005-2008 Kendall Correll, Aidan Williams
 *
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   servo.c
 * @date   Tue Jul 20 16:19:19 2010
 *
 * @brief  Code which implements the clock servo in software.
 *
 *
 */

#include "../ptpd.h"
#include "sfptpd_engine.h"

#define SERVO_MAGIC	(0x53525630)	/* SRV0 */

static void servo_adjust_frequency(ptp_servo_t *servo, LongDouble adj);
static void servo_reset_filters(ptp_servo_t *servo);
static void servo_reset_freq_adjustment(ptp_servo_t *servo);


bool
servo_init(const RunTimeOpts *rtOpts, ptp_servo_t *servo, struct sfptpd_clock *clock)
{
	bool rc = false;
	time_t filter_timeout;

	assert(rtOpts != NULL);
	assert(servo != NULL);

	DBG("servo_init()\n");

	/* On first call don't know whether the servo structure was initialised to
	 * zero so use marker to check whether it is safe to reference pointers. */
	if (servo->magic == SERVO_MAGIC) {
		/* Must clear down any existing information first - filters allocate
		 * memory so simply clearing servo will leak memory. */
		servo_shutdown (servo);
	}

	memset(servo, 0, sizeof(*servo));

	/* Now valid */
	servo->magic = SERVO_MAGIC;

	/* Take copy of configuration required */
	servo->ctrl_flags = SYNC_MODULE_CTRL_FLAGS_DEFAULT;
	servo->clock_ctrl = rtOpts->clock_ctrl;
	servo->step_threshold = rtOpts->step_threshold;
	servo->clock_first_updated = FALSE;
	servo->critical_stats_logger = (struct ptpd_critical_stats_logger *) &rtOpts->criticalStatsLogger;
        servo->clustering_evaluator = (struct sfptpd_clustering_evaluator *)  &rtOpts->clusteringEvaluator;

	/* Filter timeout based on sync interval */
	filter_timeout = (time_t) (rtOpts->path_delay_filter_size * powl(2, (long double)rtOpts->syncInterval));
	// TODO This doesn't really work. We don't find out the sync interval until
	// we choose a master.
	if (filter_timeout < SFPTPD_SMALLEST_FILTER_TIMEOUT_MIN)
		filter_timeout = SFPTPD_SMALLEST_FILTER_TIMEOUT_MIN;
	else if (filter_timeout > SFPTPD_SMALLEST_FILTER_TIMEOUT_MAX)
		filter_timeout = SFPTPD_SMALLEST_FILTER_TIMEOUT_MAX;

	/* Create the filters */
	servo->smallest_filt = sfptpd_smallest_filter_create(rtOpts->path_delay_filter_size,
							     rtOpts->path_delay_filter_ageing,
							     filter_timeout);
	if (NULL == servo->smallest_filt) {
		WARNING("ptp %s: failed to allocate smallest filter\n", rtOpts->name);
		goto exit;
	}

	servo->peirce_filt = sfptpd_peirce_filter_create(rtOpts->outlier_filter_size,
							 rtOpts->outlier_filter_adaption,
							 rtOpts->outlier_filter_drift);
	if (NULL == servo->peirce_filt) {
		WARNING("ptp %s: failed to allocate peirce filter\n", rtOpts->name);
		goto exit;
	}

	/* Initialise the FIR filter */
	sfptpd_fir_filter_init(&servo->fir_filter, rtOpts->fir_filter_size);

	/* Initialise PID filter */
	sfptpd_pid_filter_init(&servo->pid_filter,
			       rtOpts->servoKP, rtOpts->servoKI, rtOpts->servoKD,
			       powl(2, (long double)rtOpts->syncInterval));

	/* Set the slave clock. This will reset the frequency adjustment
	 * and configure the PID filter appropriately. */
	if (clock == NULL) {
		WARNING("servo: no clock specified\n");
		rc = false;
	} else {
		servo_set_slave_clock(servo, clock);
		rc = true;
	}

	/* Reset the servo including all the filters */
	servo_reset(servo);
	rc = true;

exit:
	return rc;
}

void
servo_shutdown(ptp_servo_t *servo)
{
	assert(servo != NULL);
	assert(servo->magic == SERVO_MAGIC);

	if (NULL != servo->smallest_filt) {
		sfptpd_smallest_filter_destroy(servo->smallest_filt);
		servo->smallest_filt = NULL;
	}

	if (NULL != servo->peirce_filt) {
		sfptpd_peirce_filter_destroy(servo->peirce_filt);
		servo->peirce_filt = NULL;
	}
}

sfptpd_sync_module_alarms_t
servo_get_alarms(ptp_servo_t *servo)
{
	return servo->alarms;
}

void
servo_reset(ptp_servo_t *servo)
{
	assert(servo != NULL);

	sfptpd_ptp_tsd_init(&servo->timestamps);
	servo_reset_filters(servo);
	servo_reset_freq_adjustment(servo);
	servo_reset_operator_messages(servo);
	servo->alarms = 0;
}


void
servo_reset_operator_messages(ptp_servo_t *servo)
{
	assert(servo != NULL);

	servo->warned_operator_slow_slewing = 0;
	servo->warned_operator_fast_slewing = 0;
}


void
servo_reset_filters(ptp_servo_t *servo)
{
	assert(servo != NULL);
	assert(servo->peirce_filt != NULL);
	assert(servo->smallest_filt != NULL);

	sfptpd_peirce_filter_reset(servo->peirce_filt);
	sfptpd_fir_filter_reset(&servo->fir_filter);
	sfptpd_pid_filter_reset(&servo->pid_filter);

	servo->offset_from_master = 0.0;
	servo->mean_path_delay = 0.0;

	sfptpd_smallest_filter_reset(servo->smallest_filt);
}


void
servo_pid_adjust(const RunTimeOpts *rtOpts, ptp_servo_t *servo, bool reset)
{
	assert(rtOpts != NULL);
	assert(servo != NULL);

	DBG("servo_pid_adjust()\n");

	assert(servo->magic == SERVO_MAGIC);

	sfptpd_pid_filter_adjust(&servo->pid_filter,
				 rtOpts->servoKP, rtOpts->servoKI, rtOpts->servoKD,
				 reset);

	if (reset)
		sfptpd_pid_filter_reset(&servo->pid_filter);
}


static void
servo_reset_freq_adjustment(ptp_servo_t *servo)
{
	assert(servo != NULL);

	servo->frequency_correction = sfptpd_clock_get_freq_correction(servo->clock);
	sfptpd_pid_filter_reset(&servo->pid_filter);

	/* Set the frequency adjustment to the saved value */
	servo_adjust_frequency(servo, servo->frequency_correction);
	servo->frequency_adjustment = servo->frequency_correction;
}


void servo_set_slave_clock(ptp_servo_t *servo,  struct sfptpd_clock *clock)
{
	LongDouble max_adj;

	assert(servo != NULL);
	assert(clock != NULL);

	servo->clock = clock;

	/* We are using a new clock so we need to clear the frequency adjustment */
	servo_reset_freq_adjustment(servo);

	/* Set the maximum frequency adjustment based on the clock characteristics */
	max_adj = sfptpd_clock_get_max_frequency_adjustment(clock);
	sfptpd_pid_filter_set_i_term_max(&servo->pid_filter, max_adj);
}


void servo_set_interval(ptp_servo_t *servo, long double interval)
{
	assert(servo != NULL);
	sfptpd_pid_filter_set_interval(&servo->pid_filter, interval);
}


sfptpd_time_t servo_get_offset_from_master(ptp_servo_t *servo)
{
	assert(servo != NULL);
	return servo->offset_from_master;
}


struct sfptpd_timespec servo_get_time_of_last_offset(ptp_servo_t *servo)
{
	assert(servo != NULL);
	return sfptpd_ptp_tsd_get_protocol_time(&servo->timestamps);
}


sfptpd_time_t servo_get_mean_path_delay(ptp_servo_t *servo)
{
	assert(servo != NULL);
	return servo->mean_path_delay;
}


long double servo_get_frequency_adjustment(ptp_servo_t *servo)
{
	assert(servo != NULL);
	return servo->frequency_adjustment;
}


long double servo_get_p_term(ptp_servo_t *servo)
{
	assert(servo != NULL);
	return sfptpd_pid_filter_get_p_term(&servo->pid_filter);
}


long double servo_get_i_term(ptp_servo_t *servo)
{
	assert(servo != NULL);
	return sfptpd_pid_filter_get_i_term(&servo->pid_filter);
}


long double servo_get_outlier_threshold(ptp_servo_t *servo)
{
	assert(servo != NULL);
	if (servo->peirce_filt->num_samples == 0){
		return 0.0;
	}
	long double sd = sfptpd_stats_std_dev_get(&servo->peirce_filt->std_dev, NULL);
	int criterion = peirce_filter_get_criterion(servo->peirce_filt->num_samples);
	return sd * criterion;
}


void servo_reset_counters(ptp_servo_t *servo)
{
	assert(servo != NULL);
	memset(&servo->counters, 0, sizeof(servo->counters));
}


void servo_get_counters(ptp_servo_t *servo, ptp_servo_counters_t *counters)
{
	assert(servo != NULL);
	assert(counters != NULL);
	*counters = servo->counters;
}


static bool servo_update(ptp_servo_t *servo)
{
	sfptpd_ptp_tsd_t *filtered_delay;
	sfptpd_time_t offset;
	int outlier;
 
	assert(servo != NULL);

	assert(servo->smallest_filt != NULL);
	assert(servo->timestamps.complete);

	DBGV("servo_update()\n");

	filtered_delay = sfptpd_smallest_filter_update(servo->smallest_filt, &servo->timestamps);

	offset = sfptpd_ptp_tsd_get_offset_from_master(filtered_delay);

	/* Get monotonic timestamp for the Peirce filter */
	struct sfptpd_timespec timestamp = sfptpd_ptp_tsd_get_monotonic_time(&servo->timestamps);

	outlier = sfptpd_peirce_filter_update(servo->peirce_filt, offset,
					      servo->frequency_adjustment - servo->frequency_correction,
					      &timestamp);
	servo->counters.outliers_num_samples += 1;
	if (outlier) {
		/* We have an outlier so don't update the offset from master or
		 * mean path delay. */
		DBGV("discarding " SFPTPD_FORMAT_FLOAT " as outlier\n", offset);
		servo->counters.outliers += 1;
		return false;
	}

	/* Execute the FIR filter to smooth the offset */
	offset = sfptpd_fir_filter_update(&servo->fir_filter, offset);

	/* Store the offset and corresponding mean path delay */
	servo->offset_from_master = offset;
	servo->mean_path_delay = sfptpd_ptp_tsd_get_path_delay(filtered_delay);
	DBGV("offset filter " SFPTPD_FORMAT_FLOAT "\n", offset);
	DBGV("mean path delay " SFPTPD_FORMAT_FLOAT "\n", servo->mean_path_delay);

	return true;
}


void servo_missing_s2m_ts(ptp_servo_t *servo)
{
	assert(servo != NULL);
	sfptpd_ptp_tsd_clear_s2m(&servo->timestamps);
}


void servo_missing_p2p_ts(ptp_servo_t *servo)
{
	assert(servo != NULL);
	sfptpd_ptp_tsd_clear_p2p(&servo->timestamps);
}


void servo_missing_m2s_ts(ptp_servo_t *servo)
{
	assert(servo != NULL);
	sfptpd_ptp_tsd_clear_m2s(&servo->timestamps);
}


bool servo_provide_s2m_ts(ptp_servo_t *servo,
			  struct sfptpd_timespec *send_time,
			  struct sfptpd_timespec *recv_time,
			  struct sfptpd_timespec *correction)
{
	assert(servo != NULL);
	assert(send_time != NULL);
	assert(recv_time != NULL);

	/* If timestamp processing is disabled, return immediately. */
	if ((servo->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0) {
		servo_missing_s2m_ts(servo);
		return false;
	}

	if (!sfptpd_ptp_tsd_set_s2m(&servo->timestamps, send_time,
				    recv_time, correction)) {
		return false;
	}

	return servo_update(servo);
}


bool servo_provide_p2p_ts(ptp_servo_t *servo,
			  struct sfptpd_timespec *req_send_time,
			  struct sfptpd_timespec *req_recv_time,
			  struct sfptpd_timespec *resp_send_time,
			  struct sfptpd_timespec *resp_recv_time,
			  struct sfptpd_timespec *correction)
{
	assert(servo != NULL);
	assert(req_send_time != NULL);
	assert(req_recv_time != NULL);
	assert(resp_send_time != NULL);
	assert(resp_recv_time != NULL);

	/* If timestamp processing is disabled, return immediately. */
	if ((servo->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0) {
		servo_missing_p2p_ts(servo);
		return false;
	}

	if (!sfptpd_ptp_tsd_set_p2p(&servo->timestamps, req_send_time,
				    req_recv_time, resp_send_time,
				    resp_recv_time, correction)) {
		return false;
	}

	return servo_update(servo);
}


bool servo_provide_m2s_ts(ptp_servo_t *servo,
			  struct sfptpd_timespec *send_time,
		          struct sfptpd_timespec *recv_time,
			  struct sfptpd_timespec *correction)
{
	assert(servo != NULL);
	assert(send_time != NULL);
	assert(recv_time != NULL);

	/* If timestamp processing is disabled, return immediately. */
	if ((servo->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0) {
		servo_missing_m2s_ts(servo);
		return false;
	}

	/* Add the timestamps to the timestamp dataset. If we have a complete
	 * set, continue the processing, otherwise return immediately. */
	if (!sfptpd_ptp_tsd_set_m2s(&servo->timestamps, send_time,
				    recv_time, correction)) {
		return false;
	}

	return servo_update(servo);
}


void servo_control(ptp_servo_t *servo,
		   sfptpd_sync_module_ctrl_flags_t ctrl_flags)
{
	assert(servo != NULL);

	/* If clock control is being disabled, reset just the PID filter- the
	 * timestamps will still be processed. */
	if (((servo->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) != 0) &&
	    ((ctrl_flags & SYNC_MODULE_CLOCK_CTRL) == 0)) {
		servo->frequency_correction = sfptpd_clock_get_freq_correction(servo->clock);
		sfptpd_pid_filter_reset(&servo->pid_filter);
	}

	/* If timestamp processing is being disabled, reset the whole servo. */
	if (((servo->ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) != 0) &&
	    ((ctrl_flags & SYNC_MODULE_TIMESTAMP_PROCESSING) == 0)) {
		/* Reset the timestamp set. Leave everything else alone as
		 * typically this is used as a temporary measure e.g. when
		 * stepping the clocks. */
		sfptpd_ptp_tsd_init(&servo->timestamps);
	}

	/* Record the new control flags */
	servo->ctrl_flags = ctrl_flags;
	
}


void servo_step_clock(ptp_servo_t *servo, struct sfptpd_timespec *offset)
{
	struct sfptpd_timespec o;

	assert(servo != NULL);
	assert(offset != NULL);

	if (servo->clock_ctrl == SFPTPD_CLOCK_CTRL_NO_ADJUST) {
		WARNING("clock step blocked - clock adjustment disabled\n");
		return;
	}

	/* Negate the offset such that we subtract rather that add it */
	sfptpd_time_negate(&o, offset);

	if (sfptpd_clock_adjust_time(servo->clock, &o) != 0)
		SYNC_MODULE_ALARM_SET(servo->alarms, CLOCK_CTRL_FAILURE);
	else
		SYNC_MODULE_ALARM_CLEAR(servo->alarms, CLOCK_CTRL_FAILURE);

	/* Record the fact that the time has been stepped */
	servo->counters.clock_steps++;

	/* Reset the timestamp set */
	sfptpd_ptp_tsd_init(&servo->timestamps);

	/* Reset the offset from master portion of the clock servo and the
	 * frequency adjustment back to last good value. Note that we do not
	 * reset the one-way-delay measurement as this should be the same
	 * after the step */
	servo_reset_filters(servo);

	/* Note that the correction doesn't get updated at runtime so this
	 * will be the default value. */
	servo_reset_freq_adjustment(servo);
}


static void warn_operator_fast_slewing(ptp_servo_t *servo, LongDouble adj)
{
	assert(servo != NULL);

	LongDouble max_adj = sfptpd_clock_get_max_frequency_adjustment(servo->clock);

	if ((adj >= max_adj) || (adj <= -max_adj)) {
		if (servo->warned_operator_fast_slewing == 0) {
			servo->warned_operator_fast_slewing = 1;
			NOTICE("slewing clock %s with the maximum frequency adjustment\n",
				   sfptpd_clock_get_short_name(servo->clock));
		}
	} else {
		servo->warned_operator_fast_slewing = 0;
	}
}


static void warn_operator_slow_slewing(ptp_servo_t *servo)
{
	assert(servo != NULL);

	if(servo->warned_operator_slow_slewing == 0){
		servo->warned_operator_slow_slewing = 1;
		servo->warned_operator_fast_slewing = 1;

		/* rule of thumb: our maximum slew rate is about 1ms/s */
		long double estimated = sfptpd_time_abs(servo->offset_from_master) / ONE_MILLION / 3600.0;

		/* we don't want to arrive early 1s in an expiration opening,
		 * so all consoles get a message when the time is 1s off. */
		WARNING(SFPTPD_FORMAT_FLOAT " seconds offset detected; will take %.1Lf hours to slew\n",
			sfptpd_time_abs(servo->offset_from_master) / ONE_BILLION, estimated);
	}
}


/*
 * this is a wrapper around adjTime to abstract extra operations
 */
static void servo_adjust_frequency(ptp_servo_t *servo, LongDouble adj)
{
	assert(servo != NULL);
	if (sfptpd_clock_adjust_frequency(servo->clock, adj) != 0)
		SYNC_MODULE_ALARM_SET(servo->alarms, CLOCK_CTRL_FAILURE);
	else
		SYNC_MODULE_ALARM_CLEAR(servo->alarms, CLOCK_CTRL_FAILURE);

	warn_operator_fast_slewing(servo, adj);
}


void servo_update_clock(ptp_servo_t *servo)
{
	LongDouble max_adj;
	struct sfptpd_timespec monotonic_time;
	struct ptpd_critical_stats stats;

	assert(servo != NULL);
	assert(servo->critical_stats_logger);
	assert(servo->critical_stats_logger->log_fn);
        assert(servo->clustering_evaluator);
        assert(servo->clustering_evaluator->calc_fn);
        assert(servo->clustering_evaluator->comp_fn);

	DBGV("==> updateClock\n");

	stats.ofm_ns = servo->offset_from_master;
	stats.owd_ns = servo->mean_path_delay;
	stats.sync_time = servo_get_time_of_last_offset(servo);
	stats.freq_adj = servo->frequency_adjustment;
	stats.valid = true;

	int current_clustering_score = servo->clustering_evaluator->calc_fn(
                servo->clustering_evaluator, servo->offset_from_master, servo->clock);

	if (servo->clustering_evaluator->comp_fn(servo->clustering_evaluator, current_clustering_score)) {
		if (!SYNC_MODULE_ALARM_TEST(servo->alarms, CLUSTERING_THRESHOLD_EXCEEDED)) {
			SYNC_MODULE_ALARM_SET(servo->alarms, CLUSTERING_THRESHOLD_EXCEEDED);
                        sfptpd_clock_stats_record_clustering_alarm(servo->clock, 1);
                        WARNING("ptp clustering guard: clock %s out of clustering threshold\n",
                                sfptpd_clock_get_long_name(servo->clock));
                }
		goto finish;
	} else {
		/* Once the sync module is no longer out of clustering threshold, we can
		 * clear the alarm. */
		SYNC_MODULE_ALARM_CLEAR(servo->alarms, CLUSTERING_THRESHOLD_EXCEEDED);
	}

	if (sfptpd_time_abs(servo->offset_from_master) >= servo->step_threshold) {
		/* If clock control is disabled, go no further! */
		if ((servo->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) == 0)
			goto finish;

		/* if secs, reset clock or set freq adjustment to max */

		/*
		  if offset from master seconds is non-zero, then this is a "big jump:
		  in time.  Check Run Time options to see if we will reset the clock or
		  set frequency adjustment to max to adjust the time
		*/

		if ((servo->clock_ctrl == SFPTPD_CLOCK_CTRL_SLEW_AND_STEP) ||
	            ((servo->clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_AT_STARTUP ||
                      servo->clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_ON_FIRST_LOCK) && !servo->clock_first_updated) || 
		    ((servo->clock_ctrl == SFPTPD_CLOCK_CTRL_STEP_FORWARD) && (servo->offset_from_master < 0))) {
			struct sfptpd_timespec step;
			sfptpd_time_float_ns_to_timespec(servo->offset_from_master, &step);
			servo_step_clock(servo, &step);

		} else if (servo->clock_ctrl != SFPTPD_CLOCK_CTRL_NO_ADJUST) {
			max_adj = sfptpd_clock_get_max_frequency_adjustment(servo->clock);

			servo->frequency_adjustment
				= (servo->offset_from_master < 0)? max_adj: -max_adj;
			warn_operator_slow_slewing(servo);
			servo_adjust_frequency(servo, servo->frequency_adjustment);

			/* Updated the value to be logged because it has been saturated */
			stats.freq_adj = servo->frequency_adjustment;
		}

		/* Mark the clock as having been updated. */
		servo->clock_first_updated = TRUE;
                sfptpd_clock_set_been_locked(servo->clock, TRUE);
	} else {
		/* Clear the slow slewing warning so that it will be re-issued
		 * if another large offset occurs */
		servo->warned_operator_slow_slewing = 0;

		/* Get the current monotonic time to perform the PID filter
		 * update. This ensures that the integral term is calculated
		 * correctly when in unicast mode. */
		(void)sfclock_gettime(CLOCK_MONOTONIC, &monotonic_time);

		/* If we are not currently controlling the clock, the frequency
		 * adjustment is the saved value. If we are controlling the clock then
		 * we apply the output of the PID filter to this value. */
		servo->frequency_adjustment = servo->frequency_correction;

		if (servo->ctrl_flags & SYNC_MODULE_CLOCK_CTRL) {
			/* Offset from master is less than one second. Use the
			 * PID filter to adjust the time */
			servo->frequency_adjustment
				+= sfptpd_pid_filter_update(&servo->pid_filter,
							    servo->offset_from_master,
							    &monotonic_time);

			max_adj = sfptpd_clock_get_max_frequency_adjustment(servo->clock);

			/* Clamp the adjustment to the min/max values */
			if (servo->frequency_adjustment > max_adj)
				servo->frequency_adjustment = max_adj;
			else if (servo->frequency_adjustment < -max_adj)
				servo->frequency_adjustment = -max_adj;

			DBG2("     After PI: Adj: " SFPTPD_FORMAT_FLOAT
			     "   Drift: " SFPTPD_FORMAT_FLOAT 
			     "   OFM " SFPTPD_FORMAT_FLOAT "\n",
			     servo->frequency_adjustment,
			     sfptpd_pid_filter_get_i_term(&servo->pid_filter),
			     servo->offset_from_master);

			servo_adjust_frequency(servo, servo->frequency_adjustment);

			/* Mark the clock as having been updated. */
			servo->clock_first_updated = TRUE;
                        sfptpd_clock_set_been_locked(servo->clock, TRUE);
		}
	}

 finish:

	/* Log the data instantly */
	servo->critical_stats_logger->log_fn(servo->critical_stats_logger, stats);
}
