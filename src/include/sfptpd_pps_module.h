/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_PPS_MODULE_H
#define _SFPTPD_PPS_MODULE_H

#include "sfptpd_config.h"
#include "sfptpd_time.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/** Identifier for the PPS sync module */
#define SFPTPD_PPS_MODULE_NAME "pps"

/** Default clock class, time source and accuracy. */
#define SFPTPD_PPS_DEFAULT_CLOCK_CLASS   (SFPTPD_CLOCK_CLASS_LOCKED)
#define SFPTPD_PPS_DEFAULT_TIME_SOURCE   (SFPTPD_TIME_SOURCE_GPS)
#define SFPTPD_PPS_DEFAULT_ACCURACY      INFINITY
#define SFPTPD_PPS_DEFAULT_STEPS_REMOVED (1)
#define SFPTPD_PPS_DEFAULT_TIME_TRACEABLE (true)
#define SFPTPD_PPS_DEFAULT_FREQ_TRACEABLE (true)

#define SFPTPD_PPS_DEFAULT_OUTLIER_FILTER_ENABLED  (true)
#define SFPTPD_PPS_DEFAULT_OUTLIER_FILTER_SIZE     30
#define SFPTPD_PPS_DEFAULT_OUTLIER_FILTER_ADAPTION 1.0

#define SFPTPD_PPS_DEFAULT_PID_FILTER_KP  0.05
#define SFPTPD_PPS_DEFAULT_PID_FILTER_KI  0.001

#define SFPTPD_PPS_DEFAULT_FIR_FILTER_SIZE 4


/** PPS Sync module configuration structure */
typedef struct sfptpd_pps_module_config {
	/** Common configuration header */
	sfptpd_config_section_t hdr;

	/* Textual name of interface that PPS should use */
	char interface_name[IF_NAMESIZE];

	/* Priority of this instance */
	unsigned int priority;

	/* Convergence threshold */
	long double convergence_threshold;

	/* The name of the sync instance to use for time of day */
	char tod_name[SFPTPD_CONFIG_SECTION_NAME_MAX];

	/* Clock class, time source and accuracy */
	enum sfptpd_clock_class master_clock_class;
	enum sfptpd_time_source master_time_source;
	long double master_accuracy;
	bool master_time_traceable;
	bool master_freq_traceable;

	/* Number of steps between primary reference time source and slave */
	unsigned int steps_removed;

	/** PPS propagation delay in nanoseconds */
	long double propagation_delay;

	/** PID filter */
	struct {
		/* Proportional coefficient */
		long double kp;

		/* Integral coefficient */
		long double ki;
	} pid_filter;

	/** Outlier filtering */
	struct {
		/* Is outlier filter enabled? */
		bool enabled;

		/* Size of outlier filter in samples */
		unsigned int size;

		/* Weighting given to outliers - controls how adaptive the
		 * filter is */
		long double adaption;
	} outlier_filter;

	/** FIR filter size */
	unsigned int fir_filter_size;
} sfptpd_pps_module_config_t;


/** Forward structure declarations */
struct sfptpd_engine;
struct sfptpd_thread;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create and initialise the PPS module configuration options. This will
 * create a global instance of the PPS configuration and set the values to
 * defaults. The function also registers config options applicable to this
 * sync module type.
 * @param config  Pointer to the configuration structure
 * @return 0 on success or an errno otherwise
 */
int sfptpd_pps_module_config_init(struct sfptpd_config *config);

/** Get the PPS configuration. This will return the global configuration from
 * which PPS instances can be accessed.
 * @param config  Pointer to the configuration
 * @return A pointer to the PPS global configuration
 */
struct sfptpd_pps_module_config *sfptpd_pps_module_get_config(struct sfptpd_config *config);

/** Set the default interface to be used by the sync module. This is supported
 * to allow the interface to be specified on the command line which is
 * convenient for users and non-ambiguous for simple configurations.
 * @param config  Pointer to the configuration
 * @param interface_name  Default interface
 */
void sfptpd_pps_module_set_default_interface(struct sfptpd_config *config,
					     const char *interface_name);

/** Retrieve the configured PPS propagation delay for a specified clock.
 * Note that this will return the propagation delay for any PPS instance
 * using the same clock as this interface on the basis that only one PPS
 * instance is allowed for each PPS input. If not PPS delay has been
 * specified for the clock, the global PPS propagation delay will be
 * returned.
 * @param config  Pointer to the configuration structure
 * @param interface  Interface for which the propagation delay is required
 * @return PPS propagation delay in ns
 */
sfptpd_time_t sfptpd_pps_module_config_get_propagation_delay(struct sfptpd_config *config,
							     struct sfptpd_clock *clock);

/** Create a PPS sync module instance based on the configuration supplied
 * @param config  Pointer to configuration
 * @param engine  Pointer to sync engine
 * @param sync_module Returned pointer to created sync module
 * @param instance_info_buffer To be populated by information on each sync instance
 * @param instance_info_entries Number of entries in instance_info_buffer
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_pps_module_create(struct sfptpd_config *config,
			     struct sfptpd_engine *engine,
			     struct sfptpd_thread **sync_module,
			     struct sfptpd_sync_instance_info *instances_info_buffer,
			     int instances_info_entries);


#endif /* _SFPTPD_PPS_MODULE_H */
