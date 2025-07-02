/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2012-2025, Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_CLOCK_H
#define _SFPTPD_CLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <net/ethernet.h>

#include <sfptpd_time.h>
#include <sfptpd_phc.h>

/* ANSI/T1.101-1987
 *  Synchronization Interface Standards for Digital Networks */

/* Autonomous source of timing e.g. a caesiam beam clock. Referred to as a
 * Primary Reference Source (PRS) */
#define SFPTPD_CLOCK_STRATUM_1_ACCURACY_PPB   (0.01)

/* A clock that tracks a primary reference source */
#define SFPTPD_CLOCK_STRATUM_2_ACCURACY_PPB   (16.0)
#define SFPTPD_CLOCK_STRATUM_2_HOLDOVER_PPB   (0.1)

/* OCXO - maybe some TCXOs */
#define SFPTPD_CLOCK_STRATUM_3E_ACCURACY_PPB  (1000.0)
#define SFPTPD_CLOCK_STRATUM_3E_HOLDOVER_PPB  (10.0)

/* TCXO */
#define SFPTPD_CLOCK_STRATUM_3_ACCURACY_PPB   (4600.0)
#define SFPTPD_CLOCK_STRATUM_3_HOLDOVER_PPB   (370.0)

/* Many standard crystal oscillators are this good */
#define SFPTPD_CLOCK_STRATUM_4_ACCURACY_PPB   (32000.0)
#define SFPTPD_CLOCK_STRATUM_4_HOLDOVER_PPB   (32000.0)

/* System clock. There isn't a clock stratum that describes just how bad
 * a PC system clock is. */
#define SFPTPD_CLOCK_STRATUM_X_ACCURACY_PPB   (256000.0)
#define SFPTPD_CLOCK_STRATUM_X_HOLDOVER_PPB   (256000.0)

/* NIC clock accuracy and maximum frequency adjustment */
#define SFPTPD_NIC_TCXO_CLOCK_STRATUM    (SFPTPD_CLOCK_STRATUM_3)
#define SFPTPD_NIC_XO_CLOCK_STRATUM      (SFPTPD_CLOCK_STRATUM_4)

/** Enumeration of leap second types */
enum sfptpd_leap_second_type {
	SFPTPD_LEAP_SECOND_NONE,
	SFPTPD_LEAP_SECOND_61,
	SFPTPD_LEAP_SECOND_59,
	SFPTPD_LEAP_SECOND_MAX
};

/** Enumeration defining various clock strata */
enum sfptpd_clock_stratum {
	SFPTPD_CLOCK_STRATUM_1,
	SFPTPD_CLOCK_STRATUM_2,
	SFPTPD_CLOCK_STRATUM_3E,
	SFPTPD_CLOCK_STRATUM_3,
	SFPTPD_CLOCK_STRATUM_4,
	SFPTPD_CLOCK_STRATUM_X,
	SFPTPD_CLOCK_STRATUM_MAX
};

/** Clock classes */
enum sfptpd_clock_class {
	SFPTPD_CLOCK_CLASS_LOCKED,
	SFPTPD_CLOCK_CLASS_HOLDOVER,
	SFPTPD_CLOCK_CLASS_FREERUNNING,
	SFPTPD_CLOCK_CLASS_UNKNOWN,
	SFPTPD_CLOCK_CLASS_MAX
};

/** Time sources with values from IEEE1588-2008 Table 7 */
enum sfptpd_time_source {
	SFPTPD_TIME_SOURCE_ATOMIC_CLOCK = 0x10,
	SFPTPD_TIME_SOURCE_GPS = 0x20,
	SFPTPD_TIME_SOURCE_TERRESTRIAL_RADIO = 0x30,
	SFPTPD_TIME_SOURCE_PTP = 0x40,
	SFPTPD_TIME_SOURCE_NTP = 0x50,
	SFPTPD_TIME_SOURCE_HANDSET = 0x60,
	SFPTPD_TIME_SOURCE_OTHER = 0x90,
	SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR = 0xA0,
};

enum sfptpd_clock_adj_method {
	SFPTPD_CLOCK_PREFER_TICKADJ,
	SFPTPD_CLOCK_PREFER_FREQADJ,
};

#define SFPTPD_CLOCK_SHORT_NAME_SIZE 16
#define SFPTPD_CLOCK_FULL_NAME_SIZE 64
#define SFPTPD_CLOCK_HW_ID_SIZE 8
#define SFPTPD_CLOCK_HW_ID_STRING_SIZE 32

/** Structure defining a clock ID
 * @clock_id Array containing EUI64 format ID
 */
typedef struct sfptpd_clock_id {
	uint8_t id[SFPTPD_CLOCK_HW_ID_SIZE];
} sfptpd_clock_id_t;

/* Declare the uninitialised clock identity */
extern const struct sfptpd_clock_id SFPTPD_CLOCK_ID_UNINITIALISED;

/** Forward declaration of structures */
struct sfptpd_clock;
struct sfptpd_config;


/****************************************************************************
 * Macros and inline functions for convenience operations
****************************************************************************/

static inline int sfclock_gettime(clockid_t clk_id, struct sfptpd_timespec *sfts)
{
	struct timespec ts;
	int rc = clock_gettime(clk_id, &ts);
	sfts->sec = ts.tv_sec;
	sfts->nsec = ts.tv_nsec;
	sfts->nsec_frac = 0;
	return rc;
}

static inline int sfclock_nanosleep(clockid_t clk_id, int flags,
				    const struct sfptpd_timespec *sfrequest,
				    struct sfptpd_timespec *sfremain)
{
	struct timespec request = { .tv_sec = sfrequest->sec,
				    .tv_nsec = sfrequest->nsec };
	struct timespec remain;
	int rc = clock_nanosleep(clk_id, flags, &request, sfremain ? &remain : NULL);
	if (sfremain) {
		sfremain->sec = remain.tv_sec;
		sfremain->nsec = remain.tv_nsec;
		sfremain->nsec_frac = 0;
	}
	return rc;
}


/****************************************************************************
 * Function Prototypes
****************************************************************************/

/** Find all hardware clocks and open them. Creates set of sfptpd_clock structures,
 * one for each hardware clock. This should be called after calling
 * sfptpd_interface_initialise().
 * @param config Pointer to configuration structure
 * @param hardware_state_lock Pointer to shared hardware mutex
 * @return 0 for success or an errno on failure.
 */
int sfptpd_clock_initialise(struct sfptpd_config *config, pthread_mutex_t *hardware_state_lock);

/** Release any resources associated with the clock objects
 */
void sfptpd_clock_shutdown(void);

/** Get the number of clocks in the system including PTP hardware clocks and
 * the system clock.
 * @return The number of clocks 
 */
int sfptpd_clock_get_total(void);

/** Get a snapshot of the active clocks ordered by clock pointer.
 * @param num_clocks Set to the number of active clocks in snapshot.
 * @return A pointer to an array of clock pointers.
 */
struct sfptpd_clock **sfptpd_clock_get_active_snapshot(size_t *num_clocks);

/** Free a snapshot of clock pointers.
 * @param snapshot The snapshot to be freed.
 */
void sfptpd_clock_free_active_snapshot(struct sfptpd_clock **snapshot);

/** Find a clock by name 
 * @param name  Textual name of clock
 * @return A pointer to the clock instance or NULL if not found.
 */
struct sfptpd_clock *sfptpd_clock_find_by_name(const char *name);

/** Find a clock by HW ID
 * @param hw_id  Pointer to HW ID
 * @return A pointer to the clock instance or NULL if not found.
 */
struct sfptpd_clock *sfptpd_clock_find_by_hw_id(sfptpd_clock_id_t *hw_id);

/** Get the system clock
 * @return A pointer to the clock instance for the CPU realtime clock
 */
struct sfptpd_clock *sfptpd_clock_get_system_clock(void);

/** Check if two clock IDs are equal
 * @param id1 ID of fisrt clock
 * @param id2 ID of second clock
 * @return A boolean indicating if the clock IDs are equal
 */
bool sfptpd_clock_ids_equal(sfptpd_clock_id_t *id1,
			    sfptpd_clock_id_t *id2);

/** Get a textual representation of a clock class
 * @param clock_class Clock class enum value
 * @return A textual representation of the clock class
 */
const char *sfptpd_clock_class_text(enum sfptpd_clock_class clock_class);

/** Get a textual representation of a clock time source
 * @param time_source Clock time source enum value
 * @return A textual representation of the clock time source
 */
const char *sfptpd_clock_time_source_text(enum sfptpd_time_source time_source);


/** Load a saved frequency correction from file and set the clock frequency
 * adjustment. If no state file exists or the information is determined to be
 * invalid, the frequency correction is set to zero.
 * @param clock  Pointer to clock instance
 * @param freq_correction_ppb  The restored frequency correction or 0.0 if no
 * saved value is available
 * @return 0 for success or ENODATA if no saved value is available.
 */
int sfptpd_clock_load_freq_correction(struct sfptpd_clock *clock,
				      long double *freq_correction_ppb);

/** Save a frequency correction value for a clock to file.
 * @param clock  Pointer to clock instance
 * @param freq_correction_ppb Frequency correction to save
 * @return 0 for success or ERANGE if current frequency adjustment outside
 * valid range for the clock.
 */
int sfptpd_clock_save_freq_correction(struct sfptpd_clock *clock,
				      long double freq_correction_ppb);

/** Get the last saved/loaded frequency correction. Note that this is
 * different from the current frequency adjustment and represents the long
 * term good value.
 * @param clock  Pointer to clock instance
 * @return The current frequency correction in parts-per-billion
 */
long double sfptpd_clock_get_freq_correction(struct sfptpd_clock *clock);



/** Record the offset from master and whether the clock is believed to be 
 * synchronized. Used to gather long term stats about the clock
 * @param clock  Clock instance
 * @param offset Current offset from reference clock
 * @param synchronized Boolean indicating whether clock is currently considered
 * to be synchronized to its reference clock
 */
void sfptpd_clock_stats_record_offset(struct sfptpd_clock *clock,
				      long double offset, bool synchronized);


/** Records whether the clock is near epoch.
 * @param clock  Clock instance
 * @param near_epoch Boolean indicating whether clock is currently near epoch
 */
void sfptpd_clock_stats_record_epoch_alarm(struct sfptpd_clock *clock,
					   bool near_epoch);


/** Records whether the clock is within clustering guard threshold.
 * @param clock  Clock instance
 * @param out_of_threshold Boolean indicating whether clock is out of clustering threshold.
 */
void sfptpd_clock_stats_record_clustering_alarm(struct sfptpd_clock *clock,
					   bool out_of_threshold);


/** Used to signal to the clock object that it is the end of the current
 * statisticst period. The statistics are updated and dumped to file
 * @param clock  Clock instance
 * @param time Time to record stats against at end of stats period
 */
void sfptpd_clock_stats_end_period(struct sfptpd_clock *clock,
				   struct sfptpd_timespec *time);


/** Get the short name of a clock instance - this is just the clock name
 * @param clock  Pointer to clock instance
 * @return The name of the clock or NULL in the case of an error.
 */
const char *sfptpd_clock_get_short_name(const struct sfptpd_clock *clock);

/** Get the long name of a clock instance - this combines the clock name
 * and the associated interface names
 * @param clock  Pointer to clock instance
 * @return The name of the clock or NULL in the case of an error.
 */
const char *sfptpd_clock_get_long_name(const struct sfptpd_clock *clock);

/** Get the clock ID associated with this clock
 * @param clock Pointer to clock instance
 * @return 0 or errno
 */
int sfptpd_clock_get_hw_id(const struct sfptpd_clock *clock,
			   sfptpd_clock_id_t *hw_id);

/** Get the formatted string form of the clock ID associated with this clock
 * @param clock Pointer to clock instance
 * @return String of clock ID
 */
const char *sfptpd_clock_get_hw_id_string(const struct sfptpd_clock *clock);

/** Get the formatted filename form of the clock ID associated with this clock
 * @param clock Pointer to clock instance
 * @return String to use in filenames labelling clock
 */
const char *sfptpd_clock_get_fname_string(const struct sfptpd_clock *clock);

/** Create formatted string from hw_id
 * @param buf Pointer to buffer to print string into
 * @param hw_id HW ID to format
 * @return Pointer to filled buffer
 */
void sfptpd_clock_init_hw_id_string(char *buf, const sfptpd_clock_id_t hw_id,
				     int max_len);

/** Get the primary interface assoicated with this clock. Only valid
 * for PTP (non-system) clocks.
 * @param clock  Pointer to clock instance
 * @return A handle to the primary interface associated with the clock instance.
 */
struct sfptpd_interface *sfptpd_clock_get_primary_interface(const struct sfptpd_clock *clock);

/** Get the clock accuracy. This is indication of the maximum frequency error
 * of the clock.
 * @param clock  Pointer to clock instance
 * @param stratum Returned clock stratum
 * @param accuracy Returned clock accuracy in parts-per-billion
 * @param holdover Returned clcok holdover in parts-per-billion
 */
void sfptpd_clock_get_accuracy(struct sfptpd_clock *clock,
			       enum sfptpd_clock_stratum *stratum,
			       long double *accuracy,
			       long double *holdover);

/** Get the maximum frequency adjustment that this clock supports.
 * @param clock  Pointer to clock instance
 * @return The maximum frequency adjustment in parts-per-billion
 */
long double sfptpd_clock_get_max_frequency_adjustment(struct sfptpd_clock *clock);

/** Get the discipline flag which indicates that a servo should discipline
 * the clock.
 * @param clock  Pointer to clock instance
 * @return A boolean indicating if the clock should be disciplined
 */
bool sfptpd_clock_get_discipline(struct sfptpd_clock *clock);

/** Get the discipline flag which indicates that a servo should observe
 * the clock.
 * @param clock  Pointer to clock instance
 * @return A boolean indicating if the clock should be observed
 */
bool sfptpd_clock_get_observe(struct sfptpd_clock *clock);

/** Adjust the clock instance by the specified offset
 * @param clock  Pointer to clock instance
 * @param offset Offset to be applied in seconds and nanoseconds
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_clock_adjust_time(struct sfptpd_clock *clock, struct sfptpd_timespec *offset);

/** Adjust the clock instance by the specified frequency
 * @param clock  Pointer to clock instance
 * @param freq_adj_ppb Frequency adjustment to be applied in parts per billion
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_clock_adjust_frequency(struct sfptpd_clock *clock, long double freq_adj_ppb);

/** Get the clock time
 * @param clock  Pointer to clock instance
 * @param time   Pointer to structure where clock time will be written
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_clock_get_time(const struct sfptpd_clock *clock, struct sfptpd_timespec *time);

/** Get the clock frequency
 * @param clock     Pointer to clock instance
 * @param freq_adj  Pointer to where frequency adjustment in ppb will be written
 * @param tick      Pointer to where tick lenth in ns will be written
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_clock_get_frequency(struct sfptpd_clock *clock,
			       sfptpd_time_t *freq_adj,
			       sfptpd_time_t *tick_len);

/** Schedule or deschedule a leap second for midnight today UTC for all
 * clocks that support leap second scheduling
 * @param type Leap second type
 * @return 0 on success or errno status code
 */
int sfptpd_clock_schedule_leap_second(enum sfptpd_leap_second_type type);

/** Leap second now. Step all clocks that don't support leap second
 * scheduling
 * @param type Leap second type
 * @return 0 on success or errno on error
 */
int sfptpd_clock_leap_second_now(enum sfptpd_leap_second_type type);

/** Compare the difference between two clock instances- evaluates clock1 - clock2
 * @param clock1  Pointer to first clock instance
 * @param clock2  Pointer to second clock instance
 * @param diff    Pointer to structure where clock difference will be stored
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_clock_compare(struct sfptpd_clock *clock1, struct sfptpd_clock *clock2,
			 struct sfptpd_timespec *diff);

/** Set one clock to another using differences. This should be used in
 *  preference to the caller performing compare and adjustment operations to
 *  avoid a race window that could result in double adjustment.
 * @param clock_to    Pointer to the clock to set
 * @param clock_from  Pointer to the clock to use as a reference
 * @param threshold   A threshold which below which the clock difference
 *		      should not trigger an adjustment or NULL.
 * @param is_initial_correction This is an initial clock correction
 *                    and should not be repeated if already done.
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_clock_set_time(struct sfptpd_clock *clock_to,
			  struct sfptpd_clock *clock_from,
			  const struct sfptpd_timespec *threshold,
			  bool is_initial_correction);

/** Report the sync status to the NIC associated with the clock. This is used
 * by the NIC firmware to report the sync status to other interested parties
 * using the NIC resources.
 * @param clock Pointer to the clock instance
 * @param in_sync Boolean indicating if the clock is considered in sync with
 * its reference
 * @param timeout Time until a synchronized clock should be considered no
 * longer in sync.
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_clock_set_sync_status(struct sfptpd_clock *clock, bool in_sync,
				 unsigned int timeout);


/** Enable or disable external PPS input on the NIC associated with the clock
 * @param clock Pointer to the clock instance
 * @param pin Pin to control
 * @param function Function to apply to the pin
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_clock_pps_configure(struct sfptpd_clock *clock,
			       int pin,
			       enum sfptpd_phc_pin_func function);

/** Get the fd to wait on for PPS events if necessary
 * @param clock pointer to the clock instance
 * @return -1 if no polling necessary, otherwise the fd to poll
 */
int sfptpd_clock_pps_get_fd(struct sfptpd_clock *clock);

/** Get the last PPS event from the NIC associated with the clock
 * @param clock Pointer to the clock instance
 * @param sequence_num Returned sequence number of PPS event
 * @param time NIC time at which the PPS pulse was detected
 * @return 0 for success, EAGAIN if no PPS event is available to
 * read. Otherwise an errno status code.
 */
int sfptpd_clock_pps_get(struct sfptpd_clock *clock, uint32_t *sequence_num,
			 struct sfptpd_timespec *time);

/**
 * For a new clock, load the frequency correction if configured to do
 * so. For each NIC clock, check whether the NIC clock has ever been
 * set and if not, set it to the current system time.
 * @param clock Pointer to the clock instance
 */
void sfptpd_clock_correct_new(struct sfptpd_clock *clock);

/** Update clocks based on the latest interface list */
void sfptpd_clock_rescan_interfaces(void);

/** Fix up the discipline and read_only flags for all the clocks */
void fixup_readonly_and_clock_lists(void);

/** Report the current clock list
 * @param trace_level the sfptpd trace level to use
 */
void sfptpd_clock_diagnostics(int trace_level);

/** Whether sfptpd will write to a given clock
 * @param clock the clock to check
 * @return true if the clock is disciplined and not read-only
 */
bool sfptpd_clock_is_writable(struct sfptpd_clock *clock);

/** Add or subtract from reference count of temporary blocks on
 * a clock. If blocked, a clock will not be adjusted by sfptpd.
 * @param block whether to block or unblock
 * @return true if the clock is now temporarily blocked
 */
bool sfptpd_clock_set_blocked(struct sfptpd_clock *clock, bool block);

/** Is given clock currently blocked
 * @param clock the clock to check
 * @return true if the clock is currently blocked
 */
bool sfptpd_clock_is_blocked(const struct sfptpd_clock *clock);

/** Get the lrc_been_locked flag of a clock instance
 * @param clock  Pointer to clock instance
 * @return The value of lrc_been_locked
 */
bool sfptpd_clock_get_been_locked(const struct sfptpd_clock *clock);

/** Set the lrc_been_locked flag of a clock instance
 * @param clock  Pointer to clock instance
 */
void sfptpd_clock_set_been_locked(struct sfptpd_clock *clock, bool value);

/** Get the clock diff method in use
 * @return The clock diff method
 */
const char *sfptpd_clock_get_diff_method(struct sfptpd_clock *clock);

/** Get the PPS method in use
 * @return The PPS method
 */
const char *sfptpd_clock_get_pps_method(struct sfptpd_clock *clock);

/** Is the given clock the system clock
 * @param clock the clock to check
 * @return true if the clock is the system clock
 */
bool sfptpd_clock_is_system(const struct sfptpd_clock *clock);

/** Is the given clock active
 * @param clock the clock to check
 * @return true if the clock is active
 */
bool sfptpd_clock_is_active(const struct sfptpd_clock *clock);

/** Deduplicate phc devices sharing underlying clock
 * @return 0 on success else error code
 */
int sfptpd_clock_deduplicate(void);

#endif /* _SFPTPD_CLOCK_H */
