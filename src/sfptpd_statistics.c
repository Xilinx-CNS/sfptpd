/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2021 Xilinx, Inc. */

/**
 * @file   sfptpd_statistics.c
 * @brief  Statistics calculation, weights and measures
 */

#include <time.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <limits.h>

#include "sfptpd_constants.h"
#include "sfptpd_logging.h"
#include "sfptpd_statistics.h"
#include "sfptpd_misc.h"
#include "sfptpd_interface.h"


/****************************************************************************
 * Types & Defines
 ****************************************************************************/

/* Size of initial array allocated for collection */
#define SFPTPD_STATS_COLLECTION_DEFAULT_SIZE (16)


struct sfptpd_stats_period_info
{
	const char *name;
	unsigned int length;
};

struct stats_range_history
{
	/* Base class member data */
	struct sfptpd_stats_item parent;

	/* Currently active statistics */
	sfptpd_stats_range_t active;

	/* Historical data for range measure */
	sfptpd_stats_range_t history[SFPTPD_STATS_PERIOD_MAX][SFPTPD_STATS_HISTORY_MAX];
};

struct stats_count_history
{
	/* Base class member data */
	struct sfptpd_stats_item parent;

	/* Currently active statistics */
	sfptpd_stats_count_t active;

	/* Historical data for count measure */
	sfptpd_stats_count_t history[SFPTPD_STATS_PERIOD_MAX][SFPTPD_STATS_HISTORY_MAX];
};


/****************************************************************************
 * Constants
 ****************************************************************************/

const struct sfptpd_stats_period_info sfptpd_stats_periods[SFPTPD_STATS_PERIOD_MAX] =
{
	{"minute",      60},
	{"ten-minutes", 60*10},
	{"hour",        60*60},
	{"day",         60*60*24},
	{"week",        60*60*24*7}
};

const struct timespec zero_time = { 0, 0 };

/* Forward declarations */
static const struct sfptpd_stats_item_ops stats_range_ops;
static const struct sfptpd_stats_item_ops stats_count_ops;

static const char *stats_range_format_string = "%-16s %22s %22s %22s %22s %14s %24s %24s %24s %24s %4s\n";
static const char *stats_range_format_data   = "%-16s %22.*Lf %22.*Lf %22.*Lf %22.*Lf %14d %24s %24s %24s %24s %4s\n";

static const char *stats_count_format_string = "%-16s %14s %14s %24s %24s\n";
static const char *stats_count_format_data   = "%-16s %14d %14d %24s %24s\n";


/****************************************************************************
 * Local Functions
 ****************************************************************************/


/****************************************************************************
 * Convergence Measure
 ****************************************************************************/

void sfptpd_stats_convergence_init(struct sfptpd_stats_convergence *conv)
{
	assert(conv != NULL);

	conv->min_period = SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT;
	conv->max_offset = SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT;

	sfptpd_stats_convergence_reset(conv);
}


void sfptpd_stats_convergence_set_min_period(struct sfptpd_stats_convergence *conv,
					     time_t min_period)
{
	assert(conv != NULL);
	conv->min_period = min_period;
}


void sfptpd_stats_convergence_set_max_offset(struct sfptpd_stats_convergence *conv,
					     long double max_offset_ns)
{
	assert(conv != NULL);
	assert(max_offset_ns > 0);
	conv->max_offset = max_offset_ns;
}


void sfptpd_stats_convergence_reset(struct sfptpd_stats_convergence *conv)
{
	assert(conv != NULL);
	
	conv->num_samples = 0;
	conv->start_time = 0;
	conv->latest_time = 0;
}


bool sfptpd_stats_convergence_update(struct sfptpd_stats_convergence *conv,
				     time_t time, long double offset_ns)
{
	assert(conv != NULL);
	
	/* Is the new sample outside the convergence criteria? */
	if ((offset_ns < -conv->max_offset) || 
	    (offset_ns > conv->max_offset)) {
		sfptpd_stats_convergence_reset(conv);
		return false;
	}

	/* Is something wrong like time has gone backwards? */
	if (time < conv->start_time) {
		WARNING("convergence detected time has gone backwards %ld -> %ld\n",
			conv->start_time, time);
		conv->num_samples = 1;
		conv->start_time = time;
		conv->latest_time = time;
		return false;
	}
	
	/* Is this the first sample below the threshold? */
	if (conv->num_samples == 0)
		conv->start_time = time;
	
	/* Update the data with the new sample */
	conv->num_samples++;
	conv->latest_time = time;
	
	return (time >= conv->start_time + conv->min_period);
}


/****************************************************************************
 * Standard Deviation Measure
 ****************************************************************************/

void sfptpd_stats_std_dev_init(struct sfptpd_stats_std_dev *std_dev)
{
	assert(std_dev != NULL);

	std_dev->num_samples = 0;
	std_dev->sum_data = 0.0;
	std_dev->sum_data_squared = 0.0;
}

void sfptpd_stats_std_dev_add_sample(struct sfptpd_stats_std_dev *std_dev,
				     long double sample)
{
	assert(std_dev != NULL);

	std_dev->num_samples++;
	std_dev->sum_data += sample;
	std_dev->sum_data_squared += (sample * sample);
}

void sfptpd_stats_std_dev_remove_sample(struct sfptpd_stats_std_dev *std_dev,
					long double sample)
{
	assert(std_dev != NULL);
	assert(std_dev->num_samples > 0);

	std_dev->num_samples--;
	std_dev->sum_data -= sample;
	std_dev->sum_data_squared -= (sample * sample);
}

long double sfptpd_stats_std_dev_get(struct sfptpd_stats_std_dev *std_dev,
				     long double *mean)
{
	long double m, sd_sqr;

	assert(std_dev != NULL);
	assert(std_dev->num_samples > 0);

	m = std_dev->sum_data / std_dev->num_samples;

	/* This uses the equivalence that the standard deviation is equal to
	 * the mean of the squares (of the data) - the square of the means */
	sd_sqr = (std_dev->sum_data_squared / std_dev->num_samples) - (m * m);

	if (mean != NULL)
		*mean = m;
	return sqrtl(sd_sqr);
}


/****************************************************************************
 * PPS Statistics
 ****************************************************************************/

int sfptpd_stats_get_pps_statistics(struct sfptpd_interface *interface,
				    struct sfptpd_stats_pps *pps_stats)
{
	int rc;
	uint64_t stats[SFPTPD_DRVSTAT_MAX];

	if (interface == NULL) {
		ERROR("stats: pps statistics requested without an interface\n");
		return EINVAL;
	}

	rc = sfptpd_interface_driver_stats_read(interface, stats);
	if (rc != 0)
		return rc;

	pps_stats->overflow_count = stats[SFPTPD_DRVSTAT_PPS_OFLOW];
	pps_stats->bad_period_count = stats[SFPTPD_DRVSTAT_PPS_BAD];
	pps_stats->offset.last = stats[SFPTPD_DRVSTAT_PPS_OFF_LAST];
	pps_stats->offset.mean = stats[SFPTPD_DRVSTAT_PPS_OFF_MEAN];
	pps_stats->offset.min = stats[SFPTPD_DRVSTAT_PPS_OFF_MIN];
	pps_stats->offset.max = stats[SFPTPD_DRVSTAT_PPS_OFF_MAX];
	pps_stats->period.last = stats[SFPTPD_DRVSTAT_PPS_PER_LAST];
	pps_stats->period.mean = stats[SFPTPD_DRVSTAT_PPS_PER_MEAN];
	pps_stats->period.min = stats[SFPTPD_DRVSTAT_PPS_PER_MIN];
	pps_stats->period.max = stats[SFPTPD_DRVSTAT_PPS_PER_MAX];

	return rc;
}


void sfptpd_stats_reset_pps_statistics(struct sfptpd_interface *interface)
{
	int rc;

	rc = sfptpd_interface_driver_stats_reset(interface);
	if (rc != 0) {
		ERROR("stats: reset failed, %s\n", strerror(rc));
	}
}


/****************************************************************************
 * General Measures
 ****************************************************************************/

void sfptpd_stats_range_init(struct sfptpd_stats_range *range)
{
	assert(range != NULL);
	range->valid = true;
	range->num_samples = 0;
	range->total = 0.0;
	range->total_squares = 0.0;
	range->min = 1.0e100;
	range->max = -1.0e100;
	range->qualified = true;
	range->min_time.tv_sec = 0;
	range->min_time.tv_nsec = 0;
	range->max_time.tv_sec = 0;
	range->max_time.tv_nsec = 0;
}


void sfptpd_stats_range_update(struct sfptpd_stats_range *range,
			       long double sample,
			       struct timespec time,
			       bool qualified)
{
	assert(range != NULL);
	if (qualified) {
		range->num_samples++;
		range->total += sample;
		range->total_squares += (sample * sample);
		if (sample < range->min) {
			range->min = sample;
			range->min_time = time;
		}
		if (sample > range->max) {
			range->max = sample;
			range->max_time = time;
		}
	} else {
		range->qualified = false;
	}
}


void sfptpd_stats_range_add(struct sfptpd_stats_range *dst,
			    struct sfptpd_stats_range *src)
{
	assert(dst != NULL);
	assert(src != NULL);
	dst->num_samples += src->num_samples;
	dst->total += src->total;
	dst->total_squares += src->total_squares;
	if (src->min < dst->min) {
		dst->min = src->min;
		dst->min_time = src->min_time;
	}
	if (src->max > dst->max) {
		dst->max = src->max;
		dst->max_time = src->max_time;
	}
	if (!src->qualified)
		dst->qualified = false;
}


void sfptpd_stats_count_init(struct sfptpd_stats_count *count)
{
	assert(count != NULL);
	count->valid = true;
	count->num_samples = 0;
	count->total = 0;
}


void sfptpd_stats_count_update(struct sfptpd_stats_count *count,
			       unsigned long sample,
			       unsigned long num_samples)
{
	assert(count != NULL);
	count->num_samples += num_samples;
	count->total += sample;
}


void sfptpd_stats_count_add(struct sfptpd_stats_count *dst,
			    struct sfptpd_stats_count *src)
{
	assert(dst != NULL);
	assert(src != NULL);
	dst->num_samples += src->num_samples;
	dst->total += src->total;
}


/****************************************************************************
 * Historical records of the range of a measurement augmented with times
 * for extreme events and a continuous qualification flag
 ****************************************************************************/

static sfptpd_stats_item_t *stats_range_history_alloc(const char *name,
						     const char *units,
						     unsigned int decimal_places)
{
	struct stats_range_history *stat;
	enum sfptpd_stats_time_period p;

	assert(name != NULL);

	stat = (struct stats_range_history *)calloc(1, sizeof(*stat));
	if (stat == NULL) {
		ERROR("stats: failed to allocate memory for range history %s\n",
		      name);
		return NULL;
	}

	stat->parent.type = SFPTPD_STATS_TYPE_RANGE;
	stat->parent.name = name;
	stat->parent.units = units;
	stat->parent.decimal_places = decimal_places;
	stat->parent.ops = &stats_range_ops;

	sfptpd_stats_range_init(&stat->active);

	/* Initialise the current statistics for each time period */
	for (p = 0; p < sizeof(stat->history)/sizeof(stat->history[0]); p++)
		sfptpd_stats_range_init(&stat->history[p][SFPTPD_STATS_HISTORY_CURRENT]);

	return &stat->parent;
}


static void stats_range_history_free(struct sfptpd_stats_item *item)
{
	assert(item != NULL);
	free(item);
}


static void stats_range_history_update(struct sfptpd_stats_item *item, va_list args)
{
	struct stats_range_history *stat = (struct stats_range_history *)item;
	long double sample;
	struct timespec time;
	int qualified;

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_RANGE);

	sample = va_arg(args, long double);
	time = va_arg(args, struct timespec);
	qualified = va_arg(args, int);
	sfptpd_stats_range_update(&stat->active, sample, time, (bool) qualified);
}


static void stats_range_history_end_period(struct sfptpd_stats_item *item,
					  enum sfptpd_stats_time_period period)
{
	struct stats_range_history *stat = (struct stats_range_history *)item;
	sfptpd_stats_range_t *entry;
	enum sfptpd_stats_time_period p;

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_RANGE);
	assert(period < sizeof(stat->history)/sizeof(stat->history[0]));

	if (period == 0) {
		for (p = 0; p < sizeof(stat->history)/sizeof(stat->history[0]); p++) {
			/* Get a pointer to the current statistics entry for this
			 * time period and add the active statistics to these */
			entry = &stat->history[p][SFPTPD_STATS_HISTORY_CURRENT];
			sfptpd_stats_range_add(entry, &stat->active);
		}

		/* Reset the active stats accumulator */
		sfptpd_stats_range_init(&stat->active);
	}

	/* Store the end time of the period that has ended and shift along the
	 * previous results. This causes us to discard the oldest data for this
	 * time period */
	memmove(&stat->history[period][SFPTPD_STATS_HISTORY_1],
		&stat->history[period][SFPTPD_STATS_HISTORY_CURRENT],
		sizeof(stat->history[0]) - sizeof(stat->history[0][0]));

	/* Reset the current stats for this period */
	sfptpd_stats_range_init(&stat->history[period][SFPTPD_STATS_HISTORY_CURRENT]);
}


static void stats_range_history_write_headings(struct sfptpd_stats_item *item,
					       FILE *stream)
{
	assert(stream != NULL);
	fprintf(stream, stats_range_format_string,
		"", "mean", "min", "max", "std-dev", "samples", "start-time", "end-time", "min-time", "max-time", "qual");
}


static void stats_range_history_write_data(struct sfptpd_stats_item *item,
					   FILE *stream, const char *name,
					   const char *start, const char *end,
					   enum sfptpd_stats_time_period period,
					   enum sfptpd_stats_history_index index)
{
	struct stats_range_history *stat = (struct stats_range_history *)item;
	sfptpd_stats_range_t *entry;
	char min_time_str[24];
	char max_time_str[24];

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_RANGE);
	assert(stream != NULL);

	entry = &stat->history[period][index];

	if (!entry->valid)
		return;

	/* If no data was collected, during the period, output null values and
	 * avoid a divide by 0 */
	if (entry->num_samples == 0) {
		fprintf(stream, stats_range_format_string,
			name, "---", "---", "---", "---", "0", start, end, "---", "---", "no");
	} else {
		/* This uses the equivalence that the standard deviation is equal to
		 * the mean of the squares (of the data) - the square of the means */
		long double mean = entry->total / entry->num_samples;
		long double sd_sqr = (entry->total_squares / entry->num_samples)
				   - (mean * mean);

		if (entry->min_time.tv_sec == 0) {
			snprintf(min_time_str, sizeof min_time_str, "---");
		} else {
			sfptpd_local_strftime(min_time_str, sizeof min_time_str,
					      "%Y-%m-%d %X", &entry->min_time.tv_sec);
		}

		if (entry->max_time.tv_sec == 0) {
			snprintf(max_time_str, sizeof max_time_str, "---");
		} else {
			sfptpd_local_strftime(max_time_str, sizeof max_time_str,
					      "%Y-%m-%d %X", &entry->max_time.tv_sec);
		}

		fprintf(stream, stats_range_format_data, name,
			stat->parent.decimal_places, mean,
			stat->parent.decimal_places, entry->min,
			stat->parent.decimal_places, entry->max,
			stat->parent.decimal_places, sqrtl(sd_sqr),
			entry->num_samples, start, end,
			min_time_str, max_time_str,
			entry->qualified ? "yes" : "no");
	}
}


static void stats_range_history_write_json_opening(
	struct sfptpd_stats_item *item,
	FILE *stream)
{
	assert(item != NULL);
	assert(item->name != NULL);
	assert(stream != NULL);

	fprintf(stream, "{\"name\":\"%s\"", item->name);
	if (item->units != NULL)
		fprintf(stream, ",\"units\":\"%s\"", item->units);
	fprintf(stream, ",\"type\":\"range\",\"values\":[");
}


static void stats_range_history_write_json_data(
	struct sfptpd_stats_item *item, FILE *stream,
	enum sfptpd_stats_time_period period, enum sfptpd_stats_history_index index,
	const char *period_name, int period_secs, int seq_num,
	const char *start, const char *end)
{
	struct stats_range_history *stat = (struct stats_range_history *)item;
	sfptpd_stats_range_t *entry;
	char time_str[24];

	assert(stat != NULL);
	assert(stat->history != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_RANGE);
	assert(stream != NULL);

	entry = &stat->history[period][index];

	fprintf(stream,
		"{\"period\":\"%s\",\"period-secs\":%d,\"seq-num\":%d,\"samples\":%lu",
		period_name, period_secs, seq_num, entry->num_samples);
	if (entry->num_samples == 0){
		fprintf(stream, ",\"end-time\":null}");
		return;
        }

	/* This uses the equivalence that the standard deviation is equal to
	 * the mean of the squares (of the data) - the square of the means */
	long double mean = entry->total / entry->num_samples;
	long double sd_sqr = (entry->total_squares / entry->num_samples)
				- (mean * mean);

	fprintf(stream,
			",\"mean\":%.*Lf,\"min\":%.*Lf,\"max\":%.*Lf,\"std-dev\":%.*Lf",
			stat->parent.decimal_places, mean,
			stat->parent.decimal_places, entry->min,
			stat->parent.decimal_places, entry->max,
			stat->parent.decimal_places, sqrtl(sd_sqr));

	fprintf(stream, ",\"start-time\":\"%s\"", start);

	if (strcmp(end, "---") == 0)
		fprintf(stream, ",\"end-time\":null");
	else
		fprintf(stream, ",\"end-time\":\"%s\"", end);

	if (entry->min_time.tv_sec == 0)
		snprintf(time_str, sizeof time_str, "null");
	else
		sfptpd_local_strftime(time_str, sizeof time_str,
				      "\"%Y-%m-%d %X\"", &entry->min_time.tv_sec);
	fprintf(stream, ",\"min-time\":%s", time_str);

	if (entry->max_time.tv_sec == 0)
		snprintf(time_str, sizeof time_str, "null");
	else
		sfptpd_local_strftime(time_str, sizeof time_str,
				      "\"%Y-%m-%d %X\"", &entry->max_time.tv_sec);
	fprintf(stream, ",\"max-time\":%s", time_str);

	fprintf(stream, ",\"qualified\":%s}", entry->qualified ? "true" : "false");
}


static void stats_range_history_write_json_closing(
	struct sfptpd_stats_item *item,
	FILE *stream)
{
	fputs("]}", stream);
}


static int stats_range_history_get(sfptpd_stats_item_t *item,
				  enum sfptpd_stats_time_period period,
				  enum sfptpd_stats_history_index index,
				  va_list args)
{
	struct stats_range_history *stat = (struct stats_range_history *)item;
	long double *mean, *max, *min;
	int *qualified;
	struct timespec *max_time, *min_time;

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_RANGE);
	assert(period < SFPTPD_STATS_PERIOD_MAX);
	assert(index < SFPTPD_STATS_HISTORY_MAX);

	sfptpd_stats_range_t *entry = &stat->history[period][index];
	if (!entry->valid)
		return ENOENT;

	mean = va_arg(args, long double *);
	max = va_arg(args, long double *);
	min = va_arg(args, long double *);
	qualified = va_arg(args, int *);
	max_time = va_arg(args, struct timespec *);
	min_time = va_arg(args, struct timespec *);
	if (mean != NULL) *mean = entry->total / entry->num_samples;
	if (max != NULL) *max = entry->max;
	if (min != NULL) *min = entry->min;
	if (qualified != NULL) *qualified = (int) entry->qualified;
	if (max_time != NULL) *max_time = entry->max_time;
	if (min_time != NULL) *min_time = entry->min_time;
	return 0;
}


static const struct sfptpd_stats_item_ops stats_range_ops =
{
	stats_range_history_free,
	stats_range_history_update,
	stats_range_history_end_period,
	stats_range_history_write_headings,
	stats_range_history_write_data,
	stats_range_history_write_json_opening,
	stats_range_history_write_json_data,
	stats_range_history_write_json_closing,
	stats_range_history_get
};


/****************************************************************************
 * Historical records of the frequency of an event
 ****************************************************************************/

static sfptpd_stats_item_t *stats_count_history_alloc(const char *name,
						      const char *units,
						      unsigned int decimal_places)
{
	struct stats_count_history *stat;
	enum sfptpd_stats_time_period p;

	assert(name != NULL);

	stat = (struct stats_count_history *)calloc(1, sizeof(*stat));
	if (stat == NULL) {
		ERROR("stats: failed to allocate memory for range history %s\n",
		      name);
		return NULL;
	}

	stat->parent.type = SFPTPD_STATS_TYPE_COUNT;
	stat->parent.name = name;
	stat->parent.units = units;
	stat->parent.decimal_places = decimal_places;
	stat->parent.ops = &stats_count_ops;

	sfptpd_stats_count_init(&stat->active);

	/* Initialise the active statistics for each time period */
	for (p = 0; p < sizeof(stat->history)/sizeof(stat->history[0]); p++) {
		sfptpd_stats_count_init(&stat->history[p][SFPTPD_STATS_HISTORY_CURRENT]);
	}

	return &stat->parent;
}


static void stats_count_history_free(struct sfptpd_stats_item *item)
{
	assert(item != NULL);
	free(item);
}


static void stats_count_history_update(sfptpd_stats_item_t *item, va_list args)
{
	struct stats_count_history *stat = (struct stats_count_history *)item;
	unsigned long sample;

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_COUNT);

	sample = va_arg(args, unsigned long);
	sfptpd_stats_count_update(&stat->active, sample, 1);
}


static void stats_count_history_end_period(sfptpd_stats_item_t *item,
					   enum sfptpd_stats_time_period period)
{
	struct stats_count_history *stat = (struct stats_count_history *)item;
	sfptpd_stats_count_t *entry;
	enum sfptpd_stats_time_period p;

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_COUNT);
	assert(period < sizeof(stat->history)/sizeof(stat->history[0]));

	if (period == 0) {
		for (p = 0; p < sizeof(stat->history)/sizeof(stat->history[0]); p++) {
			/* Get a pointer to the current statistics entry for this
			 * time period and add the active statistics to these */
			entry = &stat->history[p][SFPTPD_STATS_HISTORY_CURRENT];
			sfptpd_stats_count_add(entry, &stat->active);
		}

		/* Reset the active stats accumulator */
		sfptpd_stats_count_init(&stat->active);
	}

	/* Store the end time of the period that has ended and shift along the
	 * previous results. This causes us to discard the oldest data for this
	 * time period */
	memmove(&stat->history[period][SFPTPD_STATS_HISTORY_1],
		&stat->history[period][SFPTPD_STATS_HISTORY_CURRENT],
		sizeof(stat->history[0]) - sizeof(stat->history[0][0]));

	/* Reset the current stats for this period */
	sfptpd_stats_count_init(&stat->history[period][SFPTPD_STATS_HISTORY_CURRENT]);
}


static void stats_count_history_write_headings(struct sfptpd_stats_item *item,
					       FILE *stream)
{
	assert(stream != NULL);
	fprintf(stream, stats_count_format_string,
		"", "total", "samples", "start-time", "end-time");
}


static void stats_count_history_write_data(struct sfptpd_stats_item *item,
					   FILE *stream, const char *name,
					   const char *start, const char *end,
					   enum sfptpd_stats_time_period period,
					   enum sfptpd_stats_history_index index)
{
	struct stats_count_history *stat = (struct stats_count_history *)item;
	sfptpd_stats_count_t *entry;

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_COUNT);
	assert(stream != NULL);

	entry = &stat->history[period][index];

	if (!entry->valid)
		return;

	fprintf(stream, stats_count_format_data,
		name, entry->total, entry->num_samples, start, end);
}


static void stats_count_history_write_json_opening(
	struct sfptpd_stats_item *item,
	FILE *stream)
{
	assert(item != NULL);
	assert(item->name != NULL);
	assert(stream != NULL);

	fprintf(stream, "{\"name\":\"%s\"", item->name);
	if (item->units != NULL)
		fprintf(stream, ",\"units\":\"%s\"", item->units);
	fprintf(stream, ",\"type\":\"count\",\"values\":[");
}


static void stats_count_history_write_json_data(
	struct sfptpd_stats_item *item, FILE *stream,
	enum sfptpd_stats_time_period period, enum sfptpd_stats_history_index index,
	const char *period_name, int period_secs, int seq_num,
	const char *start, const char *end)
{
	sfptpd_stats_count_t *entry;
	struct stats_count_history *stat = (struct stats_count_history *)item;

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_COUNT);
	assert(stream != NULL);

	entry = &stat->history[period][index];
	if (!entry->valid)
		return;

	fprintf(stream, "{\"period\":\"%s\",\"period-secs\":%d,\"seq-num\":%d"
		",\"samples\":%lu,\"total\":%lu,\"start-time\":\"%s\"",
		period_name, period_secs, seq_num, entry->num_samples,
		entry->total, start);

	if (strcmp(end, "---") == 0)
		fprintf(stream, ",\"end-time\":null}");
	else
		fprintf(stream, ",\"end-time\":\"%s\"}", end);

}


static void stats_count_history_write_json_closing(
	struct sfptpd_stats_item *item,
	FILE *stream)
{
	fputs("]}", stream);
}


static int stats_count_history_get(sfptpd_stats_item_t *item,
				   enum sfptpd_stats_time_period period,
				   enum sfptpd_stats_history_index index,
				   va_list args)
{
	struct stats_count_history *stat = (struct stats_count_history *)item;
	unsigned long *count;

	assert(stat != NULL);
	assert(stat->parent.type == SFPTPD_STATS_TYPE_COUNT);
	assert(period < SFPTPD_STATS_PERIOD_MAX);
	assert(index < SFPTPD_STATS_HISTORY_MAX);

	sfptpd_stats_count_t *entry = &stat->history[period][index];
	if (!entry->valid)
		return ENOENT;

	count = va_arg(args, unsigned long *);
	assert(count != NULL);
	*count = entry->total;
	return 0;
}


static const struct sfptpd_stats_item_ops stats_count_ops =
{
	stats_count_history_free,
	stats_count_history_update,
	stats_count_history_end_period,
	stats_count_history_write_headings,
	stats_count_history_write_data,
	stats_count_history_write_json_opening,
	stats_count_history_write_json_data,
	stats_count_history_write_json_closing,
	stats_count_history_get
};


/****************************************************************************
 * Statistics collection
 ****************************************************************************/

int sfptpd_stats_collection_alloc(struct sfptpd_stats_collection *stats,
				  const char *name)
{
	enum sfptpd_stats_time_period p;
	struct timespec time;

	assert(stats != NULL);
	assert(name != NULL);

	/* We expect the stats collection interval to be 60 seconds */
	assert(SFPTPD_STATS_COLLECTION_INTERVAL == 60);

	/* Get the time and initialise the statistics measures */
	if (clock_gettime(CLOCK_REALTIME, &time) < 0) {
		ERROR("failed to get realtime time, %s\n", strerror(errno));
		return errno;
	}

	memset(stats, 0, sizeof(*stats));
	stats->name = name;

	for (p = 0; p < sizeof(stats->intervals)/sizeof(stats->intervals[0]); p++) {
		stats->elapsed[p] = 0;
		stats->intervals[p][SFPTPD_STATS_HISTORY_CURRENT].start_valid = true;
		stats->intervals[p][SFPTPD_STATS_HISTORY_CURRENT].start_time = time;
	}

	stats->capacity = 1 /*SFPTPD_STATS_COLLECTION_DEFAULT_SIZE;*/;
	stats->items = (sfptpd_stats_item_t **)calloc(stats->capacity, sizeof(stats->items));

	if (stats->items == NULL) {
		CRITICAL("stats %s: failed to allocate memory for collection\n", name);
		return ENOMEM;
	}

	return 0;
}


int sfptpd_stats_collection_create(struct sfptpd_stats_collection *stats,
				   const char *name,
				   unsigned int num_items,
				   const struct sfptpd_stats_collection_defn *definitions)
{
	int rc;
	unsigned int i;

	assert(num_items <= SFPTPD_STATS_COLLECTION_MAX_SIZE);
	assert(definitions != NULL);

	rc = sfptpd_stats_collection_alloc(stats, name);
	if (rc != 0)
		return rc;

	for (i = 0; (i < num_items) && (rc == 0); i++, definitions++) {
		rc = sfptpd_stats_collection_add(stats,
						 definitions->id,
						 definitions->type,
						 definitions->name,
						 definitions->units,
						 definitions->decimal_places);
	}

	if (rc != 0)
		sfptpd_stats_collection_free(stats);

	return rc;
}


void sfptpd_stats_collection_free(struct sfptpd_stats_collection *stats)
{
	unsigned int i;
	struct sfptpd_stats_item *item;

	assert(stats != NULL);

	/* Free each item in the collection */
	if (stats->items != NULL) {
		for (i = 0; i < stats->capacity; i++) {
			item = stats->items[i];
			if (item != NULL)
				item->ops->free(item);
		}

		/* Free the collection memory */
		free(stats->items);
	}

	stats->capacity = 0;
	stats->items = NULL;
}


int sfptpd_stats_collection_add(struct sfptpd_stats_collection *stats,
				unsigned int id, enum sfptpd_stats_type type,
				const char *name, const char *units,
				unsigned int decimal_places)
{
	struct sfptpd_stats_item *item;

	assert(stats != NULL);
	assert(type < SFPTPD_STATS_TYPE_MAX);
	assert(name != NULL);
	assert(id < SFPTPD_STATS_COLLECTION_MAX_SIZE);

	/* If the requested index is larger than our current allocation,
	 * allocate a new larger array */
	if (id >= stats->capacity) {
		unsigned int new_size;
		struct sfptpd_stats_item **new_items;

		/* Get the next size by rounding up to the next power of 2 */
		new_size = (1 << ((8 * sizeof(id)) - __builtin_clz(id)));
		TRACE_L6("stats %s: increasing size from %d to %d\n",
			 stats->name, stats->capacity, new_size);

		new_items = (struct sfptpd_stats_item **)calloc(new_size,
								sizeof(item));
		if (new_items == NULL) {
			CRITICAL("stats %s: failed to allocate memory for collection, index %d\n",
				 stats->name, id);
			return ENOMEM;
		}

		memcpy(new_items, stats->items, stats->capacity * sizeof(item));
		free(stats->items);
		stats->capacity = new_size;
		stats->items = new_items;
	}

	/* Stats should only be added at startup and in each collection all
	 * items must have a unique index */
	item = stats->items[id];
	if (item != NULL) {
		CRITICAL("stats %s: index %d already exists- %s\n",
			 stats->name, id, item->name);
		return EEXIST;
	}

	switch (type) {
	case SFPTPD_STATS_TYPE_RANGE:
		item = stats_range_history_alloc(name, units, decimal_places);
		break;

	case SFPTPD_STATS_TYPE_COUNT:
		item = stats_count_history_alloc(name, units, decimal_places);
		break;

	default:
		assert(false);
	}

	/* Finally insert the item into the table */
	stats->items[id] = item;
	TRACE_L6("stats %s: added item %d, type %d, name %s, units %s\n",
		 stats->name, id, type, name, (units != NULL)? units: "<none>");
	return 0;
}


static int stats_collection_type_check(struct sfptpd_stats_collection *stats,
				       unsigned int index,
				       enum sfptpd_stats_type type)
{
	struct sfptpd_stats_item *item;

	assert(stats != NULL);
	assert(index < SFPTPD_STATS_COLLECTION_MAX_SIZE);

	/* If caller supplies the 'max' value this means accept any type. */
	assert(type <= SFPTPD_STATS_TYPE_MAX);

	if (index >= stats->capacity) {
		ERROR("stats %s: index %d out of range for collection\n",
		      stats->name, index);
		return ERANGE;
	}

	item = stats->items[index];
	if (item == NULL) {
		ERROR("stats %s: index %d doesn't exist\n",
		      stats->name, index);
		return ENOENT;
	}

	if (type != SFPTPD_STATS_TYPE_MAX &&
	    item->type != type) {
		ERROR("stats %s: index %d expected type %d got type %d\n",
		      stats->name, index, type, item->type);
		return EINVAL;
	}

	assert(item->ops);

	return 0;
}


static int sfptpd_stats_collection_update(struct sfptpd_stats_collection *stats,
					  unsigned int index,
					  enum sfptpd_stats_type type, ...)
{
	struct sfptpd_stats_item *item;
	va_list args;
	int rc;

	va_start(args, type);

	rc = stats_collection_type_check(stats, index, type);
	if (rc == 0) {
		item = stats->items[index];
		assert(item->ops->update != NULL);

		/* Update the item */
		item->ops->update(item, args);
	}

	va_end(args);

	return rc;
}


int sfptpd_stats_collection_update_range(struct sfptpd_stats_collection *stats,
					 unsigned int index,
					 long double sample,
					 struct timespec time,
					 bool qualified)
{
	return sfptpd_stats_collection_update(stats, index,
					      SFPTPD_STATS_TYPE_RANGE, sample, time, (int) qualified);
}


int sfptpd_stats_collection_update_count(struct sfptpd_stats_collection *stats,
					 unsigned int index,
					 unsigned long sample)
{
	return sfptpd_stats_collection_update_count_samples(stats, index, sample, 1);
}


int sfptpd_stats_collection_update_count_samples(struct sfptpd_stats_collection *stats,
					 unsigned int index,
					 unsigned long sample,
					 unsigned long num_samples)
{
	return sfptpd_stats_collection_update(stats, index,
					      SFPTPD_STATS_TYPE_COUNT, sample, num_samples);
}



void sfptpd_stats_collection_end_period(struct sfptpd_stats_collection *stats,
					struct timespec *time)
{
	enum sfptpd_stats_time_period p;
	unsigned int i;
	struct sfptpd_stats_item *item;
	struct sfptpd_stats_time_interval *interval;

	assert(stats != NULL);
	assert(time != NULL);

	/* For each statistics time period... */
	for (p = 0; p < sizeof(stats->intervals)/sizeof(stats->intervals[0]); p++) {
		interval = &stats->intervals[p][SFPTPD_STATS_HISTORY_CURRENT];

		/* Update the seconds elapsed for this time period and check
		 * whether the time period has finished */
		stats->elapsed[p] += SFPTPD_STATS_COLLECTION_INTERVAL;
		if (stats->elapsed[p] >= sfptpd_stats_periods[p].length) {
			/* Store the end time of the period and mark the period
			 * as complete. Shift along the previous results for the
			 * period. This causes us to discard the oldest data
			 * for this time period */
			interval->end_valid = true;
			interval->end_time = *time;

			/* Free each item in the collection, call end period
			 * indicating which period has ended */
			for (i = 0; i < stats->capacity; i++) {
				item = stats->items[i];
				if (item != NULL)
					item->ops->end_period(item, p);
			}

			memmove(&stats->intervals[p][SFPTPD_STATS_HISTORY_1],
				&stats->intervals[p][SFPTPD_STATS_HISTORY_CURRENT],
				sizeof(stats->intervals[0]) - sizeof(stats->intervals[0][0]));

			/* Set up the next period */
			stats->elapsed[p] = 0;
			interval->seq_num++;
			interval->start_valid = true;
			interval->start_time = *time;
			interval->end_valid = false;
			interval->end_time = zero_time;
		}
	}
}


static int sfptpd_stats_collection_get(struct sfptpd_stats_collection *stats,
				       unsigned int index,
				       enum sfptpd_stats_time_period period,
				       enum sfptpd_stats_history_index instance, ...)
{
	struct sfptpd_stats_item *item;
	va_list ap;
	int rc;

	va_start(ap, instance);

	rc = stats_collection_type_check(stats, index, SFPTPD_STATS_TYPE_MAX);
	if (rc == 0) {
		item = stats->items[index];
		assert(item->ops->get != NULL);

		rc = item->ops->get(item, period, instance, ap);
	}

	va_end(ap);

	return rc;
}


int sfptpd_stats_collection_get_range(struct sfptpd_stats_collection *stats,
				      unsigned int index,
				      enum sfptpd_stats_time_period period,
				      enum sfptpd_stats_history_index instance,
				      long double *mean, long double *min,
				      long double *max, int *qualified,
				      struct timespec *min_time,
				      struct timespec *max_time)
{
	int rc;

	rc = stats_collection_type_check(stats, index, SFPTPD_STATS_TYPE_RANGE);
	if (rc != 0)
		return rc;

	return sfptpd_stats_collection_get(stats, index, period, instance,
					   mean, max, min, qualified, max_time, min_time);
}


int sfptpd_stats_collection_get_count(struct sfptpd_stats_collection *stats,
				      unsigned int index,
				      enum sfptpd_stats_time_period period,
				      enum sfptpd_stats_history_index instance,
				      unsigned long *count)
{
	int rc;

	rc = stats_collection_type_check(stats, index, SFPTPD_STATS_TYPE_COUNT);
	if (rc != 0)
		return rc;

	return sfptpd_stats_collection_get(stats, index, period, instance, count);
}


int sfptpd_stats_collection_get_interval(struct sfptpd_stats_collection *stats,
					 enum sfptpd_stats_time_period period,
					 enum sfptpd_stats_history_index instance,
					 struct sfptpd_stats_time_interval *interval)
{
	int rc;

	assert(period < SFPTPD_STATS_PERIOD_MAX);
	assert(instance < SFPTPD_STATS_HISTORY_MAX);

	rc = stats_collection_type_check(stats, 0, SFPTPD_STATS_TYPE_MAX);
	if (rc == 0) {
		*interval = stats->intervals[period][instance];
	}

	return rc;
}


void sfptpd_stats_collection_dump(struct sfptpd_stats_collection *stats,
								  struct sfptpd_clock *clock,
								  const char *sync_instance_name)
{
	unsigned int i;
	struct sfptpd_log *log, *log_json;
	FILE *stream, *stream_json;
	enum sfptpd_stats_time_period p;
	enum sfptpd_stats_history_index h;
	struct sfptpd_stats_item *item;
	struct sfptpd_stats_time_interval *interval;
	char name[16], start[24], end[24];
	bool first_entry = true;

	assert(stats != NULL);
	assert(clock != NULL);

	/* Open the logs and get their stream handles */
	log = sfptpd_log_open_statistics(clock, sync_instance_name);
	if (log == NULL) {
		return;
	}
	log_json = sfptpd_log_open_statistics_json(clock, sync_instance_name);
	if (log_json == NULL) {
		sfptpd_log_file_close(log);
		return;
	}
	stream = sfptpd_log_file_get_stream(log);
	stream_json = sfptpd_log_file_get_stream(log_json);

	putc('[', stream_json);

	/* Free each item in the collection, write out the stats */
	for (i = 0; i < stats->capacity; i++) {
		item = stats->items[i];

		/* If there's no item here just move on */
		if (item == NULL)
			continue;

		/* Write the title for the stats measure */
		fputs(item->name, stream);
		if (item->units != NULL)
			fprintf(stream, " (%s)", item->units);
		fputs("\n=========\n", stream);

		item->ops->write_headings(item, stream);
		if (i > 0)
			putc(',', stream_json);
		item->ops->write_json_opening(item, stream_json);

		first_entry = true;

		/* For each time period in the history... */
		for (p = 0; p < SFPTPD_STATS_PERIOD_MAX; p++) {
			/* For each complete set of data in the time period */
			for (h = 0; h < SFPTPD_STATS_HISTORY_MAX; h++) {
				interval = &stats->intervals[p][h];

				/* If this entry is the current minute, don't
				 * output anything as we haven't got any data */
				if ((p == SFPTPD_STATS_PERIOD_MINUTE) &&
				    (h == SFPTPD_STATS_HISTORY_CURRENT))
					continue;

				if (!interval->start_valid)
					continue;

				snprintf(name, sizeof(name), "%s[%d]",
					 sfptpd_stats_periods[p].name, -h);

				sfptpd_local_strftime(start, sizeof(start), "%Y-%m-%d %X",
						      &interval->start_time.tv_sec);

				/* If this is the current stats for a time
				 * period, we don't print an end time */
				if (h == SFPTPD_STATS_HISTORY_CURRENT) {
					sfptpd_strncpy(end, "---", sizeof(end));
				} else {
					sfptpd_local_strftime(end, sizeof(end), "%Y-%m-%d %X",
							      &interval->end_time.tv_sec);
				}

				item->ops->write_data(item, stream, name,
						      start, end, p, h);

				if (!first_entry)
					putc(',', stream_json);
				first_entry = false;
				item->ops->write_json_data(item, stream_json, p, h,
					sfptpd_stats_periods[p].name, sfptpd_stats_periods[p].length,
					interval->seq_num, start, end);
			}
		}

		fprintf(stream, "\n\n");
		item->ops->write_json_closing(item, stream_json);
	}

	putc(']', stream_json);
	sfptpd_log_file_close(log);
	sfptpd_log_file_close(log_json);
}

/**************************************************************
* Hash Table functions
**************************************************************/

static void sfptpd_stats_ht_copy_node(void *new_user, void *user)
{
	assert(new_user != NULL);
	assert(user != NULL);
	memcpy(new_user, user, sizeof(struct sfptpd_stats_ptp_node));
}

static void *sfptpd_stats_ht_alloc_node(void)
{
	void *new_user;
	
	new_user = calloc(1, sizeof(struct sfptpd_stats_ptp_node));
	if (new_user == NULL) {
		ERROR("Could not allocate memory for new foreign node info.");
	}
	
	return new_user;
}

static void sfptpd_stats_ht_free_node(void *user)
{
	struct sfptpd_stats_ptp_node *info;
	info = (struct sfptpd_stats_ptp_node *)user;
	assert(info != NULL);
	free(info);
}

static void sfptpd_stats_ht_node_get_key(void *user, void **key, unsigned int *key_length)
{
	assert(user != NULL);
	assert(key != NULL);
	assert(key_length != NULL);

	struct sfptpd_stats_ptp_node *node;

	node = (struct sfptpd_stats_ptp_node *)user;
	*key = node->clock_id.id;
	*key_length = sizeof(node->clock_id);
}


static const struct sfptpd_ht_ops sfptpd_stats_ht_ops = {
	sfptpd_stats_ht_alloc_node,
	sfptpd_stats_ht_copy_node,
	sfptpd_stats_ht_free_node,
	sfptpd_stats_ht_node_get_key
};


struct sfptpd_hash_table *sfptpd_stats_create_set(void)
{
	struct sfptpd_hash_table *new_table;

	new_table = sfptpd_ht_alloc(&sfptpd_stats_ht_ops,
				    SFPTPD_STATS_SET_SIZE,
				    SFPTPD_HT_STATS_SET_MAX);

	return new_table;
}


struct sfptpd_stats_ptp_node *sfptpd_stats_node_ht_get_first(struct sfptpd_hash_table *table,
							     struct sfptpd_ht_iter *iter)
{
	return (struct sfptpd_stats_ptp_node *)sfptpd_ht_first(table, iter);
}


struct sfptpd_stats_ptp_node *sfptpd_stats_node_ht_get_next(struct sfptpd_ht_iter *iter)
{
	return (struct sfptpd_stats_ptp_node *)sfptpd_ht_next(iter);
}


int sfptpd_stats_add_node(struct sfptpd_hash_table *table,
			   unsigned char *clock_id,
			   bool state,
			   uint16_t port_number,
			   uint16_t domain_number,
			   const char *transport_address)
{
	assert(table != NULL);

	struct sfptpd_stats_ptp_node new_node;
	int rc;

	memcpy(&new_node.clock_id, clock_id, sizeof(new_node.clock_id));
	sfptpd_clock_init_hw_id_string(new_node.clock_id_string,
					 new_node.clock_id,
					 SFPTPD_CLOCK_HW_ID_STRING_SIZE);
	new_node.state = (state) ? "Master" : "Slave";
	new_node.port_number = port_number;
	new_node.domain_number = domain_number;
	sfptpd_strncpy(new_node.transport_address,
		       transport_address,
		       sizeof new_node.transport_address);
	rc = sfptpd_ht_add(table, &new_node, true);

	if (rc != 0) {
		TRACE_L3("Addition to PTP-node set was unsuccessful\n");
	}

	return rc;
}


/* fin */
