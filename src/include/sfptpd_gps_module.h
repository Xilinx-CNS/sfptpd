/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advance Micro Devices, Inc. */

#ifndef _SFPTPD_GPS_MODULE_H
#define _SFPTPD_GPS_MODULE_H

#include "sfptpd_config.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Identifier for the sync module */
#define SFPTPD_GPS_MODULE_NAME "gps"

/** GPS Sync module configuration structure */
typedef struct sfptpd_gps_module_config {
	/** Common configuration header */
	sfptpd_config_section_t hdr;

	/* Priority of this instance */
	unsigned int priority;

	/* Convergence threshold */
	long double convergence_threshold;

	/** Whether to connect to gpsd */
	bool gpsd;

	/** Host for connecting to gpsd */
	char gpsd_host[NI_MAXHOST];

	/** Port for connecting to gpsd */
	char gpsd_serv[NI_MAXSERV];
} sfptpd_gps_module_config_t;

/** Forward structure declarations */
struct sfptpd_engine;
struct sfptpd_thread;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create and initialise the GPS module configuration options. This will
 * create a global instance of the GPS configuration and set the values to
 * defaults. The function also registers config options applicable to this
 * sync module type.
 * @param config  Pointer to the configuration structure
 * @return 0 on success or an errno otherwise
 */
int sfptpd_gps_module_config_init(struct sfptpd_config *config);

/** Get the GPD configuration. This will return the global configuration from
 * which GPS instances can be accessed.
 * @param config  Pointer to the configuration
 * @return A pointer to the GPS global configuration
 */
struct sfptpd_gps_module_config *sfptpd_gps_module_get_config(struct sfptpd_config *config);

/** Set the default interface to be used by the sync module. This is supported
 * to allow the interface to be specified on the command line which is
 * convenient for users and non-ambiguous for simple configurations.
 * @param config  Pointer to the configuration
 * @param interface_name  Default interface
 */
void sfptpd_gps_module_set_default_interface(struct sfptpd_config *config,
					     const char *interface_name);

/** Create a GPS sync module instance based on the configuration supplied
 * @param config  Pointer to configuration
 * @param engine  Pointer to sync engine
 * @param sync_module Returned pointer to created sync module
 * @param instance_info_buffer To be populated by information on each sync instance
 * @param instance_info_entries Number of entries in instance_info_buffer
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_gps_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries,
			     const struct sfptpd_link_table *link_table,
			     bool *link_table_subscriber);


#endif /* _SFPTPD_GPS_MODULE_H */
