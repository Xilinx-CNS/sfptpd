/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

#ifndef _SFPTPD_PHC_H
#define _SFPTPD_PHC_H

#include <unistd.h>
#include <time.h>
#include <sys/timex.h>


/****************************************************************************
 * Types and defines
****************************************************************************/

/* Used to identify a null value for a clock ID */
#define POSIX_ID_NULL                    ((clockid_t)-1)


/* Preserve ordering to match strings
IMPORTANT: If you update this list, make sure to update
sfptpd_phc_diff_method_text appropriately.
*/
enum sfptpd_phc_diff_method {
	SFPTPD_DIFF_METHOD_SYS_OFFSET_PRECISE = 0,
	SFPTPD_DIFF_METHOD_EFX,
	SFPTPD_DIFF_METHOD_PPS,
	SFPTPD_DIFF_METHOD_SYS_OFFSET_EXTENDED,
	SFPTPD_DIFF_METHOD_SYS_OFFSET,
	SFPTPD_DIFF_METHOD_READ_TIME,
	SFPTPD_DIFF_METHOD_MAX
};

/* Preserve ordering to match strings */
typedef enum {
	SFPTPD_PPS_METHOD_DEV_PTP = 0,
	SFPTPD_PPS_METHOD_DEV_PPS,
	SFPTPD_PPS_METHOD_MAX
} sfptpd_phc_pps_method_t;


/* Forward declaration of PHC context */
struct sfptpd_phc;

/* Function to perform clock diff */
typedef int (*sfptpd_phc_diff_fn)(void *context, struct timespec *diff);


extern const char *sfptpd_phc_diff_method_text[];
extern const enum sfptpd_phc_diff_method sfptpd_default_phc_diff_methods[SFPTPD_DIFF_METHOD_MAX+1];

extern const char *sfptpd_phc_pps_method_text[];
extern const sfptpd_phc_pps_method_t sfptpd_default_pps_method[SFPTPD_PPS_METHOD_MAX + 1];


/****************************************************************************
 * Function Prototypes
****************************************************************************/

/** Open a PHC device
 * @param phx_idx Index of PHC device to open
 * @param phc Returned PHC context
 * @return 0 on success or an errno otherwise
 */
int sfptpd_phc_open(int phc_index, struct sfptpd_phc **phc);

/** Choose methods and start PHC operations
 * @param phc Handle of the PHC device
 * @return 0 on success or an errno otherwise
 */
int sfptpd_phc_start(struct sfptpd_phc *phc);

/** Close a PHC device
 * @param phc Handle of the PHC device
 */
void sfptpd_phc_close(struct sfptpd_phc *phc);

/** Get the Posix clock ID for the PHC device.
 * @param phc Handle of the PHC device
 * @return The Posix clock ID
 */
clockid_t sfptpd_phc_get_clock_id(struct sfptpd_phc *phc);

/** Determine the maximum frequency adjustment supported by the PHC device
 * @param phc Handle of the PHC device
 * @return The maximum frequency adjustment supported in parts per billion.
 */
int sfptpd_phc_get_max_freq_adj(struct sfptpd_phc *phc);

/** Depending on the mechanism used to carry out the clock comparison there can
 * be a minimum interval between clock comparisons e.g. 1 second. Get the
 * minimum interval or zero if no restriction.
 * @param phc Handle of the PHC device
 * @return Minimum interval between clock comparisons in seconds
 */
int sfptpd_phc_get_clk_compare_interval(struct sfptpd_phc *phc);

/** Compare the PHC clock to the system clock. Specifically, this returns
 *          diff = Tphc - Tsystem
 * @param phc Handle of the PHC device
 * @param diff Difference between PHC and system clock
 * @return 0 on success or an errno otherwise
 */
int sfptpd_phc_compare_to_sys_clk(struct sfptpd_phc *phc, struct timespec *diff);

/** Enable or disable the external PPS input events
 * @param phc Handle of the PHC device
 * @param bool True to enable; false to disable
 * @return 0 on success or an errno otherwise
 */
int sfptpd_phc_enable_pps(struct sfptpd_phc *phc, bool on);


/** Get any PPS fd needed for polling
 * @param phc Handle of the PHC device
 * @return -1 if not necessary or the fd of the PPS device
 */
int sfptpd_phc_get_pps_fd(struct sfptpd_phc *phc);

/** Retrieve an external PPS event
 * @param phc Handle of the PHC device
 * @param timestamp Structure to contain timestamp of retrieved event
 * @param seq_num Variable to hold sequence number
 * @return 0 on success or an errno otherwise
 */
int sfptpd_phc_get_pps_event(struct sfptpd_phc *phc,
			     struct timespec *timestamp, uint32_t *seq_num);

/** The oldest distribution we support has kernel support for PHC but the
 * syscall is missing in that glibc version. To mitigate this we create our
 * own weakly bound version need to of the call clock_adjtime() that simply
 * invokes the syscall directly. See adjtimex() for more details.
 * @param id Posix clock ID for clock to adjust
 * @param tx Time adjustment structure
 * @return A non-negative number on success and -1 on failure. */
int clock_adjtime(clockid_t id, struct timex *tx);

/** Get the method used for clock diff.
 * @param phc Handle of the PHC device
 * @return Diff method
 */
enum sfptpd_phc_diff_method sfptpd_phc_get_diff_method(struct sfptpd_phc *phc);

/** Get the name of the method used for clock diff.
 * @param phc Handle of the PHC device
 * @return Diff method name
 */
const char *sfptpd_phc_get_diff_method_name(struct sfptpd_phc *phc);

/** Get the name of the method used for PPS.
 * @param phc Handle of the PHC device
 * @return PPS method name
 */
const char *sfptpd_phc_get_pps_method_name(struct sfptpd_phc *phc);

/** Record that there has been a clock step in case this affects
 * PHC sampling technique.
 * @param phc Handle of the PHC device
 */
void sfptpd_phc_record_step(struct sfptpd_phc *phc);

/** Set the order of diff methods to use.
 * @param new_order the new list of methods.
 * @return 0 on success or an errno otherwise
 */
int sfptpd_phc_set_diff_methods(const enum sfptpd_phc_diff_method *new_order);

/** Set the order of PPS methods to use.
 * @param new_order the new list of methods.
 * @return 0 on success or an errno otherwise
 */
int sfptpd_phc_set_pps_methods(sfptpd_phc_pps_method_t *new_order);

void sfptpd_phc_define_diff_method(struct sfptpd_phc *phc,
				   enum sfptpd_phc_diff_method method,
				   sfptpd_phc_diff_fn implementation,
				   void *state);

#endif /* _SFPTPD_PHC_H */
