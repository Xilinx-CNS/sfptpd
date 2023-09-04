/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_ENGINE_H
#define _SFPTPD_ENGINE_H

#include <stdbool.h>

#include "sfptpd_clock.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_link.h"
#include "sfptpd_netlink.h"
#include "sfptpd_servo.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Forward structure declarations */
struct sfptpd_config;
struct sfptpd_thread;
struct sfptpd_engine;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create an sfptpd engine based on the specified configuration
 * @param config  Pointer to configuration structure
 * @param engine  Returned handle to the engine
 * @param netlink  Handle to the netlink client state
 * @param initial_link_table  The initial link table
 * @return 0 on success or an errno otherwise
 */
int sfptpd_engine_create(struct sfptpd_config *config,
			 struct sfptpd_engine **engine,
			 struct sfptpd_nl_state *netlink,
			 const struct sfptpd_link_table *initial_link_table);

/** Destroy sfptpd engine and release resources. The function will
 * send a signal to the engine and wait for it to exit.
 * @param engine  Pointer to engine instance
 */
void sfptpd_engine_destroy(struct sfptpd_engine *engine);

/** Get a handle to the ntp sync module
 * @param engine  Pointer to engine instance
 * @return A pointer to the ntp module or NULL if not found
 */
struct sfptpd_thread *sfptpd_engine_get_ntp_module(struct sfptpd_engine *engine);

/** Get the handles for a sync instance by name
 * @param engine  Pointer to engine instance
 * @param name    The name of the sync instance
 * @return A pointer to the the sync instance information or NULL if not found
 */
const struct sfptpd_sync_instance_info *sfptpd_engine_get_sync_instance_by_name(struct sfptpd_engine *engine,
										const char *name);

/** Step the clocks to the current offset from master. This is typically
 * called in response to receiving a signal from an external entity.
 * This function sends an asynchronous message to engine thread to action the
 * request so is safe to call from another thread context.
 * @param engine  Pointer to engine instance
 */
void sfptpd_engine_step_clocks(struct sfptpd_engine *engine);

/** Select a new sync instance. This is typically called in response
 * to receiving an asynchronous message from an external entity.
 * This function sends an asynchronous message to engine thread to action the
 * request so is safe to call from another thread context.
 * @param engine        Pointer to engine instance
 * @param new_instance  Name of new instance to select
 */
void sfptpd_engine_select_instance(struct sfptpd_engine *engine, const char *new_instance);

/** Signal to the engine that the sync module state has changed and
 * pass the updated status. This will send an asynchronous message to the
 * engine thread so is safe to call from another thread context.
 * @param engine  Pointer to engine instance
 * @param sync_module  Sync module whose state has changed
 * @param sync_instance  Sync instance whose state has changed
 * @param status  Updated sync module status
 */
void sfptpd_engine_sync_instance_state_changed(struct sfptpd_engine *engine,
					       struct sfptpd_thread *sync_module,
					       struct sfptpd_sync_instance *sync_instance,
					       struct sfptpd_sync_instance_status *status);

/** Signal to the engine that a link table has been released.
 * @param engine  Pointer to engine instance
 * @param link_table  Pointer to link table
 */
void sfptpd_engine_link_table_release(struct sfptpd_engine *engine, const struct sfptpd_link_table *link_table);

/** Calculates and returns the clustering score for the sync module
 * based on the mechanism specified in the config.
 * @param engine  Pointer to engine instance
 * @param offset_from_master  The sync instance's offset from master
 * @param instance_clock  The sync instance's clock
 */
int sfptpd_engine_calculate_clustering_score(struct sfptpd_clustering_evaluator *evaluator,
					       sfptpd_time_t offset_from_master,
					       struct sfptpd_clock *instance_clock);

bool sfptpd_engine_compare_clustering_guard_threshold(struct sfptpd_clustering_evaluator *evaluator, int clustering_score);

/** Schedule the insertion or deletion of a leap second to occur at the end
 * of the current UTC day. This will send an asynchronous message to the
 * engine thread so is safe to call from another thread context.
 * @param engine  Pointer to engine instance
 * @param type  Type of leap second
 * @param guard_interval  Guard interval to use either side of leap second in
 * seconds.
 */
void sfptpd_engine_schedule_leap_second(struct sfptpd_engine *engine,
					enum sfptpd_leap_second_type type,
					long double guard_interval);

/** Cancel the currently scheduled leap second (if any). This will send an
 * asynchronous message to the engine thread so is safe to call from another
 * thread context.
 * @param engine  Pointer to engine instance
 */
void sfptpd_engine_cancel_leap_second(struct sfptpd_engine *engine);

/** Test function to set a test mode. Refer to sfptpd_test_id for information
 * on the various test modes available. This will send an asynchronous message
 * to the engine thread so is safe to call from another thread context.
 * @param engine Pointer to engine instance
 * @param test_id Test identifier
 * @param param0 First parameter for test mode
 * @param param1 Second parameter for test mode
 * @param param2 Third parameter for test mode
 */
void sfptpd_engine_test_mode(struct sfptpd_engine *engine,
			     enum sfptpd_test_id test_id,
			     int param0, int param1, int param2);

/** Pack and send a realtime stats message to the engine.
 * This will send an asynchronous message to the
 * engine thread so is safe to call from another thread context.
 * @param engine  Pointer to engine instance
 * @param time The time received in the log stats request
 * @param instance Name of the instance which is generating the stats
 * @param source Clock source (required if clock_master is null)
 * @param clock_master Master clock, may be null
 * @param clock_slave Slave clock, may not be null
 * @param disciplining True if master is disciplining slave clock
 * @param blocked True if master is blocked from disciplining slave clock
 * @param in_sync True if the clocks are in sync
 * @param va_args Pairs of STATS_KEY_x and associated values
 */
void sfptpd_engine_post_rt_stats(struct sfptpd_engine *engine,
		struct sfptpd_log_time *time,
		const char *instance_name,
		const char *source,
		const struct sfptpd_clock *clock_master,
		const struct sfptpd_clock *clock_slave,
		bool disciplining,
		bool blocked,
		bool in_sync,
		sfptpd_sync_module_alarms_t alarms,
		...);

/** Send a realtime stats message to the engine.
 * This will send an asynchronous message to the
 * engine thread so is safe to call from another thread context.
 * @param engine  Pointer to engine instance
 * @param servo The time received in the log stats request
 */
void sfptpd_engine_post_rt_stats_simple(struct sfptpd_engine *engine, 
					struct sfptpd_servo *servo);

/** Post clustering determination inputs to the engine.
 * This will send an asynchronous message to the
 * engine thread so is safe to call from another thread context.
 * @param engine Pointer to the engine instance
 * @param offset_from_master The offset from the remote clock
 * @param offset_valid Whether the offset is valid.
 */
void sfptpd_engine_clustering_input(struct sfptpd_engine *engine,
				    const char *instance_name,
				    struct sfptpd_clock *lrc,
				    sfptpd_time_t offset_from_master,
				    bool offset_valid);

/** Stats keys for va_args of sfptpd_engine_post_rt_stats function */
enum sfptpd_rt_stats_key {     /* Type of following argument */
	STATS_KEY_OFFSET = 0,  /*  sfptpd_time_t             */
	STATS_KEY_FREQ_ADJ,    /*  sfptpd_time_t             */
	STATS_KEY_OWD,         /*  sfptpd_time_t             */
	STATS_KEY_PARENT_ID,   /*  uint8_t [8]               */
	STATS_KEY_GM_ID,       /*  uint8_t [8]               */
	STATS_KEY_PPS_OFFSET,  /*  sfptpd_time_t             */
	STATS_KEY_BAD_PERIOD,  /*  int                       */
	STATS_KEY_OVERFLOWS,   /*  int                       */
	STATS_KEY_ACTIVE_INTF, /*  struct sfptpd_interface*  */
	STATS_KEY_BOND_NAME,   /*  char*                     */
	STATS_KEY_P_TERM,      /*  long double               */
	STATS_KEY_I_TERM,      /*  long double               */
	STATS_KEY_M_TIME,      /*  struct timespec           */
	STATS_KEY_S_TIME,      /*  struct timespec           */
	STATS_KEY_END
};


/** Allow logs to rotate, by closing and re-opening them.
 * This function sends an asynchronous message to engine thread to action the
 * request so is safe to call from another thread context.
 * @param engine  Pointer to engine instance
 */
void sfptpd_engine_log_rotate(struct sfptpd_engine *engine);

#endif /* _SFPTPD_ENGINE_H */
