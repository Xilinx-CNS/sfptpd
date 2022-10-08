/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_SERVO_H
#define _SFPTPD_SERVO_H

#include <stdbool.h>
#include <time.h>

#include "sfptpd_time.h" /* sfptpd_time_t */

/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Forward declarations of structures */
struct sfptpd_config;
struct sfptpd_servo;
struct sfptpd_clock;
enum sfptpd_leap_second_type;

struct sfptpd_servo_stats {
	char *servo_name;
	struct sfptpd_clock *clock_master;
	struct sfptpd_clock *clock_slave;
	bool disciplining;
	sfptpd_time_t offset;
	sfptpd_time_t freq_adj;
	bool in_sync;
	sfptpd_sync_module_alarms_t alarms;
	long double p_term;
	long double i_term;
};

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create a clock servo that can be used to sync one clock to another
 * @param config  Pointer to configuration
 * @param idx Servo index, to create a unique name for logging
 * @return Pointer to servo on success or NULL otherwise.
 */
struct sfptpd_servo *sfptpd_servo_create(struct sfptpd_config *config, int idx);

/** Destroy a clock servo and free any resources
 * @param servo  Servo instance to destroy
 * @param in_signal_handler Boolean indicating whether the function is being
 * called from a signal handler
 */
void sfptpd_servo_destroy(struct sfptpd_servo *servo);

/** Reset a clock servo. This reinitialises the servo filters and proportional,
 * intergral and differentiate terms
 * @param servo  Servo instance
 */
void sfptpd_servo_reset(struct sfptpd_servo *servo);

/** Set the master and slave clocks for servo. The servo provides the means to
 * synchronize the slave clock to the master
 * @param servo  Servo instance
 * @param master Master clock - the reference
 * @param slave  Slave clock - the clock to be synchronized
 */
void sfptpd_servo_set_clocks(struct sfptpd_servo *servo, struct sfptpd_clock *master,
			     struct sfptpd_clock *slave);

/** Perform a clock step operation on the servo's slave clock. Step the clock by
 * the offset specified and reset the servo's filters.
 * @param servo  Servo instance
 * @param offset Amount to step the clock
 * @return 0 for success or an errno if the operation failed.
 */
int sfptpd_servo_step_clock(struct sfptpd_servo *servo, struct timespec *offset);

/** Perform a sync operation using the servo. This will perform a single
 * synchronization operation on the slave clock. In order to synchronise the
 * slave clock to the master and keep it in sync, this function should be called
 * periodically at the interval specified in the configuration when the servo
 * was created.
 * @param servo  Servo instance
 * @param time  Current time
 * @return 0 for success or an errno if the sync operation failed.
 */
int sfptpd_servo_synchronize(struct sfptpd_engine *engine, struct sfptpd_servo *servo, struct timespec *time);

/** Get the latest difference between the master and slave clocks
 * @param servo  Servo instance
 * @param offset Difference between master and slave clocks (master - slave)
 */
void sfptpd_servo_get_offset_from_master(struct sfptpd_servo *servo, struct timespec *offset);

/** Get statistics suitable for output to realtime module
 * @param servo Servo instance
 * @return Statistics and clocks
 */
struct sfptpd_servo_stats sfptpd_servo_get_stats(struct sfptpd_servo *servo);

/** Update the NIC clock's sync status
 * @param servo Servo instance
 */
void sfptpd_servo_update_sync_status(struct sfptpd_servo *servo);

/** Save the current state of the servo to file
 * @param servo  Servo instance
 */
void sfptpd_servo_save_state(struct sfptpd_servo *servo);

/** Update statistics collection for servo
 * @param servo  Servo instance
 * @param time Time to record stats against at end of stats period
 */
void sfptpd_servo_stats_end_period(struct sfptpd_servo *servo,
				   struct timespec *time);

/** Write the servo slave clock offset to the topology stream
 * @param servo  Servo instance
 * @param stream Stream to write to
 */
void sfptpd_servo_write_topology_offset(struct sfptpd_servo *servo,
					FILE *stream);

/** Write the servo slave clock name to the topology stream
 * @param servo  Servo instance
 * @param stream Stream to write to
 */
void sfptpd_servo_write_topology_clock_name(struct sfptpd_servo *servo,
					    FILE *stream);

/** Write the servo slave clock hardware ID to the topology stream
 * @param servo  Servo instance
 * @param stream Stream to write to
 */
void sfptpd_servo_write_topology_clock_hw_id(struct sfptpd_servo *servo,
					     FILE *stream);

/** Get the servo alarms set and optionally the name of the servo
 * @param servo Servo instance
 * @param servo_name The pointer to receive the servo name
 * @return The servo alarms
 */
sfptpd_sync_module_alarms_t sfptpd_servo_get_alarms(struct sfptpd_servo *servo,
						    const char **servo_name);

#endif /* _SFPTPD_SERVO_H */
