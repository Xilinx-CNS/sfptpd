/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_sync_module.c
 * @brief  Synchronization Module base class & factory
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "sfptpd_logging.h"
#include "sfptpd_thread.h"
#include "sfptpd_message.h"
#include "sfptpd_config.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_freerun_module.h"
#include "sfptpd_ptp_module.h"
#include "sfptpd_pps_module.h"
#include "sfptpd_ntp_module.h"
#include "sfptpd_crny_module.h"


/****************************************************************************
 * Types
 ****************************************************************************/

typedef int (*sync_module_create_fn)(struct sfptpd_config *,
				     struct sfptpd_engine *,
				     struct sfptpd_thread **,
				     struct sfptpd_sync_instance_info *, int,
				     const struct sfptpd_link_table *, bool *
);

struct sync_module_defn {
	const char *name;
	sync_module_create_fn create;
};

struct sync_module_bitmask_to_text_map {
	unsigned int bitmask;
	const char *text;
};


/****************************************************************************
 * Constants
 ****************************************************************************/

/* Sync module definitions indexed by the configuration category identifier */
static const struct sync_module_defn sync_module_defns[] = {
	[SFPTPD_CONFIG_CATEGORY_FREERUN]
		= {SFPTPD_FREERUN_MODULE_NAME, sfptpd_freerun_module_create},
	[SFPTPD_CONFIG_CATEGORY_PTP]
		= {SFPTPD_PTP_MODULE_NAME, sfptpd_ptp_module_create},
	[SFPTPD_CONFIG_CATEGORY_PPS]
		= {SFPTPD_PPS_MODULE_NAME, sfptpd_pps_module_create},
	[SFPTPD_CONFIG_CATEGORY_NTP]
		= {SFPTPD_NTP_MODULE_NAME, sfptpd_ntp_module_create},
	[SFPTPD_CONFIG_CATEGORY_CRNY]
		= {SFPTPD_CRNY_MODULE_NAME, sfptpd_crny_module_create},
};


const struct sync_module_bitmask_to_text_map alarm_texts[] = {
	{SYNC_MODULE_ALARM_PPS_NO_SIGNAL,             "pps-no-signal"},
	{SYNC_MODULE_ALARM_PPS_SEQ_NUM_ERROR,         "pps-seq-num-error"},
	{SYNC_MODULE_ALARM_NO_TIME_OF_DAY,            "no-time-of-day"},
	{SYNC_MODULE_ALARM_PPS_BAD_SIGNAL,            "pps-bad-signal"},
	{SYNC_MODULE_ALARM_NO_SYNC_PKTS,              "no-sync-pkts"},
	{SYNC_MODULE_ALARM_NO_FOLLOW_UPS,             "no-follow-ups"},
	{SYNC_MODULE_ALARM_NO_DELAY_RESPS,            "no-delay-resps"},
	{SYNC_MODULE_ALARM_NO_PDELAY_RESPS,           "no-pdelay-resps"},
	{SYNC_MODULE_ALARM_NO_PDELAY_RESP_FOLLOW_UPS, "no-pdelay-resp-follow-ups"},
	{SYNC_MODULE_ALARM_NO_TX_TIMESTAMPS,          "no-tx-timestamps"},
	{SYNC_MODULE_ALARM_NO_RX_TIMESTAMPS,          "no-rx-timestamps"},
	{SYNC_MODULE_ALARM_NO_INTERFACE,              "no-interface"},
	{SYNC_MODULE_ALARM_CLOCK_CTRL_FAILURE,        "clock-ctrl-failure"},
	{SYNC_MODULE_ALARM_CLOCK_NEAR_EPOCH,          "clock-near-epoch"},
	{SYNC_MODULE_ALARM_CAPS_MISMATCH,             "caps-mismatch "},
	{SYNC_MODULE_ALARM_CLUSTERING_THRESHOLD_EXCEEDED, "clustering-guard"},
	{SYNC_MODULE_ALARM_SUSTAINED_SYNC_FAILURE   , "sustained-sync-failure"},
};

STATIC_ASSERT(1 << (sizeof(alarm_texts) / sizeof(*alarm_texts)) == SYNC_MODULE_ALARM_MAX);


const char *sync_module_state_text[] = {
    "listening",        /* SYNC_MODULE_STATE_LISTENING */
    "slave",            /* SYNC_MODULE_STATE_SLAVE */
    "master",           /* SYNC_MODULE_STATE_MASTER */
    "passive",          /* SYNC_MODULE_STATE_PASSIVE */
    "disabled",         /* SYNC_MODULE_STATE_DISABLED */
    "faulty",           /* SYNC_MODULE_STATE_FAULTY */
    "selection",        /* SYNC_MODULE_STATE_SELECTION */
};

STATIC_ASSERT(sizeof(sync_module_state_text) / sizeof(*sync_module_state_text) == SYNC_MODULE_STATE_MAX);


/****************************************************************************
 * Private functions
 ****************************************************************************/

static void sync_module_bitmask_to_text(unsigned int bitmask, char *buffer,
					unsigned int buffer_size,
					const struct sync_module_bitmask_to_text_map map[],
					unsigned int map_size)
{
	const char *ellipsis = "...";
	const unsigned int ellipsis_len = strlen(ellipsis) + 1;
	unsigned int i, len = 0;
	bool truncated = false;

	assert(buffer != NULL);
	assert(map != NULL);
	assert(buffer_size >= ellipsis_len);

	/* If there are no flags, write the string "none" and return */
	if (bitmask == 0) {
		len = snprintf(buffer, buffer_size, "none");
		if (len >= buffer_size) {
			len = buffer_size;
			truncated = true;
		}
		goto exit;
	}

	for (i = 0; i < map_size; i++) {
		if (bitmask & map[i].bitmask) {
			len += snprintf(buffer + len, buffer_size - len, "%s ",
					map[i].text);
			if (len >= buffer_size) {
				len = buffer_size;
				truncated = true;
			}
		}
	}

exit:
	/* If we have filled the buffer, write an ellipsis at the end. Note
	 * that snprintf 0 terminates even in the case where there isn't
	 * sufficient space for the print so we have to overwrite that null if
	 * the output string has been truncated. */
	if (truncated)
		snprintf(buffer + buffer_size - ellipsis_len, ellipsis_len,
		         "%s", ellipsis);
}


/****************************************************************************
 * Help and configuration
 ****************************************************************************/

int sfptpd_sync_module_config_init(struct sfptpd_config *config)
{
	int rc;
	assert(config != NULL);

	rc = sfptpd_freerun_module_config_init(config);
	if (rc != 0)
		return rc;
	rc = sfptpd_ptp_module_config_init(config);
	if (rc != 0)
		return rc;
	rc = sfptpd_pps_module_config_init(config);
	if (rc != 0)
		return rc;
	rc = sfptpd_ntp_module_config_init(config);
	if (rc != 0)
		return rc;
	rc = sfptpd_crny_module_config_init(config);
	return rc;
}


void sfptpd_sync_module_set_default_interface(struct sfptpd_config *config,
					      const char *interface_name)
{
	assert(config != NULL);
	assert(interface_name != NULL);

	sfptpd_freerun_module_set_default_interface(config, interface_name);
	sfptpd_ptp_module_set_default_interface(config, interface_name);
	sfptpd_pps_module_set_default_interface(config, interface_name);
	sfptpd_ntp_module_set_default_interface(config, interface_name);

	TRACE_L3("default interface set to %s\n", optarg);
}


void sfptpd_sync_module_ctrl_flags_text(sfptpd_sync_module_ctrl_flags_t flags,
					char *buffer, unsigned int buffer_size)
{
	const struct sync_module_bitmask_to_text_map map[] = {
		{SYNC_MODULE_SELECTED,               "selected"},
		{SYNC_MODULE_TIMESTAMP_PROCESSING,   "timestamp-processing"},
		{SYNC_MODULE_CLOCK_CTRL,             "clock-ctrl"},
		{SYNC_MODULE_LEAP_SECOND_GUARD,      "leap-second-guard"},
		{SYNC_MODULE_CLUSTERING_DETERMINANT, "clustering-determinant"},
	};

	sync_module_bitmask_to_text(flags, buffer, buffer_size, map,
				    sizeof(map)/sizeof(map[0]));
}


void sfptpd_sync_module_alarms_stream(FILE *stream,
	sfptpd_sync_module_alarms_t alarms, const char *separator)
{
	int i;
	const char *sep = "";

	for (i = 0; i < sizeof(alarm_texts)/sizeof(alarm_texts[0]); i++) {
		if (alarms & alarm_texts[i].bitmask) {
			fprintf(stream, "%s\"%s\"", sep, alarm_texts[i].text);
			sep = separator;
		}
	}
}


void sfptpd_sync_module_alarms_text(sfptpd_sync_module_alarms_t alarms,
				    char *buffer, unsigned int buffer_size)
{
	sync_module_bitmask_to_text(alarms, buffer, buffer_size, alarm_texts,
				    sizeof(alarm_texts)/sizeof(alarm_texts[0]));
}


const char *sfptpd_sync_module_name(enum sfptpd_config_category type)
{
	assert(type < SFPTPD_CONFIG_CATEGORY_MAX);
	return sync_module_defns[type].name;
}


bool sfptpd_sync_module_gm_info_equal(struct sfptpd_grandmaster_info *gm1,
				      struct sfptpd_grandmaster_info *gm2)
{
	assert(gm1 != NULL);
	assert(gm2 != NULL);

	return sfptpd_clock_ids_equal(&gm1->clock_id, &gm2->clock_id) &&
	       (gm1->clock_class == gm2->clock_class) &&
	       (gm1->time_source == gm2->time_source) &&
	       (gm1->accuracy == gm2->accuracy) &&
	       (gm1->allan_variance == gm2->allan_variance ||
		(isnan(gm1->allan_variance) && isnan(gm2->allan_variance))) &&
	       (gm1->steps_removed == gm2->steps_removed);
}


/****************************************************************************
 * Sync module creation and management
 ****************************************************************************/

int sfptpd_sync_module_create(enum sfptpd_config_category type,
			      struct sfptpd_config *config,
			      struct sfptpd_engine *engine,
			      struct sfptpd_thread **sync_module,
			      struct sfptpd_sync_instance_info *instance_info_buffer,
			      int instance_info_entries,
			      const struct sfptpd_link_table *link_table,
			      bool *link_table_subscriber)
{
	int rc;

	assert(config != NULL);
	assert(engine != NULL);
	assert(sync_module != NULL);

	*sync_module = NULL;

	if (type < 0 || type >= SFPTPD_CONFIG_CATEGORY_MAX) {
		ERROR("unrecognised sync module type %d\n", type);
		rc = EINVAL;
	} else {
		if (sync_module_defns[type].create == NULL) {
			/* This is not a sync module - tell the caller politely */
			rc = ENOENT;
		} else {
			rc = sync_module_defns[type].create(config,
							    engine,
							    sync_module,
							    instance_info_buffer,
							    instance_info_entries,
							    link_table,
							    link_table_subscriber);
		}
	}

        return rc;
}


void sfptpd_sync_module_destroy(struct sfptpd_thread *sync_module)
{
	(void)sfptpd_thread_destroy(sync_module);
}


int sfptpd_sync_module_get_status(struct sfptpd_thread *sync_module,
				  struct sfptpd_sync_instance *sync_instance,
				  struct sfptpd_sync_instance_status *status)
{
	sfptpd_sync_module_msg_t msg = { { 0 } }; /* Double braces to get around GCC bug #53119 */
	int rc;

	assert(status != NULL);

	SFPTPD_MSG_INIT(msg);
	msg.u.get_status_req.instance_handle = sync_instance;
	rc = SFPTPD_MSG_SEND_WAIT(&msg, sync_module,
				  SFPTPD_SYNC_MODULE_MSG_GET_STATUS);
	if (rc == 0)
		*status = msg.u.get_status_resp.status;

	return rc;
}


int sfptpd_sync_module_control(struct sfptpd_thread *sync_module,
			       struct sfptpd_sync_instance *sync_instance,
			       sfptpd_sync_module_ctrl_flags_t flags,
			       sfptpd_sync_module_ctrl_flags_t mask)
{
	sfptpd_sync_module_msg_t msg;
	int rc;

	assert(sync_instance != NULL);

	SFPTPD_MSG_INIT(msg);
	msg.u.control_req.instance_handle = sync_instance;
	msg.u.control_req.flags = flags;
	msg.u.control_req.mask = mask;
	rc = SFPTPD_MSG_SEND_WAIT(&msg, sync_module,
				  SFPTPD_SYNC_MODULE_MSG_CONTROL);

	return rc;
}


void sfptpd_sync_module_update_gm_info(struct sfptpd_thread *sync_module,
				       struct sfptpd_sync_instance *originator,
				       struct sfptpd_grandmaster_info *info)
{
	sfptpd_sync_module_msg_t *msg;

	assert(info != NULL);

	msg = (sfptpd_sync_module_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL,
							   false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.update_gm_info_req.originator = originator;
	msg->u.update_gm_info_req.info = *info;
	(void)SFPTPD_MSG_SEND(msg, sync_module,
			      SFPTPD_SYNC_MODULE_MSG_UPDATE_GM_INFO, false);
}


int sfptpd_sync_module_step_clock(struct sfptpd_thread *sync_module,
				  struct sfptpd_sync_instance *sync_instance,
				  struct timespec *offset)
{
	sfptpd_sync_module_msg_t msg;
	int rc;

	/* sync_instance might be NULL if this is to notify a passive NTP sync module */
	assert(offset != NULL);

	SFPTPD_MSG_INIT(msg);
	msg.u.step_clock_req.instance_handle = sync_instance;
	msg.u.step_clock_req.offset = *offset;
	rc = SFPTPD_MSG_SEND_WAIT(&msg, sync_module,
				  SFPTPD_SYNC_MODULE_MSG_STEP_CLOCK);

	return rc;
}


void sfptpd_sync_module_log_stats(struct sfptpd_thread *sync_module,
				  struct sfptpd_log_time *time)
{
	sfptpd_sync_module_msg_t *msg;

	assert(time != NULL);

	msg = (sfptpd_sync_module_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL,
							   false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.log_stats_req.time = *time;
	(void)SFPTPD_MSG_SEND(msg, sync_module,
				   SFPTPD_SYNC_MODULE_MSG_LOG_STATS, false);
}


void sfptpd_sync_module_save_state(struct sfptpd_thread *sync_module)
{
	sfptpd_sync_module_msg_t *msg;

	msg = (sfptpd_sync_module_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL,
							   false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	(void)SFPTPD_MSG_SEND(msg, sync_module,
			      SFPTPD_SYNC_MODULE_MSG_SAVE_STATE, false);
}


void sfptpd_sync_module_update_leap_second(struct sfptpd_thread *sync_module,
					   enum sfptpd_leap_second_type leap_second_type)
{
	sfptpd_sync_module_msg_t *msg;

	assert(leap_second_type < SFPTPD_LEAP_SECOND_MAX);

	msg = (sfptpd_sync_module_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL,
							   false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.update_leap_second_req.type = leap_second_type;
	(void)SFPTPD_MSG_SEND(msg, sync_module,
			      SFPTPD_SYNC_MODULE_MSG_UPDATE_LEAP_SECOND, false);
}


void sfptpd_sync_module_write_topology(struct sfptpd_thread *sync_module,
				       struct sfptpd_sync_instance *sync_instance,
				       FILE *stream)
{
	sfptpd_sync_module_msg_t msg;

	assert(sync_instance != NULL);
	assert(stream != NULL);

	SFPTPD_MSG_INIT(msg);
	msg.u.write_topology_req.instance_handle = sync_instance;
	msg.u.write_topology_req.stream = stream;
	(void)SFPTPD_MSG_SEND_WAIT(&msg, sync_module,
				   SFPTPD_SYNC_MODULE_MSG_WRITE_TOPOLOGY);
}


void sfptpd_sync_module_stats_end_period(struct sfptpd_thread *sync_module,
					 struct timespec *time)
{
	sfptpd_sync_module_msg_t *msg;

	assert(time != NULL);

	msg = (sfptpd_sync_module_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL,
							   false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.stats_end_period_req.time = *time;
	(void)SFPTPD_MSG_SEND(msg, sync_module,
			      SFPTPD_SYNC_MODULE_MSG_STATS_END_PERIOD, false);
}


void sfptpd_sync_module_test_mode(struct sfptpd_thread *sync_module,
				  struct sfptpd_sync_instance *sync_instance,
				  enum sfptpd_test_id id,
				  int param0, int param1,
				  int param2)
{
	sfptpd_sync_module_msg_t *msg;

	assert(sync_instance != NULL);

	msg = (sfptpd_sync_module_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL,
							   false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.test_mode_req.instance_handle = sync_instance;
	msg->u.test_mode_req.id = id;
	msg->u.test_mode_req.params[0] = param0;
	msg->u.test_mode_req.params[1] = param1;
	msg->u.test_mode_req.params[2] = param2;
	(void)SFPTPD_MSG_SEND(msg, sync_module,
			      SFPTPD_SYNC_MODULE_MSG_TEST_MODE, false);
}


void sfptpd_sync_module_link_table(struct sfptpd_thread *sync_module,
				   const struct sfptpd_link_table *link_table)
{
	sfptpd_sync_module_msg_t *msg;

	msg = (sfptpd_sync_module_msg_t *)sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL,
							   false);
	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	msg->u.link_table_req.link_table = link_table;

	(void)SFPTPD_MSG_SEND(msg, sync_module,
			      SFPTPD_SYNC_MODULE_MSG_LINK_TABLE, false);
}


/* fin */
