/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_FREERUN_MODULE_H
#define _SFPTPD_FREERUN_MODULE_H

#include "sfptpd_config.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Identifier for the freerun sync module */
#define SFPTPD_FREERUN_MODULE_NAME "freerun"

/** Freerun sync module configuration structure */
typedef struct sfptpd_freerun_module_config {
	/** Common configuration header */
	sfptpd_config_section_t hdr;

	/* Textual name of interface that PTP should use */
	char interface_name[IF_NAMESIZE];

	/* Priority of this instance */
	unsigned int priority;

	/* Clock class and accuracy */
	enum sfptpd_clock_class clock_class;
	long double clock_accuracy;
	bool clock_time_traceable;
	bool clock_freq_traceable;

} sfptpd_freerun_module_config_t;

/** Forward structure declarations */
struct sfptpd_engine;
struct sfptpd_thread;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create and initialise the freerun module configuration options. This will
 * create a global instance of the freerun configuration and set the values to
 * defaults. The function also registers config options applicable to this
 * sync module type.
 * @param config  Pointer to the configuration structure
 * @return 0 on success or an errno otherwise
 */
int sfptpd_freerun_module_config_init(struct sfptpd_config *config);

/** Get the freerun configuration. This will return the global configuration from
 * which freerun instances can be accessed.
 * @param config  Pointer to the configuration
 * @return A pointer to the freerun global configuration
 */
struct sfptpd_freerun_module_config *sfptpd_freerun_module_get_config(struct sfptpd_config *config);

/** Set the default interface to be used by the sync module. This is supported
 * to allow the interface to be specified on the command line which is
 * convenient for users and non-ambiguous for simple configurations.
 * @param config  Pointer to the configuration
 * @param interface_name  Default interface
 */
void sfptpd_freerun_module_set_default_interface(struct sfptpd_config *config,
						 const char *interface_name);

/** Create a free-run sync module instance based on the configuration supplied
 * @param config  Pointer to configuration
 * @param engine  Pointer to sync engine
 * @param sync_module Returned pointer to created sync module
 * @param instance_info_buffer To be populated by information on each sync instance
 * @param instance_info_entries Number of entries in instance_info_buffer
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_freerun_module_create(struct sfptpd_config *config,
				 struct sfptpd_engine *engine,
				 struct sfptpd_thread **sync_module,
				 struct sfptpd_sync_instance_info *instances_info_buffer,
				 int instances_info_entries);


#endif /* _SFPTPD_FREERUN_MODULE_H */
