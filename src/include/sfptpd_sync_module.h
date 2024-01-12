/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

#ifndef _SFPTPD_SYNC_MODULE_H
#define _SFPTPD_SYNC_MODULE_H

#include <stdint.h>
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>

#include "sfptpd_message.h"
#include "sfptpd_logging.h"
#include "sfptpd_misc.h"
#include "sfptpd_test.h"
#include "sfptpd_constants.h"
#include "sfptpd_config.h"
#include "sfptpd_clock.h"
#include "sfptpd_time.h"
#include "sfptpd_link.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

struct sfptpd_clustering_evaluator {

	/* Context for component providing clustering services, i.e. engine */
	void *private;

	/* Name of sync instance that is clustering candidate */
	const char *instance_name;

	/* Function to calculate a clustering score */
	int (*calc_fn)(struct sfptpd_clustering_evaluator *evaluator,
		       sfptpd_time_t offset_from_master,
		       struct sfptpd_clock *instance_clock);

	/* Function to check clustering score against guard threshold */
	bool (*comp_fn)(struct sfptpd_clustering_evaluator *evaluator,
			int current_clustering_score);

};

/** Forward structure declarations */
struct sfptpd_sync_module;
struct sfptpd_sync_instance;
struct sfptpd_config;
struct sfptpd_clock;
struct sfptpd_engine;
struct sfptpd_log_time;
struct sfptpd_timespec;

/** Flag indicating that the sync module instance is selected */
#define SYNC_MODULE_SELECTED               (1<<0)
/** Flag indicating that the sync module instance should process timestamps */
#define SYNC_MODULE_TIMESTAMP_PROCESSING   (1<<1)
/** Flag indicating that the sync module instance should discipline it's clock */
#define SYNC_MODULE_CLOCK_CTRL             (1<<2)
/** Flag indicating that the leap second guard is in place */
#define SYNC_MODULE_LEAP_SECOND_GUARD      (1<<3)
/** Flag indicating that the sync module instance is a determinant for clustering */
#define SYNC_MODULE_CLUSTERING_DETERMINANT (1<<4)
//** Type used to hold a bitmask of control flags */
typedef unsigned int sfptpd_sync_module_ctrl_flags_t;

/** Default value of control flags for sync instances. */
#define SYNC_MODULE_CTRL_FLAGS_DEFAULT    (SYNC_MODULE_TIMESTAMP_PROCESSING)

/** Enumeration defining the possible sync module states */
typedef enum sfptpd_sync_module_state {
	SYNC_MODULE_STATE_LISTENING,
	SYNC_MODULE_STATE_SLAVE,
	SYNC_MODULE_STATE_MASTER,
	SYNC_MODULE_STATE_PASSIVE,
	SYNC_MODULE_STATE_DISABLED,
	SYNC_MODULE_STATE_FAULTY,
	SYNC_MODULE_STATE_SELECTION,
	SYNC_MODULE_STATE_MAX
} sfptpd_sync_module_state_t;

/** Array of names in the same order as enum sfptpd_sync_module_state */
extern const char *sync_module_state_text[];

/** PTP: Indicates that Sync packets are not being received */
#define SYNC_MODULE_ALARM_NO_SYNC_PKTS               (1<<0)
/** PTP: Indicates that FollowUp packets are not being received */
#define SYNC_MODULE_ALARM_NO_FOLLOW_UPS              (1<<1)
/** PTP: Indicates that DelayResponse packets are not being received */
#define SYNC_MODULE_ALARM_NO_DELAY_RESPS             (1<<2)
/** PTP: Indicates that PeerDelayResponse packets are not being received */
#define SYNC_MODULE_ALARM_NO_PDELAY_RESPS            (1<<3)
/** PTP: Indicates that PeerDelayResponseFollowUp packets are not being received */
#define SYNC_MODULE_ALARM_NO_PDELAY_RESP_FOLLOW_UPS  (1<<4)
/** PTP: Indicates that Transmit Timestamps are missing */
#define SYNC_MODULE_ALARM_NO_TX_TIMESTAMPS           (1<<5)
/** PTP: Indicates that Receive Timestamps are missing */
#define SYNC_MODULE_ALARM_NO_RX_TIMESTAMPS           (1<<6)
/** PPS: No PPS signal detected on input */
#define SYNC_MODULE_ALARM_PPS_NO_SIGNAL              (1<<7)
/** PPS: Sequence number error detected in PPS events */
#define SYNC_MODULE_ALARM_PPS_SEQ_NUM_ERROR          (1<<8)
/** PPS: No time of day is available */
#define SYNC_MODULE_ALARM_NO_TIME_OF_DAY             (1<<9)
/** PPS: Bad PPS pulses are being detected */
#define SYNC_MODULE_ALARM_PPS_BAD_SIGNAL             (1<<10)
/** PTP: Indicates no active interfaces in bond */
#define SYNC_MODULE_ALARM_NO_INTERFACE               (1<<11)
/** Any: Indicates clock control failure */
#define SYNC_MODULE_ALARM_CLOCK_CTRL_FAILURE         (1<<12)
/** Any: Indicates clock time is near epoch */
#define SYNC_MODULE_ALARM_CLOCK_NEAR_EPOCH           (1<<13)
/** PTP: Indicates no common capabilities set */
#define SYNC_MODULE_ALARM_CAPS_MISMATCH              (1<<14)
/** Any: Indicates time is out of clustering guard threshold */
#define SYNC_MODULE_ALARM_CLUSTERING_THRESHOLD_EXCEEDED (1<<15)
/** Any: Sustained clock comparison failure */
#define SYNC_MODULE_ALARM_SUSTAINED_SYNC_FAILURE     (1<<16)
/** Invalid: any alarm value >= to this will trigger an assertion failure! */
#define SYNC_MODULE_ALARM_MAX                        (1<<17)

/** Type used to hold a bitmask of alarms */
typedef unsigned int sfptpd_sync_module_alarms_t;

/** Test if an alarm is set */
#define SYNC_MODULE_ALARM_TEST(s, a)     (((s) & SYNC_MODULE_ALARM_##a) != 0)
/** Set an alarm */
#define SYNC_MODULE_ALARM_SET(s, a)      ((s) = (s) | SYNC_MODULE_ALARM_##a)
/** Clear an alarm */
#define SYNC_MODULE_ALARM_CLEAR(s, a)    ((s) = (s) & ~(SYNC_MODULE_ALARM_##a))


/** A reasonable size to hold all alarm text that needs rendering */
#define SYNC_MODULE_ALARM_ALL_TEXT_MAX		   (300)


/** Default sync instance priority */
#define SFPTPD_DEFAULT_PRIORITY                    (128)


/** Bit field of sync modules providing NTP */
#define SFPTPD_SYNC_MODULE_IS_NTP ( \
	(1 << SFPTPD_CONFIG_CATEGORY_NTP) | \
	(1 << SFPTPD_CONFIG_CATEGORY_CRNY) | \
	0)


/** Sync instance externally constrained always to control clock */
#define SYNC_MODULE_CONSTRAINT_MUST_BE_SELECTED      (1<<0)
/** Sync instance externally constrained never to control clock */
#define SYNC_MODULE_CONSTRAINT_CANNOT_BE_SELECTED    (1<<1)
/** Invalid: any constraint value >= to this will trigger an assertion failure! */
#define SYNC_MODULE_CONSTRAINT_MAX                   (1<<2)

/** Type used to hold bitfield of external constraint flags */
typedef unsigned int sfptpd_sync_module_constraints_t;

/** Test if an external constaint flag is set */
#define SYNC_MODULE_CONSTRAINT_TEST(s, a)     (((s) & SYNC_MODULE_CONSTRAINT_##a) != 0)
/** Set an external constraint */
#define SYNC_MODULE_CONSTRAINT_SET(s, a)      ((s) = (s) | SYNC_MODULE_CONSTRAINT_##a)
/** Clear an external constraint */
#define SYNC_MODULE_CONSTRAINT_CLEAR(s, a)    ((s) = (s) & ~(SYNC_MODULE_CONSTRAINT_##a))


/** A reasonable size to hold all alarm text that needs rendering */
#define SYNC_MODULE_CONSTRAINT_ALL_TEXT_MAX		   (80)


/** Information about the currently selected Grandmaster
 * @clock_id Clock identity of grandmaster
 * @remote_clock Indicates that the grandmaster is a remote clock i.e.
 * corresponds to a sync instance synced to a remote time source.
 * @clock_class Advertised clock class of grandmaster
 * @time_source Source of time used by the grandmaster
 * @accuracy Absolute error bound between grandmaster clock and
 * the primary time source. The value infinity should be used if the accuracy
 * is unknown.
 * @allan_variance Frequency stability of grandmaster clock
 * @steps_removed Number of steps (e.g. boundary clocks or peers) between
 * grandmaster and local reference clock
 * @time_traceable whether the time is traceable to a primary source
 * @freq_traceable whether the frequency is traceable to a primary source
 */
struct sfptpd_grandmaster_info {
	struct sfptpd_clock_id clock_id;
	bool remote_clock;
	enum sfptpd_clock_class clock_class;
	enum sfptpd_time_source time_source;
	long double accuracy;
	long double allan_variance;
	unsigned int steps_removed;
	bool time_traceable;
	bool freq_traceable;
};

/** Summary of sync instance status
 * @state Current state of the sync instance
 * @alarms Bitmask of any alarms currently triggered
 * @clock Handle of the clock that this instance is using
 * @offset_from_parent Current offset between immediate remote clock and the
 * local reference clock
 * @user_priority User-specified priority of the sync instance. Smaller values
 * have higher priority.
 * @master Structure containing info about the grandmaster
 * @clustering_score Score returned by the clustering function. Used for the
 * clustering rule. Higher values have higher priority.
 */
typedef struct sfptpd_sync_instance_status {
	sfptpd_sync_module_state_t state;
	sfptpd_sync_module_alarms_t alarms;
	sfptpd_sync_module_constraints_t constraints;
	struct sfptpd_clock *clock;
	struct sfptpd_timespec offset_from_master;
	unsigned int user_priority;
	struct sfptpd_grandmaster_info master;
	long double local_accuracy;
	int clustering_score;
} sfptpd_sync_instance_status_t;

/** Static sync instance information
 * @module Owning sync module
 * @handle Handle of sync module instance
 * @name The name of the instance
 */
typedef struct sfptpd_sync_instance_info {
	struct sfptpd_thread *module;
	struct sfptpd_sync_instance *handle;
	const char *name;
} sfptpd_sync_instance_info_t;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create global configurations for each sync module
 * @param config  Pointer to the configuration
 * @return 0 on success or an errno otherwise
 */
int sfptpd_sync_module_config_init(struct sfptpd_config *config);

/** Set the default interface to be used by sync modules. This is supported
 * to allow the interface to be specified on the command line which is
 * convenient for users and non-ambiguous for simple configurations.
 * @param config  Pointer to the configuration
 * @param interface_name  Default interface
 */
void sfptpd_sync_module_set_default_interface(struct sfptpd_config *config,
					      const char *interface_name);

/** Set the default PTP domain to be used by sync modules. This is supported
 * to allow the interface to be specified on the command line which is
 * convenient for users and non-ambiguous for simple configurations.
 * @param config  Pointer to the configuration
 * @param ptp_domain  Default PTP domain
 */
void sfptpd_sync_module_set_default_ptp_domain(struct sfptpd_config *config,
					       int domain);

/** Convert a set of control flags into a textual string
 * @param flags  Bitmask of flags
 * @param buffer  Pointer to buffer to store textual representation
 * @param size  Size of output buffer.
 */
void sfptpd_sync_module_ctrl_flags_text(sfptpd_sync_module_ctrl_flags_t flags,
					char *buffer, unsigned int buffer_size);

/** Writes a textual representation of a set of alarms to a stream.
 * @param alarm Bitmask of alarms.
 * @param separator Separator printed between each alarm text (if >1).
 * @return number of characters written.
 */
size_t sfptpd_sync_module_alarms_stream(FILE *stream,
	sfptpd_sync_module_alarms_t alarms, const char *separator);

/** Convert a set of alarms into a textual string
 * @param alarms  Bitmask of alarms
 * @param buffer  Pointer to buffer to store textual representation
 * @param size  Size of output buffer.
 */
void sfptpd_sync_module_alarms_text(sfptpd_sync_module_alarms_t alarms,
				    char *buffer, unsigned int buffer_size);

/** Convert a set of external constraints into a textual string
 * @param alarms  Bitmask of constraints
 * @param buffer  Pointer to buffer to store textual representation
 * @param size  Size of output buffer.
 */
void sfptpd_sync_module_constraints_text(sfptpd_sync_module_constraints_t constraints,
					 char *buffer, unsigned int buffer_size);

/** Get the name of a sync module (not a sync instance) from the category type
 * @param type Type of the sync module
 * @return The textual name of the sync module
 */
const char *sfptpd_sync_module_name(enum sfptpd_config_category type);

/** Compare two Grandmaster Info structures.
 * @param gm1 Pointer to Grandmaster Info structure
 * @param gm2 Pointer to Grandmaster Info structure
 * @return True if the two structure are equal
 */
bool sfptpd_sync_module_gm_info_equal(struct sfptpd_grandmaster_info *gm1,
				      struct sfptpd_grandmaster_info *gm2);

/** Create a sync module with the specified name based on the configuration supplied
 * @param type Type of sync module
 * @param config Pointer to configuration
 * @param engine Pointer to sync engine
 * @param sync_module Returned pointer to created sync module
 * @param instance_info_buffer To be populated by information on each sync
 * instance or NULL not to obtain this information.
 * @param instance_info_entries Number of entries in instance_info_buffer. Must
 * be big enough for all entries if the buffer pointer is not NULL.
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_sync_module_create(enum sfptpd_config_category type,
			      struct sfptpd_config *config,
			      struct sfptpd_engine *engine,
			      struct sfptpd_thread **sync_module,
			      struct sfptpd_sync_instance_info *instance_info_buffer,
			      int instance_info_entries,
			      const struct sfptpd_link_table *link_table,
			      bool *link_subscriber);

/** Destroy a sync module
 * @param sync_module Pointer to sync module
 */
void sfptpd_sync_module_destroy(struct sfptpd_thread *sync_module);

/** Get the current status of the sync module.
 * @param sync_module Pointer to sync module
 * @param sync_instance Pointer to sync instance
 * @param status Returned sync module status
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_sync_module_get_status(struct sfptpd_thread *sync_module,
				  struct sfptpd_sync_instance *sync_instance,
				  struct sfptpd_sync_instance_status *status);

/** Control what functions of the sync module are active.
 * @param sync_module Pointer to the sync module
 * @param sync_instance Pointer to sync instance
 * @param flags Bitmask of which functions should be enabled and disabled
 * @param mask Bitmask of which functions should be modified
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_sync_module_control(struct sfptpd_thread *sync_module,
			       struct sfptpd_sync_instance *sync_instance,
			       sfptpd_sync_module_ctrl_flags_t flags,
			       sfptpd_sync_module_ctrl_flags_t mask);

/** Communicates to the sync module updated information about the grandmaster
 * of the currently selected sync instance. The information is typically only
 * useful to sync instances providing a time service to downstream slaves e.g.
 * PTP instances operating as a master.
 * @param sync_module Pointer to the sync module
 * @param originator Handle of instance that originated the update.
 * @param info Pointer to structure containing grandmaster info
 */
void sfptpd_sync_module_update_gm_info(struct sfptpd_thread *sync_module,
				       struct sfptpd_sync_instance *originator,
				       struct sfptpd_grandmaster_info *info);

/** Communicates to the sync module updated information about the leap second
 * status of the currently selected sync instance. The information is typically
 * only useful to sync instances providing a time service to downstream slaves
 * e.g. PTP instances operating as a master.
 * @param sync_module Pointer to the sync module
 * @param leap_second_type Current leap second state
 */
void sfptpd_sync_module_update_leap_second(struct sfptpd_thread *sync_module,
					   enum sfptpd_leap_second_type leap_second_type);

/** Step the sync module clock to the specified offset
 * @param sync_module Pointer to sync module
 * @param sync_instance Pointer to sync instance
 * @param offset Amount by which to step the clock
 * @return 0 for success or an errno if the operation fails
 */
int sfptpd_sync_module_step_clock(struct sfptpd_thread *sync_module,
				  struct sfptpd_sync_instance *sync_instance,
				  struct sfptpd_timespec *offset);

/** Write statistics information about current state of sync module to stdout
 * @param sync_module Pointer to sync module
 */
void sfptpd_sync_module_log_stats(struct sfptpd_thread *sync_module,
				  struct sfptpd_log_time *time);

/** Save current state of the sync module to file
 * @param sync_module Pointer to sync module
 */
void sfptpd_sync_module_save_state(struct sfptpd_thread *sync_module);

/** Based on the current state, write clock topology to the supplied stream
 * @param sync_module Pointer to sync module
 * @param sync_instance Pointer to sync instance
 * @param stream Handle to stream to write topology to
 */
void sfptpd_sync_module_write_topology(struct sfptpd_thread *sync_module,
				       struct sfptpd_sync_instance *sync_instance,
				       FILE *stream);

/** Signal the end of the current stats period. Update historical stats and
 * write to file.
 * @param sync_module Pointer to sync module
 * @param time Time to record stats against at end of stats period
 */
void sfptpd_sync_module_stats_end_period(struct sfptpd_thread *sync_module,
					 struct sfptpd_timespec *time);

/** Configure a test mode in the sync module
 * @param sync_module Pointer to sync module
 * @param sync_instance Pointer to sync instance
 * @param id Test identifier
 * @param param0 First parameter for test mode
 * @param param1 Second parameter for test mode
 * @param param2 Third parameter for test mode
 */
void sfptpd_sync_module_test_mode(struct sfptpd_thread *sync_module,
				  struct sfptpd_sync_instance *sync_instance,
				  enum sfptpd_test_id id, int param0,
				  int param1, int param2);


/** Notify a sync module of a new link table
 * @param sync_module Pointer to sync module
 * @param link_table Pointer to the link table
 */
void sfptpd_sync_module_link_table(struct sfptpd_thread *sync_module,
				   const struct sfptpd_link_table *link_table);


/****************************************************************************
 * Sync Module Messages - All Sync Modules should implement these
 ****************************************************************************/

/** Macro used to define message ID values for sync module messages */
#define SFPTPD_SYNC_MODULE_MSG(x) (SFPTPD_MSG_BASE_SYNC_MODULE + (x))

/** Message to get the status of a sync instance
 * This is a send-wait operation so the sync module should reply with the
 * requested information as soon as it handles the request message.
 * @instance_handle Handle of sync module instance
 * @status Sync module status
 */
#define SFPTPD_SYNC_MODULE_MSG_GET_STATUS SFPTPD_SYNC_MODULE_MSG(1)
struct sfptpd_sync_module_get_status_req {
	struct sfptpd_sync_instance *instance_handle;
};
struct sfptpd_sync_module_get_status_resp {
	struct sfptpd_sync_instance_status status;
};

/** Message to step the clock of the sync instance
 * This is a send-wait operation to ensure that all clocks so the sync module
 * must reply once the clock has been stepped.
 * @instance_handle Handle of sync module instance
 * @offest Amount by which the clock should be stepped
 */
#define SFPTPD_SYNC_MODULE_MSG_STEP_CLOCK SFPTPD_SYNC_MODULE_MSG(2)
struct sfptpd_sync_module_step_clock_req {
	struct sfptpd_sync_instance *instance_handle;
	struct sfptpd_timespec offset;
};

/** Message to log stats. This message applies to all instances of a sync
 * module.
 * This is a send-wait operation to ensure that only one entity is writing to
 * the stats log at any time.
 * @time Time to use for the purposes of logging
 */
#define SFPTPD_SYNC_MODULE_MSG_LOG_STATS SFPTPD_SYNC_MODULE_MSG(3)
struct sfptpd_sync_module_log_stats_req {
	struct sfptpd_log_time time;
};

/** Message to signal to the sync module to save it's state. This message
 * applies to all instances of a sync module.
 * This a sent as an asynchronous message to the sync module without an
 * associated reply.
 */
#define SFPTPD_SYNC_MODULE_MSG_SAVE_STATE SFPTPD_SYNC_MODULE_MSG(4)

/** Message to instruct a selected sync instance to write the clock topology
 * to the supplied stream.
 * This is a send-wait operation to ensure that only one entity is writing
 * to the toplogy file at any time.
 * @instance_handle Handle of sync module instance
 * @stream Stream to write topology to
 */
#define SFPTPD_SYNC_MODULE_MSG_WRITE_TOPOLOGY SFPTPD_SYNC_MODULE_MSG(5)
struct sfptpd_sync_module_write_topology_req {
	struct sfptpd_sync_instance *instance_handle;
	FILE *stream;
};

/** Message to request the sync module to end the current stats period and
 * write the stats to file. This message applies to all instances of a sync
 * module.
 * This is sent as an asynchronous message to the sync module without an
 * associated reply.
 * @time Time to record stats against at the end of this stats period
 */
#define SFPTPD_SYNC_MODULE_MSG_STATS_END_PERIOD SFPTPD_SYNC_MODULE_MSG(6)
struct sfptpd_sync_module_stats_end_period_req {
	struct sfptpd_timespec time;
};

/** Message to configure a test mode.
 * This is sent as an asynchronous message to a sync instance without an
 * associated reply.
 * @instance_handle Handle of sync module instance
 * @id Test mode to configure
 * @params Test mode specific parameters
 */
#define SFPTPD_SYNC_MODULE_MSG_TEST_MODE SFPTPD_SYNC_MODULE_MSG(7)
struct sfptpd_sync_module_test_mode_req {
	struct sfptpd_sync_instance *instance_handle;
	enum sfptpd_test_id id;
	int params[3];
};

/** Message to control a sync instance.
 * This is a send-wait operation to that the changes are in place by the time
 * the response is received
 * @instance_handle Handle of sync module instance
 * @flags Bitmask of functions to be enabled
 * @mask Bitmask of which functions to change
 */
#define SFPTPD_SYNC_MODULE_MSG_CONTROL SFPTPD_SYNC_MODULE_MSG(8)
struct sfptpd_sync_module_control_req {
	struct sfptpd_sync_instance *instance_handle;
	sfptpd_sync_module_ctrl_flags_t flags;
	sfptpd_sync_module_ctrl_flags_t mask;
};

/** Message to update a sync module with information about currently selected
 * gransmaster.
 * This is sent as an asynchronous message to the sync module without an
 * associated reply.
 * @originator Handle of instance that originated the update.
 * @info Structure containing grandmaster info
 */
#define SFPTPD_SYNC_MODULE_MSG_UPDATE_GM_INFO SFPTPD_SYNC_MODULE_MSG(9)
struct sfptpd_sync_module_update_gm_info_req {
	struct sfptpd_sync_instance *originator;
	struct sfptpd_grandmaster_info info;
};

/** Message to update a sync module with information about the current
 * leap second status.
 * This is sent as an asynchronous message to the sync module without an
 * associated reply.
 * @type Current leap second state
 */
#define SFPTPD_SYNC_MODULE_MSG_UPDATE_LEAP_SECOND SFPTPD_SYNC_MODULE_MSG(10)
struct sfptpd_sync_module_update_leap_second_req {
	enum sfptpd_leap_second_type type;
};

/** Message to notify a sync module that network interface state has changed.
 * This is sent as an asynchronous message to the sync module. The recipient
 * holds a reference-counted read lock on the link table which is released
 * with a corresponding message to the sync engine.
 */
#define SFPTPD_SYNC_MODULE_MSG_LINK_TABLE SFPTPD_SYNC_MODULE_MSG(11)
struct sfptpd_sync_module_link_table_req {
	const struct sfptpd_link_table *link_table;
};

/** Union of all sync module messages
 * @hdr Standard message header
 * @u Union of message payloads
 */
typedef struct sfptpd_sync_module_msg {
	sfptpd_msg_hdr_t hdr;
	union {
		struct sfptpd_sync_module_get_status_req get_status_req;
		struct sfptpd_sync_module_get_status_resp get_status_resp;
		struct sfptpd_sync_module_control_req control_req;
		struct sfptpd_sync_module_step_clock_req step_clock_req;
		struct sfptpd_sync_module_log_stats_req log_stats_req;
		struct sfptpd_sync_module_write_topology_req write_topology_req;
		struct sfptpd_sync_module_stats_end_period_req stats_end_period_req;
		struct sfptpd_sync_module_test_mode_req test_mode_req;
		struct sfptpd_sync_module_update_gm_info_req update_gm_info_req;
		struct sfptpd_sync_module_update_leap_second_req update_leap_second_req;
		struct sfptpd_sync_module_link_table_req link_table_req;
	} u;
} sfptpd_sync_module_msg_t;

/* Make sure that the messages are smaller than global pool size */
STATIC_ASSERT(sizeof(sfptpd_sync_module_msg_t) < SFPTPD_SIZE_GLOBAL_MSGS);


#endif /* _SFPTPD_SYNC_MODULE_H */
