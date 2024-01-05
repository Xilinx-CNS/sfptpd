/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2024 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_CRNY_MODULE_H
#define _SFPTPD_CRNY_MODULE_H

#include "sfptpd_config.h"


/****************************************************************************
 * Constants
 ****************************************************************************/

#define SFPTPD_CRNY_DEFAULT_CONTROL_SCRIPT "/usr/libexec/sfptpd/chrony_clockcontrol.py"
#define SFPTPD_CRNY_LEGACY_CONTROL_SCRIPT_PATTERN "/usr/share/doc/sfptpd.*/examples/chrony_clockcontrol.py"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Identifier for the sync module */
#define SFPTPD_CRNY_MODULE_NAME "crny"

/** External NTP (chrony) Sync module configuration structure */
typedef struct sfptpd_crny_module_config {
	/** Common configuration header */
	sfptpd_config_section_t hdr;

	/* Priority of this instance */
	unsigned int priority;

	/* Convergence threshold */
	long double convergence_threshold;

	/** Interval between polls of the NTP daemon, in seconds */
	unsigned int poll_interval;

	/** Clock control enabled */
	bool clock_control;

	/** Path for script to provide chronyd clock control */
	char chronyd_script[PATH_MAX];
} sfptpd_crny_module_config_t;

/** Forward structure declarations */
struct sfptpd_engine;
struct sfptpd_thread;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create and initialise the NTP module configuration options. This will
 * create a global instance of the NTP configuration and set the values to
 * defaults. The function also registers config options applicable to this
 * sync module type.
 * @param config  Pointer to the configuration structure
 * @return 0 on success or an errno otherwise
 */
int sfptpd_crny_module_config_init(struct sfptpd_config *config);

/** Get the NTP configuration. This will return the global configuration from
 * which NTP instances can be accessed.
 * @param config  Pointer to the configuration
 * @return A pointer to the NTP global configuration
 */
struct sfptpd_crny_module_config *sfptpd_crny_module_get_config(struct sfptpd_config *config);

/** Set the default interface to be used by the sync module. This is supported
 * to allow the interface to be specified on the command line which is
 * convenient for users and non-ambiguous for simple configurations.
 * @param config  Pointer to the configuration
 * @param interface_name  Default interface
 */
void sfptpd_crny_module_set_default_interface(struct sfptpd_config *config,
					     const char *interface_name);

/** Create an NTP sync module instance based on the configuration supplied
 * @param config  Pointer to configuration
 * @param engine  Pointer to sync engine
 * @param sync_module Returned pointer to created sync module
 * @param instance_info_buffer To be populated by information on each sync instance
 * @param instance_info_entries Number of entries in instance_info_buffer
 * @param link_table Pointer to initial link table
 * @param link_subscriber To be set to true if the sync module wishes to
 * subscribe to link table changes
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_crny_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries,
			     const struct sfptpd_link_table *link_table,
			     bool *link_subscriber);


#endif /* _SFPTPD_CRNY_MODULE_H */
