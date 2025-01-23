/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024-2025 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_METRICS_H
#define _SFPTPD_METRICS_H

#include <stdio.h>
#include <syslog.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>

#include "sfptpd_misc.h"
#include "sfptpd_thread.h"

/****************************************************************************
 * Structures, Types, Defines
 ****************************************************************************/

enum sfptpd_metrics_option {
	SFPTPD_METRICS_OPTION_ALARM_STATESET,
	SFPTPD_METRICS_NUM_OPTIONS
};

/* Storage for metrics flags, auto-sized in C23 */
typedef enum {
	SFPTPD_METRICS_FLAG_MAX = 1 << (SFPTPD_METRICS_NUM_OPTIONS - 1)
} sfptpd_metrics_flags_t;

extern const char *sfptpd_metrics_option_names[SFPTPD_METRICS_NUM_OPTIONS];


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/* Initialise metrics state. */
int sfptpd_metrics_init(void);

/* Finalise metrics state */
void sfptpd_metrics_destroy(void);

/* Service metrics fds if ready */
void sfptpd_metrics_service_fds(unsigned int num_fds,
				struct sfptpd_thread_readyfd fd[]);

/* Create the metrics listener socket */
int sfptpd_metrics_listener_open(struct sfptpd_config *config);

/* Close the metrics listener socket */
void sfptpd_metrics_listener_close(void);

/* Handle metrics push from sfptpd.
 * Must be used from the same thread that services fds. */
extern void sfptpd_metrics_push_rt_stats(struct sfptpd_sync_instance_rt_stats_entry *entry);

#endif /* _SFPTPD_METRICS_H */
