/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_CONFIG_H
#define _SFPTPD_CONFIG_H

#include <stdio.h>
#include <net/if.h>
#include <limits.h>

#include <sfptpd_interface.h>
#include <sfptpd_logging.h>


/** Macro to log a configuration error - adds the config prefix with the 
 * name of the cofniguration section instance. */
#define CFG_ERROR(section, fmt, ...)     \
	ERROR("config [%s]: " fmt, section->name, ##__VA_ARGS__)


/****************************************************************************
 * Description
 ****************************************************************************/

/**
 * Description
 * ===========
 * 
 * The configuration parsing support in the daemon allows complex
 * configurations to be defined where there are both multiple sync modules and
 * multiple instances of each sync module. The configuration is split into
 * Configuration Sections where each section has various characteristics:
 *   - A unique name
 *   - A category
 *   - A scope indicating if this is global configuration for a category or
 *     an instance within the category.
 *   - Whether instances of the category can be created.
 * The configuration data is organised as an array of linked-lists where each
 * array entry corresponds to configuration for a specific category. The first
 * item in each list will contain global configuration for the category
 * followed by a series of configurations for each instance.
 * 
 * config.categories
 * category:             [GENERAL]   [PTP]       [NTP]       [PPS]
 * name:                 "general"   "ptp"       "ntp"       "pps"
 * scope:                global      global      global      global
 * instances:            instances   instances   instances   instances
 *                         |           |           |           |
 *                         |           v           v           V
 *                         -          PTP         NTP         PPS
 *                                    "ptp1"      "ntp"       "pps-ethx"
 *                                    instance    instance    instance
 *                                    next        next        next
 *                                      |           |           |
 *                                      |           -           |
 *                                      v                       v
 *                                     PTP                     PPS
 *                                     "ptp2"                  "pps-ethy"
 *                                     instance                instance
 *                                     next                    next
 *                                       |                       |
 *                                       -                       -
 * 
 * When daemon starts, the configuration is initialised and the global
 * instance of each configuration category is created. Later when the
 * configuration is parsed, new instances in each category are created when
 * the "sync_module module name" option is used if permitted. When instances
 * of a configuration section are created a copy is taken of the global
 * settings for the category.
 * The configuration specification used ini file style sections. When a change
 * of section is identified in the configuration file, the configuration is
 * searched for a section with the corresponding name. When within a section
 * configuration options are parsed applied to the current section.
 *
 * In the current implementation, when an item is parsed in the global
 * section of a category, the option is also set in all the instances within
 * this category. Ideally, this would only be done if the same option had not
 * already been explicitly set in the instance. Some thought should be given
 * on how this could be achieved.
 * 
 * 
 * Selection Algorithm
 * ===================
 * To support multiple instance configurations, we need to define the process
 * by which we select a master clock (and associated instance) that the daemon
 * will synchronize with. From the set of instances, the daemon will construct
 * a list of Candidate master clocks. In order to be considered as a Candidate,
 * the sync module instance needs to be in the Slave state with no alarms
 * triggered e.g. for PTP this means that the sync module instance is receiving
 * packets from a PTP master and is able to calculate the offset and peer
 * delays.
 * 
 * From the set of candidates, the proposal is that a modified version of the
 * PTP best master clock algorithm will be used to select between Candidates.
 * The proposed modification is that for each clock, in addition to the reported
 * accuracy, the accuracy of the sync module will also be taken into account.
 * The values of the Clock Accuracy and Mechanism Accuracy will be added
 * together before comparison.
 * This provides protection against the case where we have two master with
 * advertising similar accuracies, one of which is available via PTP over an
 * interface supporting PTP hardware timestamping and the other of which is
 * available over a non-PTP interface.
 * 
 * The following data will be compared when selecting the best master clock from
 * the set of Candidates. In order of descending priority...
 *   1) Instance Priority - User configured priority. Note that this is not
 *      related to the PTP priority1/2 fields.
 *   2) Clock Class - Indicates Typically only the values 6, 7 and 248 are used
 *   3a) Clock Accuracy - Estimate of error between clock and primary reference
 *       source
 *   3b) Sync-module Accuracy - Estimate of error implied by the synchronization
 *       mechanism that would be used to sync to this remote clock e.g. NTP with
 *       software timestamping or PTP with hardware timestamping
 *   4) Allan Variance - Estimate of stability of clock
 *   5) Identity of clock if all other comparisons fail
 * For the NTP and PTP sync modules, the user would only specify the priority- 
 * the clock class, clock accuracy and allan-variance would come from the 
 * remote clock. For Freerun and PPS, the user would be able to override default
 * values for the clock class, clock accuracy and allan-variance in addition to
 * specifying a priority.
 *
 *
 * NTP Sync Module
 * ===============
 * The NTP sync module has slightly different behaviour compared to other
 * sync modules. Firstly there can only be one instance of it. Secondly it is
 * necessary to interact (or attempt to interact) with the NTP daemon even if
 * no instance has been instantiate. The module has the following behaviour:
 *   If an NTP sync module is instantiated:
 *     * It can be a candidate and can be selected and discipline the system
 *       clock.
 *     * A shared key must be provided to allow sfptpd to enable/disable
 *       clock discipline in the NTP daemon
 *   If not instantiated:
 *     * NTPd must not be disciplining the system clock
 *     * NTPd can be used to find the time of day (required by PPS)
 * The result of this is that the ntp_mode option is no longer needed.
 *
 * 
 * Use Cases
 * =========
 * 
 * The enhanced configuration is significantly more complex than the previous
 * solution, but greatly inproves flexibility and provides a way to support
 * more the complex configurations being requested by customers.
 * 
 * Simple Freerun
 * --------------
 * In this mode a NIC clock is selected as the Local Clock Reference (LCR). The
 * other clocks in the system are then sync'ed to it.
 * 
 * Example Configuration:
 *     [general]
 *     sync_module freerun fr1
 *     [fr1]
 *     interface eth2
 * 
 * In this example, an instance of the Freerun sync module is created called
 * 'fr1' and eth2 is selected as the Local Clock Reference. No other sync
 * modules will be created but note that the NTP daemon will be monitored to
 * ensure it is not disciplining the system clock.
 *
 * Simple PTP Slave
 * ----------------
 * In this mode an interface is selected to operate as a PTP slave. If the
 * adapter has a hardware clock then hardware timestamping will be used and the
 * adapter clock will be the Local Clock Reference. Otherwise software
 * timestamping will be used and the system clock will be the LCR.
 *
 * Example Configuration:
 *     [general]
 *     sync_module ptp my_ptp
 *     [my_ptp]
 *     interface eth4
 *     ptp_mode slave
 *
 * In this example, an instance of the PTP sync module is created called
 * 'my_ptp' and eth4 is selected to operate the PTP protocol. No other sync
 * modules will be created but note that the NTP daemon will be monitored to
 * ensure it is not disciplining the system clock.
 *
 * Simple PPS Slave
 * ----------------
 * In this mode an interface is selected to operate as a PPS slave. The adapter
 * must have a hardware clock and support PPS for this to work. The adapter
 * clock will be the LCR and the other clock in the system will be sync'ed to
 * it.
 *
 * Example Configuration:
 *     [general]
 *     sync_module pps pps1
 *     [pps1]
 *     interface eth1
 *     pps_delay 30
 *
 * In this example, an instance of the PPS sync module is created called 'pps1'
 * and eth1 is specified as the adapter receiving the PPS signal. The NTP sync
 * module retrieves the time-of-day from the NTPd daemon and supplies it to the
 * PPS sync module.
 *
 * Simple NTP Slave
 * ----------------
 * In this mode the system clock will be the LCR and the NTP daemon will be
 * used to discipline it. The other clocks in the system are then sync'ed to
 * it.
 * 
 * Example Configuration:
 *     [general]
 *     sync_module ntp ntp1
 *     [ntp1]
 *     ntp_key 8 "my shared NTP daemon key"
 * 
 * In this example, an instance of the NTP sync module is created called 'ntp1'
 * and the NTP daemon will be used to disciplin the system clock. No other sync
 * modules will be created. The sfptpd daemon will monitor NTPd to check that
 * it is disciplining the system clock.
 * 
 * PTP Master with Freerun
 * -----------------------
 * In this mode an adapter is freerunning with a NIC clock selected as the LCR.
 * An instance of PTP is running in PTP master mode.
 *
 * Example Configuration:
 *     [general]
 *     sync_module freerun fr1
 *     sync_module ptp ptp1
 *     [fr1]
 *     interface eth2
 *     [ptp1]
 *     interface eth4
 *     ptp_mode master
 *     ptp_domain 0
 * 
 * Note- Previously two different PTP modes were supported (master and
 * master-ntp). However, this is now achieved by specifying another sync module
 * to provide a time source.
 * In this example an instance of the Freerun sync module is created called
 * 'fr1'. This is selecting the adapter clock associated with interface eth2 as
 * the LCR. PTP is operating as a master on interface eth4 and providing time
 * to downstream PTP slaves.
 * Note that it is assumed in this case that the PTP master instance would
 * advertise the clock characteristics of the freerun clock which is providing
 * the reference time unless the clock options were overridden by the user
 * in the configuration file.
 * One important factor to consider in this scenario is that if a better PTP
 * Master appears on the network, the local PTP master will either switch to
 * a slave or become passive. If and when this happens, the intention is that
 * the clock being used for PTP would become the LCR and the freerun module
 * would no longer be in control. In order to make this process work it will
 * be necessary to move the clock selection process (BMC) out of the ptpd2
 * code into sfptpd and to be able to represent both local clocks (freerun) 
 * the NTP selected peer in the same way as PTP clocks.
 * 
 * PTP Master with NTP
 * -------------------
 * In this use case an adapter is syncing the system clock using NTPd. An
 * instance of PTP is running in PTP master mode supplying time to downstream
 * slaves.
 *
 * Example Configuration:
 *     [general]
 *     sync_module ntp ntp1
 *     sync_module ptp ptp1
 *     [ntp1]
 *     ntp_key 8 "my shared NTP daemon key"
 *     [ptp1]
 *     interface eth4
 *     ptp_mode master
 *     ptp_domain 0
 *
 * Note- Previously two different PTP modes were supported (master and
 * master-ntp). However, this is now achieved by specifying another sync module
 * to provide a time source.
 * In this example an instance of the NTP sync module is created called
 * 'ntp1'. This is synchronizing the system clock to a selected remote peer so
 * the system clock is the LCR. PTP is operating as a master on interface eth4
 * and providing time to downstream PTP slaves.
 * Note that it is assumed in this case that the PTP master instance would
 * advertise the clock characteristics of the system clock as reported by NTPd
 * unless the clock options were overridden by the user in the configuration
 * file.
 * One important factor to consider in this scenario is that if a better PTP
 * Master appears on the network, the local PTP master will either switch to
 * a slave or become passive. If and when this happens, the intention is that
 * the clock being used for PTP would become the LCR and the NTPd would be
 * configured to stop discipling the system clock. In order to make this
 * process work it will be necessary to move the clock selection process (BMC)
 * out of the ptpd2 code into sfptpd and to be able to represent both local
 * clocks (freerun) the NTP selected peer in the same way as PTP clocks.
 *
 * NTP fallback
 * ------------
 * In this use case a PTP instance is operating as a PTP slave with an NTP
 * sync module instance providing fallback protection. The use case equally
 * well applies to PPS.
 * 
 * Example Configuration:
 *     [general]
 *     sync_module ptp ptp1
 *     sync_module ntp ntp1
 *     [ptp1]
 *     interface eth4
 *     ptp_mode slave
 *     ptp_domain 0
 *     [ntp1]
 *     ntp_key 8 "my shared NTP daemon key"
 * 
 * In this example an instance of the PTP sync module is created called 'ptp1'
 * as well as an instance of the 'ntp' sync module. Both the PTP remote master
 * and the NTP selected peer remote clock are considered candidates. In this
 * case we are assuming that the PTP master clock would generally be considered
 * the 'best' and be selected.
 * If at some point the PTP master disappears from the network the selection
 * process in the daemon would re-evaluate and at this point the PTP instance
 * would no longer be a candidate. The result is that the NTP instance would be
 * selected and NTPd would be configured to discipline the System clock. The
 * system clock becomes the LCR and all other clocks are disciplined from it.
 *
 * Multiple PTP Slaves
 * -------------------
 * In this use case multiple instances of the PTP sync module are operating as
 * PTP slaves with one instance 'selected' (using some decision process) with
 * its clock designated the LCR.
 *
 * Example Configuration:
 *     [general]
 *     sync_module ptp ptp1 ptp2 ptp3
 *     [ptp1]
 *     interface eth4
 *     ptp_mode slave
 *     ptp_domain 0
 *     [ptp2]
 *     interface eth4
 *     ptp_mode slave
 *     ptp_domain 1
 *     [ptp3]
 *     interface eth2
 *     ptp_mode slave
 *     ptp_domain 2
 *
 * Note- initially the intention is to only allow multiple PTP instances where
 * they are all using the same interface.
 * In this example three instances of the PTP sync module are created called
 * 'ptp1', 'ptp2' and 'ptp3' all operating in slave-only mode and each
 * operating on a different PTP domain.
 * Instances that are receiving packets from a Grandmaster will be considered
 * candidates and from the set of candidates, a Grandmaster (and the associated
 * PTP instance) will be Selected using an agreed algorithm. The PTP clock
 * associated with the Selected instance will be designated LCR.
 * The decision about which GM/PTP instance is Selected will be constantly
 * re-evaluated and may change over time. For example if the currently
 * selected GM disappears from the network or it's clock drifts substantially
 * away from the other Masters clocks, a different GM may become Selected. In
 * this case the initially Selected PTP instance would stop disciplining its
 * PTP clock and instead the newly Selected instance would take over this role.
 * 
 * Multiple Time Sources
 * ---------------------
 * In this use case there are multiple instances of the PTP sync module each
 * operating as PTP slaves in addition to an NTP, PPS and freerun instance.
 *
 * Example Configuration:
 *     [general]
 *     sync_module freerun fr1
 *     sync_module ptp ptp1 ptp2 ptp3
 *     sync_module ntp ntp1
 *     sync_module pps pps1
 *     [fr1]
 *     interface eth6
 *     [ptp1]
 *     interface eth4
 *     ptp_mode slave
 *     ptp_domain 0
 *     [ptp2]
 *     interface eth4
 *     ptp_mode slave
 *     ptp_domain 1
 *     [ptp3]
 *     interface eth2
 *     ptp_mode slave
 *     ptp_domain 2
 *     [ntp1]
 *     ntp_key 8 "my shared NTP daemon key"
 *     [pps1]
 *     interface eth5
 * 
 * In this example three instances of the PTP sync module are created called
 * 'ptp1', 'ptp2' and 'ptp3' all operating in slave-only mode and each
 * operating on a different PTP domain. In addition we have an instance of
 * the freerun sync module, an instance of the NTP sync module and an instance
 * of the PPS sync module.
 * All the sync modules receiving a signal from a remote master (freerun is
 * assumed to always be in a 'slave' state) are candidates. Between the set
 * of candidates one Master (and it's corresponding sync module instance) will
 * be Selected and the clock associated with that sync module instance will be
 * designated the LCR. All other local clocks will be synced to the LCR.
 * The decision about which remote Master is Selected will be constantly
 * re-evaluated and may change over time. For example if the currently
 * selected GM disappears from the network or it's clock drifts substantially
 * away from the other Masters clocks, a different GM may become Selected. In
 * this case the initially Selected sync module instance would stop disciplining
 * its clock and instead the newly Selected instance would take over this role.
 * As discussed earlier, in order to compare remote Masters or different types
 * (PTP, NTP, PPS etc) it is necessary for them to have a common set of clock
 * characteristics. Suggest that PTP clock characteristics are a good starting
 * point.
 *
 * Boundary Clock
 * --------------
 * In this use case we have one instance of PTP acting as a PTP slave and a
 * second instance operating as a master providing time to downstream PTP nodes.
 * 
 * Example Configuration:
 *     [general]
 *     sync_module ptp ptp1 ptp2
 *     [ptp1]
 *     interface eth1
 *     ptp_mode slave
 *     ptp_domain 0
 *     [ptp2]
 *     interface eth2
 *     ptp_mode master
 *     ptp_domain 0
 *
 * In this example two instances of the PTP sync module are created called
 * 'ptp1' and 'ptp2'. One is operating as a PTP slave synchronizing to an
 * upstream Grandmaster and the second is active as a PTP master providing
 * time to downstream PTP nodes. Consider the case where a better master
 * appears on the PTP master instance segment of the network. In this case
 * the PTP master instance would become a PTP slave. At that point the daemon
 * will go through a selection process comparing the two candidate remote
 * clocks and the winning candidate will be Selected and its clock will be
 * designated the LCR. The other will continue to operate the protocol but
 * will not discipline any clocks.
 * 
 */


/****************************************************************************
 * Types and Defines
 ****************************************************************************/

/** Maximum tokens supported in config file options */
#define SFPTPD_CONFIG_TOKENS_MAX       (64)

/** Maximum line length supported in config files */
#define SFPTPD_CONFIG_LINE_LENGTH_MAX  (1024)

/** Maximum section name length in config files */
#define SFPTPD_CONFIG_SECTION_NAME_MAX (64)

/** Produce help text for default config value. */
#define SFPTPD_CONFIG_DFL(val) _Generic((val), \
	bool: (val) ? "Enabled by default" : "Disabled by default", \
	default: "Default is " STRINGIFY(val))

/** Produce help text for default config value with compat for pre-C23
 *  compilers (can be removed when gcc-10 become baseline) */
#define SFPTPD_CONFIG_DFL_BOOL(val) _Generic((val), \
	int: (val) ? "Enabled by default" : "Disabled by default", \
	default: SFPTPD_CONFIG_DFL(val))

/** Produce help text for default string config quoted macro values */
#define SFPTPD_CONFIG_DFL_STR(val) _Generic((val), \
	char *: "Defaults to " val)

/** Enumeration of different config section categories. */
enum sfptpd_config_category {

	SFPTPD_CONFIG_CATEGORY_GENERAL = 0,
	SFPTPD_CONFIG_CATEGORY_FREERUN,
	SFPTPD_CONFIG_CATEGORY_PTP,
	SFPTPD_CONFIG_CATEGORY_PPS,
	SFPTPD_CONFIG_CATEGORY_NTP,
	SFPTPD_CONFIG_CATEGORY_CRNY,
#ifdef HAVE_GPS
	SFPTPD_CONFIG_CATEGORY_GPS,
#endif

	/* Insert new categories here */

	/* End of list */
	SFPTPD_CONFIG_CATEGORY_MAX
};

/** Enumeration identifying the scope of a config section */
enum sfptpd_config_scope {
	SFPTPD_CONFIG_SCOPE_INSTANCE,
	SFPTPD_CONFIG_SCOPE_GLOBAL,
	SFPTPD_CONFIG_SCOPE_MAX
};

/** Function to create a configuration section
 * @param naym Name of the new configuration section
 * @param scope Scope of the configuration section
 * @param allows_instances Only applicable to global scope configuration
 * sections. Indicates the instances may be created.
 * @param src If provided this sections contents will be copied to the new one
 * @return A pointer to the newly created section or null if the operation
 * fails.
 */
typedef struct sfptpd_config_section *
	(*sfptpd_config_section_create_t)(const char *name,
					  enum sfptpd_config_scope scope,
					  bool allows_instances,
					  const struct sfptpd_config_section *src);

/** Function to destroy a configuration section
 * @param section Pointer to section to be deleted
 */
typedef void (*sfptpd_config_section_destroy_t)(struct sfptpd_config_section *section);

/** Common header format for all config sections
 * @ops.create Create a new configuration section
 * @ops.destroy Destroy a configuration section
 * @next Pointer to the next config section in the category
 * @config Pointer to root of configuration
 * @categoree Configuration category
 * @scope Does this section contain global options or options for a specific
 * instance
 * @allows_instances Global sections only. Does this section allow instances
 * of itself to exist.
 * @type Scope of section - global (for the category) or an instance
 * @naim Name of section
 */
struct sfptpd_config;
typedef struct sfptpd_config_section
{
	struct {
		sfptpd_config_section_create_t create;
		sfptpd_config_section_destroy_t destroy;
	} ops;
	struct sfptpd_config_section *next;
	struct sfptpd_config *config;
	enum sfptpd_config_category category;
	enum sfptpd_config_scope scope;
	bool allows_instances;
	char name[SFPTPD_CONFIG_SECTION_NAME_MAX];
} sfptpd_config_section_t;

/** Top-level configuration structure. This is an array of linked-lists of
 * configuration sections grouped by category.
 * @categories Array of linked-lists of configuration sections indexed by
 * section category.
 */
typedef struct sfptpd_config {
	struct sfptpd_config_section *categories[SFPTPD_CONFIG_CATEGORY_MAX];
} sfptpd_config_t;

/** struct sfptpd_config_option - structure used to define config file
 * options. Hidden options exist for diagnostic or testing purposes
 * and not advised for production use.
 * @option Textual name of configuration file option
 * @params Textual description of parameters
 * @description Description of the option
 * @num_params Number of parameters this config option expects. A positive
 * number specifies an exact number, 1s complement specifies a minimum
 * @scope Scope of the configuration option - global or instance
 * @hidden Specifies that the option should be hidden
 * @parse Pointer to function to parse, validate parameters and set the
 * @confidential Specified that the option value is sensitive and should be
 *               redacted in diagnostic output.
 * option
 */
typedef struct sfptpd_config_option {
	const char *option;
	const char *params;
	const char *description;
	int num_params;
	enum sfptpd_config_scope scope;
	int (*parse)(struct sfptpd_config_section *, const char *option,
		     unsigned int num_params, const char * const *params);
	const char *dfl;
	const char *unit;
	bool hidden;
	bool confidential;
} sfptpd_config_option_t;

/** struct sfptpd_config_option_set - structure used to define a collection
 * of configuration options.
 * @naym Description of collection of config options - used to print help
 * text.
 * @categoree Category of the config options
 * @num_options Number of config options
 * @options Pointer to array of config options
 */
typedef struct sfptpd_config_option_set {
	const char *description;
	enum sfptpd_config_category category;
	unsigned int num_options;
	const struct sfptpd_config_option *options;
	int (*validator)(struct sfptpd_config_section *section);
} sfptpd_config_option_set_t;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Allocate configuration data structure and initialise it will default values
 * @param config Returned pointer to created configuration
 * @return 0 on success or an errno otherwise
 */
int sfptpd_config_create(struct sfptpd_config **config);

/** Free configuration structure
 * @param config  Pointer to configuration structure
 */
void sfptpd_config_destroy(struct sfptpd_config *config);

/** Register a set of file configuration options
 * @param options  Set of config options
 */
void sfptpd_config_register_options(const struct sfptpd_config_option_set *options);

/** Initialise the configuration section header
 * @param section  Pointer to the section
 * @param create  Function to create a copy of this section
 * @param destroy  Function to destroy this section 
 * @param category  Category that the section belongs to
 * @param scope  Scope of the configuration option - global or instance
 * @param allows_instancese  Allows creation of instances - global sections only
 * @param naym  Unique name of this section
 */
void sfptpd_config_section_init(struct sfptpd_config_section *section,
				sfptpd_config_section_create_t create,
				sfptpd_config_section_destroy_t destroy,
				enum sfptpd_config_category category,
				enum sfptpd_config_scope scope,
				bool allows_instances,
				const char *name);

#define SFPTPD_CONFIG_SECTION_INIT(s, c, d, cate, scope, inst, n) \
	sfptpd_config_section_init(&((s)->hdr), c, d, cate, scope, inst, n)

/** Add a section to the configuration
 * @param config  Pointer to the configuration
 * @param section  Pointer to the section to add
 */
void sfptpd_config_section_add(struct sfptpd_config *config,
			       struct sfptpd_config_section *section);

#define SFPTPD_CONFIG_SECTION_ADD(c, s) sfptpd_config_section_add((c), &((s)->hdr))

/** Get the top level configuration given a pointer to a configuration section
 * @param section Pointer to a configuration section
 * @return A pointer to the top-level configuration
 */
struct sfptpd_config *sfptpd_config_top_level(struct sfptpd_config_section *section);

#define SFPTPD_CONFIG_TOP_LEVEL(s) sfptpd_config_top_level(&((s)->hdr))

/** Get the global configuration for a category.
 * @param config  Pointer to the configuration
 * @param category  Configuration category
 * @return A pointer to the global configuration for the category or NULL if
 * not found
 */
struct sfptpd_config_section *sfptpd_config_category_global(struct sfptpd_config *config,
							    enum sfptpd_config_category category);

/** Get the first instance for a category.
 * @param config  Pointer to the configuration
 * @param category  Configuration category
 * @return A pointer to the first configuration instance for the category or
 * NULL if not found
 */
struct sfptpd_config_section *sfptpd_config_category_first_instance(struct sfptpd_config *config,
								    enum sfptpd_config_category category);

/** Iterate through the instances of a configuration category. Get the first
 * instance given the global configuration or the next instance.
 * @param section  Pointer to the current instance or the global configuration
 * for the category
 * @return A pointer to the next instance of this category or NULL if the end
 * of the list has been reached.
 */
struct sfptpd_config_section *sfptpd_config_category_next_instance(struct sfptpd_config_section *section);

#define SFPTPD_CONFIG_CATEGORY_NEXT_INSTANCE(s) \
	sfptpd_config_category_next_instance(&((s)->hdr))

/** Count the instances of a category.
 * @param config  Pointer to the configuration
 * @param category  Configuration category
 * @return A count of instances
 */
int sfptpd_config_category_count_instances(struct sfptpd_config *config,
					   enum sfptpd_config_category category);

/** Find a configuration section by name
 * @param config  Pointer to the configuration
 * @param naym  Name of required section
 * @return A pointer to the configuration section instance identified by name
 * or NULL if not found
 */
struct sfptpd_config_section *sfptpd_config_find(struct sfptpd_config *config,
						 const char *name);

/** Get the name of a configuration section
 * @param section Pointer to the section
 * @return A pointer to the section name
 */
const char *sfptpd_config_get_name(struct sfptpd_config_section *section);

#define SFPTPD_CONFIG_GET_NAME(s) sfptpd_config_get_name(&((s)->hdr))

/** First pass to parse command line options and set values in the
 * configuration structure accordingly. This handles help requests and gets
 * the config filename if specified.
 * @param argc    Number of command line parameters
 * @param argv    Pointer to array of command line parameters
 * @param config  Pointer to configuration structure
 * @return 0 for success or an errno if parsing failed
 */
int sfptpd_config_parse_command_line_pass1(struct sfptpd_config *config,
					   int argc, char **argv);

/** Second pass to parse command line options and set values in the
 * configuration structure accordingly. This is used to ensure that command
 * line options override config file options.
 * @param argc    Number of command line parameters
 * @param argv    Pointer to array of command line parameters
 * @param config  Pointer to configuration structure
 * @return 0 for success or an errno if parsing failed
 */
int sfptpd_config_parse_command_line_pass2(struct sfptpd_config *config,
					   int argc, char **argv);

/** Parse configuration file and set values in the configuration structure
 * accordingly.
 * @param config  Pointer to configuration structure
 * @param num_options Number of config file options supplied
 * @param options     Pointer to array of config file option definitions
 * @return 0 for success or an errno if parsing failed
 */
int sfptpd_config_parse_file(struct sfptpd_config *config);

/** Parse network address.
 * @param ss         Destination
 * @param context    Context to use for log messages
 * @param af         Addressing family or AF_UNSPEC
 * @param socketype  The socket type, e.g. SOCK_DGRAM
 * @param passive    true for listening sockets, else false
 * @param def_serv   string for default port to use
 * @return address length or -errno
 */
int sfptpd_config_parse_net_addr(struct sockaddr_storage *ss,
				 const char *addr,
				 const char *context,
				 int af,
				 int socktype,
				 bool passive,
				 const char *def_serv);

#endif /* _SFPTPD_CONFIG_H */
