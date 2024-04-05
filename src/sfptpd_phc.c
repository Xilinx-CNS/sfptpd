/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_phc.c
 * @brief  Support for kernel PTP Hardware Clocks (PHC)
 */

#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <fts.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/pps.h>
#include <linux/ptp_clock.h>

#include "sfptpd_logging.h"
#include "sfptpd_time.h"
#include "sfptpd_phc.h"
#include "sfptpd_thread.h"
#include "sfptpd_priv.h"


#ifdef SFPTPD_GLIBC_COMPAT
/****************************************************************************
 * Missing kernel API bits and pieces
 ****************************************************************************/

/* The oldest distribution we support has kernel support for PHC but the
 * syscall is missing in that glibc version. In this case we need to create our
 * own version of clock_adjtime() and invoke the syscall.
 *
 * Avoid including this where not needed as it overrides glibc's smarts for
 * handling 64-bit time on 32-bit architectures.
 */
#pragma weak clock_adjtime
int clock_adjtime(clockid_t clock, struct timex *timex_block)
{
	return syscall(__NR_clock_adjtime, clock, timex_block);
}
#endif


/****************************************************************************
 * Types, Defines and Structures
 ****************************************************************************/

/* Formatting string to create a paths to PHC and PPS devices */
#define SFPTPD_PHC_DEVICE_FORMAT           "/dev/ptp%d"
#define SFPTPD_PHC_NAME_FORMAT             "ptp%d"
#define SFPTPD_PHC_EXT_NAME_FORMAT         "ptp%d.ext"

/* Sysfs path of PPS devices */
#define SFPTPD_SYSFS_PPS_PATH              "/sys/class/pps/"

/* Macros to convert between PHC file descriptors and clock IDs */
#define PHC_FD_TO_POSIX_ID(fd)             ((~(clockid_t)(fd) << 3) | 3)
#define POSIX_ID_TO_PHC_FD(clk)            ((unsigned int)~((clk) >> 3))

/* Number of samples to take when comparing by reading time */
#define READ_TIME_NUM_SAMPLES              (4)

/* Number of samples to take when comparing with PTP_SYS_OFFSET (1-25) */
#define SYS_OFFSET_NUM_SAMPLES             (4)

/* Retry time for synthetic PPS */
#define SYNTH_PPS_RETRY_TIME               (2.2)

enum pps_state {
	PPS_NOT_TRIED,
	PPS_INIT,
	PPS_NOT_READY,
	PPS_GOOD,
	PPS_BAD,
};

struct phc_diff_method {
	sfptpd_phc_diff_fn diff_fn;
	void *context; /* If NULL, phc context is used */
};

struct sfptpd_phc {
	/* PHC Index */
	int phc_idx;

	/* File descriptor for PHC device */
	int phc_fd;

	/* Posix clock ID for the PHC device */
	clockid_t posix_id;

	/* PHC capabilities */
	struct ptp_clock_caps caps;

	/* PHC diff method definitions */
	struct phc_diff_method diff_method_defs[SFPTPD_DIFF_METHOD_MAX];

	/* Method that is used to compare the PHC and system clocks */
	enum sfptpd_phc_diff_method diff_method;

	/* Keep track of which diff methods have already been tried */
	int diff_method_index;

	/* File descriptor for associated PPS device */
	int pps_fd;

	/* Last PPS event */
	struct pps_ktime pps_prev;

	/* Last PPS event time - used to avoid spamming the ioctl */
	struct sfptpd_timespec pps_prev_monotime;

	/* Last calculated diff value */
	struct sfptpd_timespec diff_prev;

	/* Step since last sample */
	bool stepped_since_sample;

	/* Method that is used to sample external PPS input */
	sfptpd_phc_pps_method_t pps_method;

	/* Path to external PPS device */
	char devpps_path[PATH_MAX];

	/* File descriptor for associated external PPS device */
	int devpps_fd;

	/* Last external PPS event */
	struct pps_kinfo devpps_prev;

	/* Synthetic pps state */
	enum pps_state synth_pps_state;
};


/****************************************************************************
 * Forward declarations
 ****************************************************************************/

static int phc_set_fallback_diff_method(struct sfptpd_phc *phc);

static int phc_compare_using_precise_offset(void *phc, struct sfptpd_timespec *diff);
static int phc_compare_using_extended_offset(void *phc, struct sfptpd_timespec *diff);
static int phc_compare_using_sys_offset(void *phc, struct sfptpd_timespec *diff);
static int phc_compare_using_pps(void *phc, struct sfptpd_timespec *diff);
static int phc_compare_by_reading_time(void *phc, struct sfptpd_timespec *diff);


/****************************************************************************
 * Constants
 ****************************************************************************/


const char *sfptpd_phc_diff_method_text[] = {
	"sys-offset-precise",
	"efx",
	"pps",
	"sys-offset-ext",
	"sys-offset",
	"read-time",
	"none"
};

const static struct phc_diff_method phc_diff_method_defs[SFPTPD_DIFF_METHOD_MAX] = {
	{ phc_compare_using_precise_offset, },
	{ NULL /* EFX method is set by user */, },
	{ phc_compare_using_pps, },
	{ phc_compare_using_extended_offset, },
	{ phc_compare_using_sys_offset, },
	{ phc_compare_by_reading_time, },
};


/* Default PHC diff method order. */
const enum sfptpd_phc_diff_method sfptpd_default_phc_diff_methods[SFPTPD_DIFF_METHOD_MAX+1] = {
	SFPTPD_DIFF_METHOD_SYS_OFFSET_PRECISE,
	SFPTPD_DIFF_METHOD_EFX,
	SFPTPD_DIFF_METHOD_PPS,
	SFPTPD_DIFF_METHOD_SYS_OFFSET_EXTENDED,
	SFPTPD_DIFF_METHOD_SYS_OFFSET,
	SFPTPD_DIFF_METHOD_READ_TIME,
	SFPTPD_DIFF_METHOD_MAX
};

/* PHC diff methods order. */
static enum sfptpd_phc_diff_method phc_diff_methods[SFPTPD_DIFF_METHOD_MAX + 1] = {
	SFPTPD_DIFF_METHOD_MAX
};

const char *sfptpd_phc_pps_method_text[] = {
	"devptp",
	"devpps",
	"none"
};

/* Default PPS method order. */
const sfptpd_phc_pps_method_t sfptpd_default_pps_method[SFPTPD_PPS_METHOD_MAX + 1] = {
	SFPTPD_PPS_METHOD_DEV_PPS,
	SFPTPD_PPS_METHOD_DEV_PTP,
	SFPTPD_PPS_METHOD_MAX
};


/* PPS methods order. */
static sfptpd_phc_pps_method_t phc_pps_methods[SFPTPD_PPS_METHOD_MAX + 1] = {
	SFPTPD_PPS_METHOD_MAX
};


/****************************************************************************
 * Internal Functions
 ****************************************************************************/

static inline int phc_gettime(clockid_t clk_id, struct sfptpd_timespec *sfts)
{
	struct timespec ts;
	int rc = clock_gettime(clk_id, &ts);
	sfts->sec = ts.tv_sec;
	sfts->nsec = ts.tv_nsec;
	sfts->nsec_frac = 0;
	return rc;
}

/* Computes diff = a - b */
static void phc_pct_subtract(struct sfptpd_timespec *diff, struct ptp_clock_time *a,
			     struct ptp_clock_time *b)
{
	struct sfptpd_timespec subtrahend;

	sfptpd_time_init(diff, a->sec, a->nsec, 0);
	sfptpd_time_init(&subtrahend, b->sec, b->nsec, 0);
	sfptpd_time_subtract(diff, diff, &subtrahend);
}


/** Updates diff_out iif window is smaller than the smallest window.
 * Assuming we have time snapshots {sys_before, device, sys_after} = {b, d, a}.
 * We define: Window w = a - b  and  Midpoint m = (a + b) / 2
 * We want to calculate: diff = d - m = d - b/2 - a/2 = d - b - w/2
 * @param window Window w = a - b
 * @param smallest_window in/out, set to INFINITY before first call
 * @param dev_start_diff d - b (device - sys_before)
 * @param diff_out will be updated iif window is smaller
 * @return true if diff_out is updated, false otherwise. */
static bool phc_update_smallest_window_diff(struct sfptpd_timespec *window,
	sfptpd_time_t *smallest_window, struct sfptpd_timespec *dev_start_diff,
	struct sfptpd_timespec *diff_out)
{
	struct sfptpd_timespec ts;
	sfptpd_time_t w = sfptpd_time_timespec_to_float_ns(window);

	/* Is the window positive and the smallest seen so far? */
	if (w > 0.0 && w < *smallest_window) {
		*smallest_window = w;

		/* Calculate the difference between the PHC and system
		 * clock estimated as half way through the window */
		sfptpd_time_float_ns_to_timespec(w / 2.0, &ts);
		sfptpd_time_subtract(diff_out, dev_start_diff, &ts);
		return true;
	}
	return false;
}


static int phc_configure_pps(struct sfptpd_phc *phc)
{
	FTS *fts;
	FTSENT *fts_entry;
	FILE *file;
	char * const search_path[] = {SFPTPD_SYSFS_PPS_PATH, NULL};
	char path[PATH_MAX];
	char phc_name[16], candidate_name[16];
	int rc, tokens;

	assert(phc != NULL);

	snprintf(phc_name, sizeof(phc_name), SFPTPD_PHC_NAME_FORMAT,
		 phc->phc_idx);

	/* Search through the PPS devices looking for the one associated with
	 * this PHC device. */
	fts = fts_open(search_path, FTS_COMFOLLOW, NULL);
	if (fts == NULL) {
		ERROR("phc: failed to open sysfs pps devices directory, %s\n",
		      strerror(errno));
		phc->synth_pps_state = PPS_BAD;
		return errno;
	}

	fts_entry = fts_read(fts);
	if (fts_entry == NULL) {
		ERROR("phc: failed to read sysfs pps directory, %s\n", strerror(errno));
		rc = errno;
		goto fail1;
	}

	fts_entry = fts_children(fts, 0);
	if (fts_entry == NULL) {
		ERROR("phc: failed to get sysfs pps directory listing, %s\n",
		      strerror(errno));
		rc = errno;
		goto fail1;
	}

	/* Iterate through the linked list of files within the directory... */
	for ( ; fts_entry != NULL; fts_entry = fts_entry->fts_link) {
		/* Attempt to read the PPS device name file */
		snprintf(path, sizeof(path), "%s%s/name",
			 fts_entry->fts_path, fts_entry->fts_name);

		file = fopen(path, "r");
		if (file == NULL) {
			TRACE_L3("phc: couldn't open %s\n", path);
		} else {
			tokens = fscanf(file, "%16s", candidate_name);
			(void)fclose(file);

			if ((tokens == 1) && (strcmp(candidate_name, phc_name) == 0)) {
				TRACE_L3("phc%d: found %s\n",
					 phc->phc_idx, fts_entry->fts_name);
				break;
			}
		}
	}

	/* If we didn't find the PPS device, abandon ship */
	if (fts_entry == NULL) {
		ERROR("phc%d: failed to find corresponding PPS device\n",
		      phc->phc_idx);
		rc = ENOENT;
		goto fail1;
	}

	/* Create the path of the PPS device, open the device and enable PPS
	 * events on the PHC device */
	snprintf(path, sizeof(path), "/dev/%s", fts_entry->fts_name);

	phc->pps_fd = sfptpd_priv_open_dev(path);
	if (phc->pps_fd < 0) {
		rc = -phc->pps_fd;
		phc->pps_fd = -1;
		ERROR("phc%d: failed to open PPS device %s, %s\n",
		      phc->phc_idx, path, strerror(errno));
		goto fail1;
	}

	if (ioctl(phc->phc_fd, PTP_ENABLE_PPS, 1) != 0) {
		ERROR("phc%d: failed to enable PPS events, %s\n",
		      phc->phc_idx, strerror(errno));
		rc = errno;
		goto fail2;
	}

	/* Reset previous sample time */
	sfptpd_time_zero(&phc->pps_prev_monotime);

	/* Reset previous sample data */
	phc->pps_prev.sec = 0;
	phc->pps_prev.nsec = 0;

	TRACE_L3("phc%d: successfully configured %s\n",
		 phc->phc_idx, fts_entry->fts_name);
	phc->synth_pps_state = PPS_INIT;
	fts_close(fts);
	return 0;

fail2:
	close(phc->pps_fd);
	phc->pps_fd = -1;
fail1:
	phc->synth_pps_state = PPS_BAD;
	fts_close(fts);
	return rc;
}


static int phc_discover_devpps(struct sfptpd_phc *phc,
			       char *path_buf,
			       size_t path_buf_size)
{
	FTS *fts;
	FTSENT *fts_entry;
	FILE *file;
	char * const search_path[] = {SFPTPD_SYSFS_PPS_PATH, NULL};
	char phc_name[16], candidate_name[16], phc_extname[16];
	int rc, tokens;

	/* The Xilinx sfc net driver provides a second PPS device following
	   the internal one, named "sfc", so this state machine looks for it. */
	enum {
		STATE_SEARCHING,
		STATE_FOUND_INTPPS,
		STATE_FOUND_EXTPPS,
		STATE_NOTFOUND,
	} state = STATE_SEARCHING;

	assert(phc != NULL);

	snprintf(phc_name, sizeof(phc_name), SFPTPD_PHC_NAME_FORMAT,
		 phc->phc_idx);
	snprintf(phc_extname, sizeof(phc_extname), SFPTPD_PHC_EXT_NAME_FORMAT,
		 phc->phc_idx);

	/* Search through the PPS devices looking for the one associated with
	 * this PHC device. */
	fts = fts_open(search_path, FTS_COMFOLLOW, NULL);
	if (fts == NULL) {
		ERROR("phc: failed to open sysfs pps devices directory, %s\n",
		      strerror(errno));
		return errno;
	}

	fts_entry = fts_read(fts);
	if (fts_entry == NULL) {
		ERROR("phc: failed to read sysfs pps directory, %s\n", strerror(errno));
		rc = errno;
		goto fail1;
	}

	fts_entry = fts_children(fts, 0);
	if (fts_entry == NULL) {
		TRACE_L5("phc: failed to get sysfs pps directory listing\n");
		rc = ENOENT;
		goto fail1;
	}

	/* Iterate through the linked list of files within the directory... */
	for ( ; fts_entry != NULL && state != STATE_NOTFOUND;
	     fts_entry = fts_entry->fts_link) {

		/* Attempt to read the PPS device name file */
		snprintf(path_buf, path_buf_size, "%s%s/name",
			 fts_entry->fts_path, fts_entry->fts_name);

		file = fopen(path_buf, "r");
		if (file == NULL) {
			TRACE_L3("phc: couldn't open %s\n", path_buf);
			if (state == STATE_FOUND_INTPPS)
				state = STATE_NOTFOUND;
		} else {
			tokens = fscanf(file, "%16s", candidate_name);
			(void)fclose(file);

			if ((tokens == 1)) {
				if (strcmp(candidate_name, phc_extname) == 0) {
					state = STATE_FOUND_EXTPPS;
				} else if (state == STATE_SEARCHING) {
					if (strcmp(candidate_name, phc_name) == 0)
						state = STATE_FOUND_INTPPS;
				} else if (state == STATE_FOUND_INTPPS) {
					if ((strcmp(candidate_name, "sfc") == 0) ||
					    (strcmp(candidate_name, "xlnx") == 0)) {
						state = STATE_FOUND_EXTPPS;
					}
				}
			} else if(state == STATE_FOUND_INTPPS)
				state = STATE_SEARCHING;
		}

		if (state == STATE_FOUND_EXTPPS) {
			TRACE_L5("phc%d: found %s (\"%s\") for external PPS input\n",
				 phc->phc_idx, fts_entry->fts_name, candidate_name);
			break;
		}
	}

	/* If we didn't find the PPS device, abandon ship */
	if (state != STATE_FOUND_EXTPPS) {
		TRACE_L6("phc%d: failed to find corresponding external PPS device\n",
			 phc->phc_idx);
		rc = ENOENT;
		goto fail1;
	}

	/* Create the path of the PPS device */
	snprintf(path_buf, path_buf_size, "/dev/%s", fts_entry->fts_name);

	fts_close(fts);
	return 0;

fail1:
	fts_close(fts);
	return rc;
}


static int phc_open_devpps(struct sfptpd_phc *phc)
{
	if (phc->devpps_fd >= 0) {
		TRACE_L4("phc%d: devpps already open\n", phc->phc_idx);
		return 0;
	}

	/* Open the PPS device */
	phc->devpps_fd = sfptpd_priv_open_dev(phc->devpps_path);
	if (phc->devpps_fd < 0) {
		int rc = -phc->devpps_fd;
		phc->devpps_fd = -1;
		ERROR("phc%d: failed to open external PPS device %s, %s\n",
		      phc->phc_idx, phc->devpps_path, strerror(rc));
		return rc;
	}

	return 0;
}


static int phc_compare_by_reading_time_n(struct sfptpd_phc *phc, unsigned int num_samples,
					 struct sfptpd_timespec *diff)
{
	struct sfptpd_timespec sys_ts[2], phc_ts, window, ts;
	unsigned int i;
	sfptpd_time_t smallest_window;
	int rc;

	assert(phc != NULL);
	assert(diff != NULL);

	rc = EAGAIN;
	smallest_window = INFINITY;

	for (i = 0; i < num_samples; i++) {
		/* Read the PHC time bounded either side by the system time */
		if ((phc_gettime(CLOCK_REALTIME, &sys_ts[0]) != 0) ||
		    (phc_gettime(phc->posix_id, &phc_ts) != 0) ||
		    (phc_gettime(CLOCK_REALTIME, &sys_ts[1]) != 0)) {
			ERROR("phc%d read-time: failed to read time, %s\n",
			      phc->phc_idx, strerror(errno));
			return errno;
		}

		/* Calculate the window size and update diff if it's smallest */
		sfptpd_time_subtract(&window, &sys_ts[1], &sys_ts[0]);
		sfptpd_time_subtract(&ts, &phc_ts, &sys_ts[0]);
		if (phc_update_smallest_window_diff(&window, &smallest_window,
						    &ts, diff))
			rc = 0;
	}

	if (rc == 0)
		phc->diff_prev = *diff;

	return rc;
}


static int phc_compare_using_pps(void *context, struct sfptpd_timespec *diff)
{
	struct sfptpd_phc *phc = (struct sfptpd_phc *) context;
	struct pps_fdata pps;
	int rc;
	struct sfptpd_timespec approx, mono_now;

	assert(phc != NULL);
	assert(diff != NULL);

	/* By definition, PPS events only happen once per second.
	 * So we only start calling the ioctl when it's close to happening. */
	phc_gettime(CLOCK_MONOTONIC, &mono_now);
	sfptpd_time_t ns_diff = sfptpd_time_timespec_to_float_ns(&mono_now) -
			sfptpd_time_timespec_to_float_ns(&phc->pps_prev_monotime);

	/* >900 ms elapsed since last PPS event? */
	if (ns_diff < (ONE_BILLION * 0.9)) {
		TRACE_L6("phc%d: returning previous PPS sample due to short elapsed time %Lf\n",
				 phc->phc_idx, ns_diff);
		*diff = phc->diff_prev;
		return phc->stepped_since_sample ? EAGAIN : 0;
	}

	pps.timeout.sec = 0;
	pps.timeout.nsec = 0;
	pps.timeout.flags = ~PPS_TIME_INVALID;

	if (ioctl(phc->pps_fd, PPS_FETCH, &pps) != 0) {
		ERROR("phc%d pps: failed to read event, %s\n",
		      phc->phc_idx, strerror(errno));
		return errno;
	}

	TRACE_L4("phc%d pps: assert = " SFPTPD_FORMAT_TIMESPEC "\n",
		 phc->phc_idx, pps.info.assert_tu.sec, pps.info.assert_tu.nsec);

	/* If we don't have a new PPS event, just return the last difference
	 * calculated. */
	if ((pps.info.assert_tu.sec == phc->pps_prev.sec) &&
	    (pps.info.assert_tu.nsec == phc->pps_prev.nsec)) {
		/* Some adapters (e.g. ixgbe X540) never produce any PPS data.
		 * In this case we fall back to another diff method. */
		if (pps.info.assert_tu.sec == 0 && pps.info.assert_tu.nsec == 0) {
			if (phc->synth_pps_state == PPS_INIT) {
				phc->pps_prev_monotime = mono_now;
				phc->synth_pps_state = PPS_NOT_READY;
				NOTICE("phc%d: no pps data yet, changing diff method temporarily\n",
					phc->phc_idx);
			} else {
				phc->synth_pps_state = PPS_BAD;
				WARNING("phc%d: no pps data, changing diff method\n",
					phc->phc_idx);
			}

			rc = phc_set_fallback_diff_method(phc);
			if (rc != 0)
				return rc;

			/* Make sure we don't go into infinite recursion. */
			assert(phc->diff_method != SFPTPD_DIFF_METHOD_PPS);
			return sfptpd_phc_compare_to_sys_clk(phc, diff);
		}
		TRACE_L6("phc%d pps: no new event, returning previous diff\n",
			 phc->phc_idx);
		*diff = phc->diff_prev;
		return phc->stepped_since_sample ? EAGAIN : 0;
	}

	/* Store the last event time */
	phc->pps_prev_monotime = mono_now;
	phc->synth_pps_state = PPS_GOOD;

	/* Compare to system time to get seconds offset. */
	rc = phc_compare_by_reading_time_n(phc, READ_TIME_NUM_SAMPLES, &approx);
	if (rc != 0) {
		TRACE_L3("phc%d pps: read_time for pps tod failed\n", phc->phc_idx);
		return rc;
	}

	/* The seconds is the rough offset rounded to the nearest second */
	diff->sec = approx.sec;
	if (approx.nsec >= 500000000)
		diff->sec += 1;

	/* The nanosecond value comes from the PPS timestamp */
	diff->nsec_frac = 0;
	diff->nsec = 1000000000 - pps.info.assert_tu.nsec;
	if (diff->nsec >= 500000000)
		diff->sec -= 1;

	TRACE_L6("phc%d pps: approx " SFPTPD_FMT_SSFTIMESPEC "\n",
		 phc->phc_idx,
		 SFPTPD_ARGS_SSFTIMESPEC(approx));

	/* Store the PPS event time and calculated diff */
	phc->pps_prev = pps.info.assert_tu;
	phc->diff_prev = *diff;
	phc->stepped_since_sample = false;

	return 0;
}


static int phc_compare_using_precise_offset(void *context,
					    struct sfptpd_timespec *diff)
{
	int rc = EOPNOTSUPP;

#ifdef PTP_SYS_OFFSET_PRECISE
	/* Kernel snapshot of {dev, real, mono} clock times */
	struct ptp_sys_offset_precise ktimes;
	struct sfptpd_phc *phc = (struct sfptpd_phc *) context;

	assert(phc != NULL);
	assert(phc->phc_fd >= 0);
	assert(diff != NULL);

	memset(&ktimes, 0, sizeof(ktimes));
	rc = ioctl(phc->phc_fd, PTP_SYS_OFFSET_PRECISE, &ktimes);
	if (rc != 0)
		return errno;

	/* Subtract ( phc - sys ) times. */
	phc_pct_subtract(diff, &ktimes.device, &ktimes.sys_realtime);
	phc->diff_prev = *diff;

	TRACE_L6("phc%d sys-offset-precise: device:  %10lld.%u\n", phc->phc_idx,
		 ktimes.device.sec, ktimes.device.nsec);
	TRACE_L6("phc%d sys-offset-precise: real:    %10lld.%u\n", phc->phc_idx,
		 ktimes.sys_realtime.sec, ktimes.sys_realtime.nsec);
#endif

	return rc;
}


static int phc_compare_using_extended_offset_n(struct sfptpd_phc *phc,
					       int n_samples,
					       struct sfptpd_timespec *diff)
{
	int rc = EOPNOTSUPP;

#ifdef PTP_SYS_OFFSET_EXTENDED
	int i;
	struct ptp_sys_offset_extended sysoff;
	struct sfptpd_timespec ts, window;
	sfptpd_time_t smallest_window;
	bool test_mode = false;

	assert(phc != NULL);
	assert(diff != NULL);

	if (n_samples == 0) {
		n_samples = 1;
		test_mode = true;
	}

	assert(n_samples >= 1 && n_samples <= PTP_MAX_SAMPLES);

	memset(&sysoff, 0, sizeof(sysoff));
	sysoff.n_samples = n_samples;
	rc = ioctl(phc->phc_fd, PTP_SYS_OFFSET_EXTENDED, &sysoff);
	if (rc != 0)
		return errno;
	if (test_mode)
		return 0;

	rc = EAGAIN;
	smallest_window = INFINITY;

	for (i = 0; i < sysoff.n_samples; i++) {
		struct ptp_clock_time *sys_before = &sysoff.ts[i][0];
		struct ptp_clock_time *device     = &sysoff.ts[i][1];
		struct ptp_clock_time *sys_after  = &sysoff.ts[i][2];

		/* Calculate the window size and update diff if it's smallest */
		phc_pct_subtract(&window, sys_after, sys_before);
		phc_pct_subtract(&ts, device, sys_before);
		if (phc_update_smallest_window_diff(&window, &smallest_window,
						    &ts, diff))
			rc = 0;
	}

	TRACE_L6("phc%d sys-offset-ext: smallest_window: %Lf\n",
		 phc->phc_idx,
		 smallest_window);

	if (rc == 0)
		phc->diff_prev = *diff;
#endif

	return rc;
}


static int phc_compare_using_kernel_readings_n(struct sfptpd_phc *phc,
					       int n_samples,
					       struct sfptpd_timespec *diff)
{
	int rc, i;
	struct ptp_sys_offset sysoff;
	struct sfptpd_timespec ts, window;
	sfptpd_time_t smallest_window;

	assert(phc != NULL);
	assert(diff != NULL);
	assert(n_samples >= 1 && n_samples <= PTP_MAX_SAMPLES);

	memset(&sysoff, 0, sizeof(sysoff));
	sysoff.n_samples = n_samples;
	rc = ioctl(phc->phc_fd, PTP_SYS_OFFSET, &sysoff);
	if (rc != 0)
		return errno;

	rc = EAGAIN;
	smallest_window = INFINITY;

	for (i = 0; i < sysoff.n_samples; i++) {
		struct ptp_clock_time *sys_before = &sysoff.ts[2*i];
		struct ptp_clock_time *device     = &sysoff.ts[2*i+1];
		struct ptp_clock_time *sys_after  = &sysoff.ts[2*i+2];

		/* Calculate the window size and update diff if it's smallest */
		phc_pct_subtract(&window, sys_after, sys_before);
		phc_pct_subtract(&ts, device, sys_before);
		if (phc_update_smallest_window_diff(&window, &smallest_window,
						    &ts, diff))
			rc = 0;
	}

	TRACE_L6("phc%d sys-offset: smallest_window: %Lf\n",
		 phc->phc_idx,
		 smallest_window);

	if (rc == 0)
		phc->diff_prev = *diff;

	return rc;
}


static int phc_set_fallback_diff_method(struct sfptpd_phc *phc)
{
	enum sfptpd_phc_diff_method method;
	const struct phc_diff_method *defn;
	int sys_offset_extended_rc;
	struct sfptpd_timespec sink;
	int rc;

	assert(phc != NULL);

	/* Requires invariant that diff methods array is terminated by SFPTPD_DIFF_METHOD_MAX */
	for (phc->diff_method_index++;
	     phc->diff_method_index < SFPTPD_DIFF_METHOD_MAX;
	     phc->diff_method_index++) {
		method = phc_diff_methods[phc->diff_method_index];
		defn = phc->diff_method_defs + method;

		TRACE_L4("phc%d: checking %dth method (%p), %s\n",
		     phc->phc_idx, phc->diff_method_index,
		     defn->diff_fn,
		     sfptpd_phc_diff_method_text[method]);

		switch (method) {
		case SFPTPD_DIFF_METHOD_SYS_OFFSET_PRECISE:
#ifdef PTP_SYS_OFFSET_PRECISE
			if (phc->caps.cross_timestamping != 0) {
				/* Use PTP_SYS_OFFSET_PRECISE (cross-timestamping) */
				INFO("phc%d: using diff method SYS_OFFSET_PRECISE\n", phc->phc_idx);
				goto diff_method_selected;
			}
#endif
			break;

		case SFPTPD_DIFF_METHOD_SYS_OFFSET_EXTENDED:
			/* Use PTP_SYS_OFFSET_EXTENDED */
			sys_offset_extended_rc =
				phc_compare_using_extended_offset_n(phc, 0, &sink);
			if (sys_offset_extended_rc == 0) {
				INFO("phc%d: using diff method SYS_OFFSET_EXTENDED\n", phc->phc_idx);
				goto diff_method_selected;
			}
			break;

		case SFPTPD_DIFF_METHOD_PPS:
			/* Using an associated PPS device */
			if (phc->caps.pps != 0) {
				/* Attempt to configure the PPS device associated with this
				* PHC device. */
				rc = phc_configure_pps(phc);
				if (rc == 0) {
					INFO("phc%d: using diff method PPS\n", phc->phc_idx);
					goto diff_method_selected;
				} else {
					WARNING("phc%d: failed to configure PPS: %d\n", phc->phc_idx, rc);
				}
			}
			break;

		case SFPTPD_DIFF_METHOD_SYS_OFFSET:
			/* PTP_SYS_OFFSET was added in kernel v3.8 :
			* github.com/torvalds/linux/commit/215b13dd288c2e1e4461c1530a801f5f83e8cd90
			* there doesn't seem to be any associated capability so just try it.
			* This will set errno to either EOPNOTSUPP or EFAULT.
			* The ioctl will always fail because we pass NULL in the 3rd arg. */
			assert(-1 == ioctl(phc->phc_fd, PTP_SYS_OFFSET, NULL));
			if (errno == EFAULT) {
				INFO("phc%d: using diff method SYS_OFFSET\n", phc->phc_idx);
				goto diff_method_selected;
			}
			break;

		case SFPTPD_DIFF_METHOD_READ_TIME:
			/* Read the system and NIC time from user-space */
			INFO("phc%d: using diff method READ_TIME\n", phc->phc_idx);
			goto diff_method_selected;

		case SFPTPD_DIFF_METHOD_MAX:
			/* We've reached the sentinel. */
			goto no_diff_method_selected;

		default:
			if (defn->diff_fn != NULL) {
				rc = defn->diff_fn(defn->context != NULL ? defn->context : phc, &sink);
				if (rc == 0) {
					INFO("phc%d: using diff method %s\n",
					     phc->phc_idx,
					     sfptpd_phc_diff_method_text[method]);
					goto diff_method_selected;
				}
			}
		}
	}

no_diff_method_selected:
	CRITICAL("phc%d: No configured diff methods available\n", phc->phc_idx);
	assert(method == SFPTPD_DIFF_METHOD_MAX);

diff_method_selected:
	phc->diff_method = method;

	return phc->diff_method == SFPTPD_DIFF_METHOD_MAX ? EOPNOTSUPP : 0;
}


static int phc_enable_devptp(struct sfptpd_phc *phc, bool on)
{
	struct ptp_extts_request req = { 0 };
	const char *indicative = on ? "enable" : "disable";
	const char *past_participle = on ? "enabled" : "disabled";
	const int pin = 0;
	int rc = 0;

	assert(phc != NULL);

	if (phc->caps.n_ext_ts == 0) {
		TRACE_L2("phc%d: no external time stamp channel available to %s\n",
		phc->phc_idx, indicative);
		return ENOTSUP;
	}

#ifdef PTP_PIN_SETFUNC
	if (on) {
		struct ptp_pin_desc pin_conf = { "" };

		pin_conf.index = 0;
		pin_conf.func = 1; /* external timestamp */
		pin_conf.chan = 0;
		rc = ioctl(phc->phc_fd, PTP_PIN_SETFUNC, &pin_conf);

		if (rc != 0) {
			rc = errno;
			ERROR("phc%d: could not set pin function: %s\n",
			      phc->phc_idx, strerror(rc));
		} else {
			TRACE_L2("phc%d: set pin %d to function %d (external timestamp)\n",
				 phc->phc_idx, pin_conf.index, pin_conf.func);
		}
	}
#endif

	req.index = pin;
	req.flags = on ? (PTP_ENABLE_FEATURE | PTP_RISING_EDGE) : 0;
	rc = ioctl(phc->phc_fd, PTP_EXTTS_REQUEST, &req);

	if (rc != 0) {
		rc = errno;
		ERROR("phc%d: could not %s PPS via PHC: %s\n",
		      phc->phc_idx, indicative, strerror(rc));
		return rc;
	} else {
		TRACE_L2("phc%d: %s external time stamp channel %d\n",
			 phc->phc_idx, past_participle, pin);
	}

	return rc;
}


static int phc_get_devptp_event(struct sfptpd_phc *phc, struct sfptpd_timespec *timestamp)
{
	struct ptp_extts_event event;
	const int pin = 0;
	int rc;

	assert(phc != NULL);
	assert(timestamp != NULL);

	do {
		rc = read(phc->phc_fd, &event, sizeof event);

		if (rc != sizeof event) {
			ERROR("phc%d: could not read event: %s\n",
			      phc->phc_idx, strerror(errno));
			return errno;
		} else if (event.index == pin) {
			TRACE_L5("phc%d: external timestamp at %lld.%09u\n",
				 phc->phc_idx, event.t.sec, event.t.nsec);
			sfptpd_time_init(timestamp, event.t.sec, event.t.nsec, 0);
		}
	} while (event.index != pin);
	return 0;
}


static int phc_enable_devpps(struct sfptpd_phc *phc, bool on)
{
	const char *indicative = on ? "enable" : "disable";
	const char *past_participle = on ? "enabled" : "disabled";
	int rc = 0;

	assert(phc != NULL);

	if (!on && phc->devpps_fd < 0) {
		/* TODO: for now don't open just to be able to disable but
		   that may have to change if we need to ensure a proper reset.
		 */
		return 0;
	}

	if (on) {
		/* Find the PPS device and open it, but don't duplicate error */
		rc = phc_open_devpps(phc);
		if (rc != 0)
			return rc;
	}

	rc = phc->devpps_fd >= 0 ? 0 : ENOENT;

	if (rc != 0)
		ERROR("phc%d: could not %s PPS via PPS: %s\n",
		      phc->phc_idx, indicative, strerror(rc));
	else
		TRACE_L2("phc%d: %s external PPS device: %s\n",
			 phc->phc_idx, past_participle,
			 phc->devpps_path);

	return rc;
}


static int phc_get_devpps_event(struct sfptpd_phc *phc, struct sfptpd_timespec *timestamp, uint32_t *seq)
{
	struct pps_fdata pps_data;
	int rc;

	assert(phc != NULL);
	assert(timestamp != NULL);
	assert(seq != NULL);
	assert(phc->devpps_fd != -1);

	pps_data.timeout.sec = 0;
	pps_data.timeout.nsec = 0;
	pps_data.timeout.flags = ~PPS_TIME_INVALID;

	rc = ioctl(phc->devpps_fd, PPS_FETCH, &pps_data);

	if (rc != 0) {
		ERROR("phc%d: could not retrieve PPS event: %s\n",
		      phc->phc_idx, strerror(errno));
		return errno;
	} else if (pps_data.info.assert_tu.sec == phc->devpps_prev.assert_tu.sec &&
		   pps_data.info.assert_tu.nsec == phc->devpps_prev.assert_tu.nsec &&
		   pps_data.info.assert_sequence == phc->devpps_prev.assert_sequence) {

		return EAGAIN;
	} else {
		TRACE_L5("phc%d: external PPS timestamp at %lld.%09u\n",
			 phc->phc_idx,
			 pps_data.info.assert_tu.sec,
			 pps_data.info.assert_tu.nsec);
		sfptpd_time_init(timestamp,
			         pps_data.info.assert_tu.sec,
				 pps_data.info.assert_tu.nsec, 0);
		*seq = pps_data.info.assert_sequence;

		phc->devpps_prev = pps_data.info;
	}
	return 0;
}


static void phc_discover_pps(struct sfptpd_phc *phc)
{
	int i;

	/* Check external time stamp capabilities */
	TRACE_L2("phc%d: %d external time stamp channels\n",
		 phc->phc_idx, phc->caps.n_ext_ts);

	/* Discover external PPS device */
	if (phc_discover_devpps(phc, phc->devpps_path, sizeof phc->devpps_path) == 0) {
		TRACE_L2("phc%d: discovered related external PPS device %s\n",
			 phc->phc_idx, phc->devpps_path);
	} else {
		phc->devpps_path[0] = '\0';
	}

	for (i = 0; i < SFPTPD_PPS_METHOD_MAX; i++) {
		sfptpd_phc_pps_method_t method = phc_pps_methods[i];
		switch (method) {
		case SFPTPD_PPS_METHOD_DEV_PTP:
			if (phc->caps.n_ext_ts >= 1) {
				phc->pps_method = method;
				return;
			}
			break;
		case SFPTPD_PPS_METHOD_DEV_PPS:
			if (phc->devpps_path[0]) {
				if (phc_open_devpps(phc) == 0) {
					phc->pps_method = method;
					return;
				}
			}
			break;
		case SFPTPD_PPS_METHOD_MAX:
			phc->pps_method = method;
			return;
		}
	}
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_phc_set_diff_methods(const enum sfptpd_phc_diff_method *new_order)
{
	const enum sfptpd_phc_diff_method sentinel = SFPTPD_DIFF_METHOD_MAX;
	int i;

	assert(new_order);

	for (i = 0; i <= SFPTPD_DIFF_METHOD_MAX; i++) {
		phc_diff_methods[i] = new_order[i];
		if (new_order[i] == sentinel)
			return 0;
	}

	phc_diff_methods[SFPTPD_DIFF_METHOD_MAX] = sentinel;
	ERROR("phc: new diff method order too long\n");
	return ERANGE;
}

int sfptpd_phc_open(int phc_index, struct sfptpd_phc **phc)
{
	const int timex_max_adj_32bit = ((1LL<<31)-1)*1000/65536;
	char path[PATH_MAX];
	struct sfptpd_phc *new;
	int rc;

	assert(phc_index >= 0);
	assert(phc != NULL);

	*phc = NULL;

	/* Allocate storage */
	new = (struct sfptpd_phc *)calloc(1, sizeof(*new));
	if (new == NULL)
		return ENOMEM;

	new->diff_method_index = SFPTPD_DIFF_METHOD_MAX;
	new->pps_method = SFPTPD_PPS_METHOD_MAX;

	/* Open the PHC device */
	snprintf(path, sizeof(path), SFPTPD_PHC_DEVICE_FORMAT, phc_index);
	new->phc_fd = sfptpd_priv_open_dev(path);
	if (new->phc_fd < 0) {
		rc = -new->phc_fd;
		new->phc_fd = -1;
		ERROR("phc%d: failed to open device %s, %s\n",
		      phc_index, path, strerror(rc));
		goto fail1;
	}

	rc = ioctl(new->phc_fd, PTP_CLOCK_GETCAPS, &new->caps);
	if (rc != 0) {
		ERROR("phc%d: failed to get capabilities, %s\n",
		      phc_index, strerror(errno));
		rc = errno;
		goto fail2;
	}

	new->phc_idx = phc_index;
	new->posix_id = PHC_FD_TO_POSIX_ID(new->phc_fd);
	new->pps_fd = -1;
	new->synth_pps_state = PPS_NOT_TRIED;
	new->devpps_fd = -1;
	new->devpps_prev.assert_sequence = UINT32_MAX;
	memcpy(new->diff_method_defs, phc_diff_method_defs, sizeof new->diff_method_defs);

	/* On 32 bit platforms, the scaled frequency adjustment in the timex
	 * structure (long) is not big enough for the range of frequency
	 * adjustments that we can support. In this case we have to saturate
	 * the frequency adjustment to the maximum value that can fit in a
	 * signed 32 bit integer. */
	if (sizeof(long) == 4)
		new->caps.max_adj = timex_max_adj_32bit;

	phc_discover_pps(new);

	*phc = new;
	return 0;

fail2:
	close(new->phc_fd);

fail1:
	free(new);
	return rc;
}

int sfptpd_phc_start(struct sfptpd_phc *phc)
{

	phc->diff_method_index = -1;
	return phc_set_fallback_diff_method(phc);
}

void sfptpd_phc_close(struct sfptpd_phc *phc)
{
	assert(phc != NULL);

	if (phc->devpps_fd >= 0) {
		/* Close the external PPS device */
		(void)close(phc->devpps_fd);
	}

	if (phc->pps_fd >= 0) {
		/* Disable PPS and close the PPS device */
		(void)ioctl(phc->phc_fd, PTP_ENABLE_PPS, 0);
		(void)close(phc->pps_fd);
	}

	if (phc->phc_fd >= 0)
		(void)close(phc->phc_fd);

	free(phc);
}


clockid_t sfptpd_phc_get_clock_id(struct sfptpd_phc *phc)
{
	assert(phc != NULL);
	return phc->posix_id;
}


int sfptpd_phc_get_max_freq_adj(struct sfptpd_phc *phc)
{
	assert(phc != NULL);
	return phc->caps.max_adj;
}


static int phc_compare_using_extended_offset(void *context, struct sfptpd_timespec *diff)
{
	struct sfptpd_phc *phc = (struct sfptpd_phc *) context;

	return phc_compare_using_extended_offset_n(phc, SYS_OFFSET_NUM_SAMPLES, diff);
}


static int phc_compare_using_sys_offset(void *context, struct sfptpd_timespec *diff)
{
	struct sfptpd_phc *phc = (struct sfptpd_phc *) context;

	return phc_compare_using_kernel_readings_n(phc, SYS_OFFSET_NUM_SAMPLES, diff);
}


static int phc_compare_by_reading_time(void *context, struct sfptpd_timespec *diff)
{
	struct sfptpd_phc *phc = (struct sfptpd_phc *) context;

	return phc_compare_by_reading_time_n(phc, SYS_OFFSET_NUM_SAMPLES, diff);
}


int sfptpd_phc_compare_to_sys_clk(struct sfptpd_phc *phc, struct sfptpd_timespec *diff)
{
	const struct phc_diff_method *method_def;
	int rc;

	assert(phc != NULL);
	assert(diff != NULL);

	/* Check if we need to give PPS a second chance */
	if (phc->synth_pps_state == PPS_NOT_READY) {
		struct sfptpd_timespec now;
		struct sfptpd_timespec expiry;

		phc_gettime(CLOCK_MONOTONIC, &now);
		sfptpd_time_float_s_to_timespec(SYNTH_PPS_RETRY_TIME, &expiry);
		sfptpd_time_add(&expiry, &phc->pps_prev_monotime, &expiry);

		if (sfptpd_time_is_greater_or_equal(&now, &expiry)) {
			int method_idx;

			/* Find and activate the PPS method */
			for (method_idx = 0; method_idx < SFPTPD_DIFF_METHOD_MAX; method_idx++) {
				if (phc_diff_methods[method_idx] == SFPTPD_DIFF_METHOD_PPS) {
					phc->diff_method_index = method_idx;
					phc->diff_method = phc_diff_methods[method_idx];
					INFO("phc%d: reselecting diff method %s\n",
					     phc->phc_idx,
					     sfptpd_phc_diff_method_text[phc->diff_method]);
					break;
				}
			}
		}
	}

	if (phc->diff_method == SFPTPD_DIFF_METHOD_MAX)
		return EOPNOTSUPP;

	assert(phc->diff_method <= SFPTPD_DIFF_METHOD_MAX);
        if (phc->diff_method > SFPTPD_DIFF_METHOD_MAX)
		return EINVAL;

	method_def = phc->diff_method_defs + phc->diff_method;

	if (method_def->diff_fn != NULL)
		rc = method_def->diff_fn(method_def->context ? method_def->context : phc, diff);
	else
		rc = EOPNOTSUPP;

	if (rc == 0) {
		TRACE_L5("phc%d %s: phc-sys diff: " SFPTPD_FMT_SSFTIMESPEC "\n",
			 phc->phc_idx,
			 sfptpd_phc_get_diff_method_name(phc),
			 SFPTPD_ARGS_SSFTIMESPEC(*diff));
	}

	return rc;
}


enum sfptpd_phc_diff_method sfptpd_phc_get_diff_method(struct sfptpd_phc *phc)
{
	assert(phc != NULL);

	return phc->diff_method;
}


const char *sfptpd_phc_get_diff_method_name(struct sfptpd_phc *phc)
{
	assert(phc != NULL);
	assert(phc->diff_method <= SFPTPD_DIFF_METHOD_MAX);

	return sfptpd_phc_diff_method_text[phc->diff_method];
}


const char *sfptpd_phc_get_pps_method_name(struct sfptpd_phc *phc)
{
	assert(phc != NULL);
	assert(phc->pps_method <= SFPTPD_PPS_METHOD_MAX);

	return sfptpd_phc_pps_method_text[phc->pps_method];
}


void sfptpd_phc_record_step(struct sfptpd_phc *phc)
{
	assert(phc != NULL);

	phc->stepped_since_sample = true;
}


int sfptpd_phc_set_pps_methods(sfptpd_phc_pps_method_t *new_order)
{
	const sfptpd_phc_pps_method_t sentinel = SFPTPD_PPS_METHOD_MAX;
	int i;

	assert(new_order);

	for (i = 0; i <= SFPTPD_PPS_METHOD_MAX; i++) {
		phc_pps_methods[i] = new_order[i];
		if (new_order[i] == sentinel)
			return 0;
	}


	phc_pps_methods[SFPTPD_PPS_METHOD_MAX] = sentinel;
	ERROR("phc: new pps method order too long\n");
	return ERANGE;
}


int sfptpd_phc_enable_pps(struct sfptpd_phc *phc, bool on)
{
	assert(phc != NULL);
	assert(phc->pps_method <= SFPTPD_PPS_METHOD_MAX);

	switch (phc->pps_method) {
	case SFPTPD_PPS_METHOD_DEV_PTP:
		return phc_enable_devptp(phc, on);

	case SFPTPD_PPS_METHOD_DEV_PPS:
		return phc_enable_devpps(phc, on);

	case SFPTPD_PPS_METHOD_MAX:
	default:
		ERROR("phc%d: HW PPS enable requested but no method available\n",
		      phc->phc_idx);
		return EOPNOTSUPP;
	}
}


int sfptpd_phc_get_pps_fd(struct sfptpd_phc *phc)
{
	assert(phc != NULL);
	assert(phc->pps_method <= SFPTPD_PPS_METHOD_MAX);

	switch (phc->pps_method) {
	case SFPTPD_PPS_METHOD_DEV_PTP:
		return phc->phc_fd;
	default:
		return -1;
	}
}


int sfptpd_phc_get_pps_event(struct sfptpd_phc *phc,
			     struct sfptpd_timespec *timestamp, uint32_t *seq)
{
	int rc;

	assert(phc != NULL);
	assert(phc->pps_method <= SFPTPD_PPS_METHOD_MAX);

	switch (phc->pps_method) {
	case SFPTPD_PPS_METHOD_DEV_PTP:
		rc = phc_get_devptp_event(phc, timestamp);
		*seq = UINT32_MAX;
		return rc;

	case SFPTPD_PPS_METHOD_DEV_PPS:
		return phc_get_devpps_event(phc, timestamp, seq);

	case SFPTPD_PPS_METHOD_MAX:
	default:
		ERROR("phc%d: HW PPS event requested but no method available\n",
		      phc->phc_idx);
		return EOPNOTSUPP;
	}
}


void sfptpd_phc_define_diff_method(struct sfptpd_phc *phc,
				   enum sfptpd_phc_diff_method method,
				   sfptpd_phc_diff_fn implementation,
				   void *context)
{
	assert(phc != NULL);
	assert(method < SFPTPD_DIFF_METHOD_MAX);
	assert(method == SFPTPD_DIFF_METHOD_EFX);

	phc->diff_method_defs[method].context = context;
	phc->diff_method_defs[method].diff_fn = implementation;
}


/* fin */
