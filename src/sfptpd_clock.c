/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_clock.c
 * @brief  Clock access abstraction
 */

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <net/if.h>
#include <limits.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/timex.h>
#include <math.h>
#include <assert.h>
#include <fts.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/sockios.h>
#include <linux/socket.h>
#include <linux/if_ether.h>

#include "efx_ioctl.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_clock.h"
#include "sfptpd_constants.h"
#include "sfptpd_statistics.h"
#include "sfptpd_time.h"
#include "sfptpd_interface.h"
#include "sfptpd_misc.h"
#include "sfptpd_thread.h"
#include "sfptpd_phc.h"


/****************************************************************************
 * Missing kernel API bits and pieces
 ****************************************************************************/

#ifndef ADJ_NANO
#define ADJ_NANO 0x2000
#endif

#ifndef ADJ_SETOFFSET
#define ADJ_SETOFFSET 0x0100
#endif


/****************************************************************************
 * Clock strata levels
 ****************************************************************************/

struct sfptpd_clock_spec {
	const char *name;
	enum sfptpd_clock_stratum stratum;
	long double accuracy;
	long double holdover;
};

static const struct sfptpd_clock_spec sfptpd_clock_specifications[] =
{
	[SFPTPD_CLOCK_STRATUM_1] = {"1", SFPTPD_CLOCK_STRATUM_1,
				    SFPTPD_CLOCK_STRATUM_1_ACCURACY_PPB,
				    SFPTPD_CLOCK_STRATUM_1_ACCURACY_PPB},
	[SFPTPD_CLOCK_STRATUM_2] = {"2", SFPTPD_CLOCK_STRATUM_2,
				    SFPTPD_CLOCK_STRATUM_2_ACCURACY_PPB,
				    SFPTPD_CLOCK_STRATUM_2_HOLDOVER_PPB},
	[SFPTPD_CLOCK_STRATUM_3E] = {"3E", SFPTPD_CLOCK_STRATUM_3E,
				     SFPTPD_CLOCK_STRATUM_3E_ACCURACY_PPB,
				     SFPTPD_CLOCK_STRATUM_3E_HOLDOVER_PPB},
	[SFPTPD_CLOCK_STRATUM_3] = {"3", SFPTPD_CLOCK_STRATUM_3,
				    SFPTPD_CLOCK_STRATUM_3_ACCURACY_PPB,
				    SFPTPD_CLOCK_STRATUM_3_HOLDOVER_PPB},
	[SFPTPD_CLOCK_STRATUM_4] = {"4", SFPTPD_CLOCK_STRATUM_4,
				    SFPTPD_CLOCK_STRATUM_4_ACCURACY_PPB,
				    SFPTPD_CLOCK_STRATUM_4_HOLDOVER_PPB},
	[SFPTPD_CLOCK_STRATUM_X] = {"undefined", SFPTPD_CLOCK_STRATUM_X,
				    SFPTPD_CLOCK_STRATUM_X_ACCURACY_PPB,
				    SFPTPD_CLOCK_STRATUM_X_HOLDOVER_PPB}
};


/****************************************************************************
 * Types, Defines and Structures
 ****************************************************************************/

#define SFPTPD_CLOCK_MAGIC (0xFACEB055)

/* Earlier drivers only supported a frequency range of +-1000000. Newer drivers
 * support a wider range and indicate the capibility using the sysfs file
 * "/sys/module/sfc/parameters/max_freq_adj" */
#define SFPTPD_NIC_CLOCK_MAX_FREQ_ADJ    (1000000.0)

/* System clock accuracy - typically worse than Statum 4. The frequency adjustment
 * is calculated at startup. */
#define SFPTPD_SYSTEM_CLOCK_STRATUM      (SFPTPD_CLOCK_STRATUM_X)

/* Textual format for NIC clock names */
#define SFPTPD_NIC_NAME_FORMAT           "phc%d"

/* Threshold for reporting failed clock comparisons */
#define CLOCK_BAD_COMPARE_WARN_THRESHOLD (16)

/* Stats ids */
enum clock_stats_ids {
	CLOCK_STATS_ID_OFFSET,
	CLOCK_STATS_ID_FREQ_ADJ,
	CLOCK_STATS_ID_SYNCHRONIZED,
	CLOCK_STATS_ID_SYNC_FAIL,
	CLOCK_STATS_ID_NEAR_EPOCH,
	CLOCK_STATS_ID_CLUSTERING,
};


typedef enum {
	SFPTPD_CLOCK_TYPE_SYSTEM,
	SFPTPD_CLOCK_TYPE_SFC,
	SFPTPD_CLOCK_TYPE_NON_SFC,
	SFPTPD_CLOCK_TYPE_XNET,
	SFPTPD_CLOCK_TYPE_MAX
} sfptpd_clock_type_t;


struct sfptpd_clock_nic {
	/* Canonical identifier for the NIC */
	int nic_id;

	/* Pointer to the primary interface for this clock */
	struct sfptpd_interface *primary_if;

	/* Handle for PHC device. A null value indicates that the PHC isn't
	 * supported or that the PHC device isn't working. */
	struct sfptpd_phc *phc;

	/* Indicates that the driver supports the EFX private ioctl. */
	bool supports_efx;

	/* Indicates that the clock supports sync status reporting. This is
	 * assumed true until proven otherwise */
	bool supports_sync_status_reporting;

	/* Hardware clock device index. In systems supporting PHC corresponds
	 * to clock device /dev/ptpX. In older systems, is simply a unique
	 * identifier for the clock */
	int device_idx;
};


struct sfptpd_clock_system {
	/* User space tick frequency */
	long double tick_freq_hz;

	/* Tick resolution in parts per billion. How many ppb is an adjustment
	 * of the tick length equivalent to */
	long double tick_resolution_ppb;

	/* Maximum frequency adjustment without using tick adjustment */
	long double max_freq_adj;

	/* Maximum and minimum tick lengths */
	long double min_tick;
	long double max_tick;

	/* Master copy of kernel status flags */
	int kernel_status;
};


struct sfptpd_clock {
	uint32_t magic;
	struct sfptpd_clock *next;

	/* Deleted flag */
	bool deleted;

	/* Clock type */
	sfptpd_clock_type_t type;

	/* Posix clock ID */
	clockid_t posix_id;

	/* Clock identifiers */
	char short_name[SFPTPD_CLOCK_SHORT_NAME_SIZE];
	char long_name[SFPTPD_CLOCK_FULL_NAME_SIZE];
	char intfs_list[SFPTPD_CLOCK_FULL_NAME_SIZE];
	char hw_id_string[SFPTPD_CLOCK_HW_ID_STRING_SIZE];
	char fname_string[SFPTPD_CLOCK_HW_ID_STRING_SIZE];
	sfptpd_clock_id_t hw_id;

	/* Indicates whether clock should be disciplined */
	bool discipline;

	/* Clock cannot be adjusted, only observed */
	bool read_only;

	/* Reference count of temporary blocks */
	int blocked_count;

	/* Boolean indicating whether to use saved clock corrections */
	bool use_clock_correction;

	/* Saved frequency correction in parts per billion */
	long double freq_correction_ppb;

	/* Clock characteristics */
	const struct sfptpd_clock_spec *spec;

	/* Maximum frequency adjustment */
	long double max_freq_adj_ppb;

	/* Clock statistics */
	struct sfptpd_stats_collection stats;

	/* Count of number of good clock comparisons since last failure. Used
	 * to rate limit warning messages */
	unsigned int good_compare_count;

	/* Union of structures for different kinds of clock */
	union {
		/* System clock specific data */
		struct sfptpd_clock_system system;
		
		/* NIC clock specific data */
		struct sfptpd_clock_nic nic;
	} u;

	/* Config options copied into state for convenience */
	bool cfg_non_sfc_nics:1;
	bool cfg_avoid_efx:1;
	bool cfg_rtc_adjust:1;

	/* Status flags */
        bool lrc_been_locked:1;
	bool initial_correction_applied:1;
};


/****************************************************************************
 * Static data
 ****************************************************************************/

static struct sfptpd_config *sfptpd_clock_config;

static struct sfptpd_clock *sfptpd_clock_list_head = NULL;
static struct sfptpd_clock *sfptpd_clock_system = NULL;

/* Shared with the interfaces module */
static pthread_mutex_t *sfptpd_clock_lock;

static const struct sfptpd_stats_collection_defn clock_stats_defns[] =
{
	{CLOCK_STATS_ID_OFFSET,       SFPTPD_STATS_TYPE_RANGE, "offset-from-reference", "ns", 3},
	{CLOCK_STATS_ID_FREQ_ADJ,     SFPTPD_STATS_TYPE_RANGE, "freq-adjustment", "ppb", 3},
	{CLOCK_STATS_ID_SYNCHRONIZED, SFPTPD_STATS_TYPE_COUNT, "synchronized"},
	{CLOCK_STATS_ID_SYNC_FAIL,    SFPTPD_STATS_TYPE_COUNT, "sync-failures"},
	{CLOCK_STATS_ID_NEAR_EPOCH,   SFPTPD_STATS_TYPE_COUNT, "epoch-alarms"},
	{CLOCK_STATS_ID_CLUSTERING,   SFPTPD_STATS_TYPE_COUNT, "clustering-alarms"},
};

/* Define the uninitialised clock identity.
 */
const struct sfptpd_clock_id SFPTPD_CLOCK_ID_UNINITIALISED = {
	{ 0, 0, 0, 0, 0, 0, 0, 0 }
};

enum clock_format_id {
	CLOCK_FMT_PHC_INDEX,
	CLOCK_FMT_INTFS,
	CLOCK_FMT_HW_ID,
	CLOCK_FMT_HW_ID_NO_SEP,
};

static size_t clock_interpolate(char *buffer, size_t space, int id, void *context, char opt);

/* %P   phc device index
 * %I   interface list, separated by '/'
 * %Cx  clock id with separator 'x'
 * %D   clock id with no separator
 */
static struct sfptpd_interpolation clock_format_specifiers[] = {
	{ CLOCK_FMT_PHC_INDEX,       'P', false, clock_interpolate },
	{ CLOCK_FMT_INTFS,           'I', false, clock_interpolate },
	{ CLOCK_FMT_HW_ID,           'C', true,  clock_interpolate },
	{ CLOCK_FMT_HW_ID_NO_SEP,    'D', false, clock_interpolate },
	{ SFPTPD_INTERPOLATORS_END }
};

/****************************************************************************
 * Clock Internal Functions
 ****************************************************************************/

static inline void clock_lock(void)
{
	int rc = pthread_mutex_lock(sfptpd_clock_lock);
	if (rc != 0) {
		CRITICAL("clock: could not acquire hardware state lock\n");
		exit(1);
	}
}


static inline void clock_unlock(void)
{
	int rc = pthread_mutex_unlock(sfptpd_clock_lock);
	if (rc != 0) {
		CRITICAL("clock: could not release hardware state lock\n");
		exit(1);
	}
}


static size_t clock_interpolate(char *buffer, size_t space, int id, void *context, char opt)
{
	const struct sfptpd_clock *clock = (const struct sfptpd_clock *) context;
	const sfptpd_clock_id_t hw_id = clock->hw_id;

	assert(clock != NULL);
	assert(buffer != NULL || space == 0);

	switch (id) {
	case CLOCK_FMT_PHC_INDEX:
		return snprintf(buffer, space, "%d", clock->u.nic.device_idx);
	case CLOCK_FMT_INTFS:
		return snprintf(buffer, space, "%s", clock->intfs_list);
	case CLOCK_FMT_HW_ID:
		return snprintf(buffer, space, SFPTPD_FORMAT_EUI64_SEP,
				hw_id.id[0], hw_id.id[1], opt, hw_id.id[2], hw_id.id[3], opt,
				hw_id.id[4], hw_id.id[5], opt, hw_id.id[6], hw_id.id[7]);
	case CLOCK_FMT_HW_ID_NO_SEP:
		return snprintf(buffer, space, SFPTPD_FORMAT_EUI64_NOSEP,
				hw_id.id[0], hw_id.id[1], hw_id.id[2], hw_id.id[3],
				hw_id.id[4], hw_id.id[5], hw_id.id[6], hw_id.id[7]);
	default:
		return 0;
	}
}


static void clock_dump_header(const char *title, int trace_level)
{
	const char *heading    = "  | type    | nic_id | clk    | phc diff method    | short name | long name\n";
	const char *separator  = "  +---------+--------+--------+--------------------+------------+----------\n";

	TRACE_LX(trace_level, "%s clocks list:-\n", title);
	TRACE_LX(trace_level, "%s", heading);
	TRACE_LX(trace_level, "%s", separator);
}

static void clock_dump_record(struct sfptpd_clock *clock, int trace_level)
{
	const char *sys_pat = "  | %-7s | %-36s | %-10s | %s%s\n";
	const char *nic_pat = "  | %-7s | %6d | %6d | %-18s | %-10s | %s%s%s\n";

	if (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM) {
	  TRACE_LX(trace_level,
		   sys_pat, "sys", "",
		   clock->short_name, clock->long_name,
		   clock->read_only ? " [read-only]" : "");
	} else {
	  TRACE_LX(trace_level,
		   nic_pat,
		   clock->type == SFPTPD_CLOCK_TYPE_SFC ? "sfc" :
			(clock->type == SFPTPD_CLOCK_TYPE_XNET ? "xnet" : "non-sfc"),
		   clock->u.nic.nic_id, clock->u.nic.device_idx,
		   sfptpd_phc_get_diff_method_name(clock->u.nic.phc),
		   clock->short_name, clock->long_name,
		   clock->deleted ? " [deleted]" : "",
		   clock->read_only ? " [read-only]" : "");
	}
}

static void clock_dump_list(const char *title, struct sfptpd_clock *list, int trace_level) {
	struct sfptpd_clock *clock;

	clock_dump_header(title, trace_level);

	for (clock = list; clock != NULL; clock = clock->next) {
		if (!clock->deleted) {
		  clock_dump_record(clock, trace_level);
		}
	}
}


void sfptpd_clock_diagnostics(int trace_level)
{
	clock_dump_list("requested", sfptpd_clock_list_head, trace_level);
}


static int clock_init_common(struct sfptpd_clock *clock,
			     struct sfptpd_config_general *config,
			     sfptpd_clock_type_t type)
{
	int rc;
	assert(config != NULL);
	assert(clock != NULL);
	assert(type < SFPTPD_CLOCK_TYPE_MAX);

	clock->magic = SFPTPD_CLOCK_MAGIC;
	clock->next = NULL;
	clock->deleted = false;
	clock->type = type;
	clock->posix_id = POSIX_ID_NULL;
	clock->discipline = false;
        clock->lrc_been_locked = false;

	/* If the configuration specifies no adjustment of clocks, mark the
	 * clock as read only */
	clock->read_only = (config->clocks.control == SFPTPD_CLOCK_CTRL_NO_ADJUST);

	/* Copy the configuration's non_sfc_nics into the clock */
	clock->cfg_non_sfc_nics = config->non_sfc_nics;
	clock->cfg_avoid_efx = config->avoid_efx;
	clock->cfg_rtc_adjust = config->rtc_adjust;

	/* Record whether to use saved clock corrections */
	clock->use_clock_correction = config->clocks.persistent_correction;
	clock->freq_correction_ppb = 0.0;

	clock->good_compare_count = 0;

	/* Create the statistics collection */
	rc = sfptpd_stats_collection_create(&clock->stats, "clock",
					    sizeof(clock_stats_defns)/sizeof(clock_stats_defns[0]),
					    clock_stats_defns);
	return rc;
}

static bool is_system_clock(const struct sfptpd_clock *clock)
{
	assert(clock);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	return (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM ||
		clock == sfptpd_clock_get_system_clock());
}

/* Checks if the cfg name matches the clocks' name, HW ID, or one of its interface names */
static bool check_clock_in_config(struct sfptpd_clock *clock,
				const char* cfg_name)
{
	return ((strcmp(clock->short_name, cfg_name) == 0) ||
		(strcmp(clock->long_name, cfg_name) == 0) ||
		(strcmp(clock->hw_id_string, cfg_name) == 0) ||
		(!is_system_clock(clock) &&
		sfptpd_check_clock_interfaces(clock->u.nic.device_idx, cfg_name)));
}

/* Set the clock as readonly if it is appears in clock_readonly in the config file */
static void configure_clock_readonly(struct sfptpd_clock *clock,
					struct sfptpd_config_general *cfg, int readonly_index)
{
        char* cfg_name = cfg->clocks.readonly_clocks[readonly_index];
        if (check_clock_in_config(clock, cfg_name)) {
                clock->discipline = false;
                if (!clock->read_only) {
                        clock->read_only = true;
                        cfg->clocks.readonly_clocks_applied[readonly_index] = CLOCK_OPTION_APPLIED;
                        NOTICE("clock %s won't ever be stepped or slewed due to clock-readonly configuration\n",
                                        clock->long_name);
                } else if (cfg->clocks.readonly_clocks_applied[readonly_index] == CLOCK_OPTION_NOT_APPLIED) {
                        cfg->clocks.readonly_clocks_applied[readonly_index] = CLOCK_OPTION_ALREADY_APPLIED;
                }
        }
}

/* Set the clock as being disciplined if it appears in clock_list in the config file */
static int configure_clock_list(struct sfptpd_clock *clock,
					struct sfptpd_config_general *cfg, int clock_index)
{
        char* cfg_name = cfg->clocks.clocks[clock_index];
        if (!clock->read_only && check_clock_in_config(clock, cfg_name)) {
                if (!clock->discipline) {
                        if ((clock->type == SFPTPD_CLOCK_TYPE_NON_SFC) &&
                                !cfg->non_sfc_nics) {
                                ERROR("clock %s: disciplining non-sfc clocks is not enabled\n",
                                        clock->long_name);
                                return EINVAL;
                        }
                        TRACE_L3("clock %s (%s) will be disciplined\n",
                                        clock->hw_id_string, clock->long_name);
                        clock->discipline = true;
                        cfg->clocks.clock_list_applied[clock_index] = CLOCK_OPTION_APPLIED;
                } else if (cfg->clocks.clock_list_applied[clock_index] == CLOCK_OPTION_NOT_APPLIED) {
                        cfg->clocks.clock_list_applied[clock_index] = CLOCK_OPTION_ALREADY_APPLIED;
                }
        }
        return 0;
}

static int configure_new_clock(struct sfptpd_clock *clock,
			       struct sfptpd_config_general *cfg)
{
	int i;
	int rc = 0;
	/* If the configuration specifies readonly clocks, mark it so.
	   The short_name and long_name are names like phc0, phc1 etc.
	   They are not interface names.
	*/
	for (i = 0; i < cfg->clocks.num_readonly_clocks; i++) {
		configure_clock_readonly(clock, cfg, i);
	}

	if (!clock->read_only && cfg->clocks.discipline_all) {
		/* If the discipline all config flag is set, mark clock to
		 * be disciplined */
		if ((clock->type != SFPTPD_CLOCK_TYPE_NON_SFC) || cfg->non_sfc_nics)
			clock->discipline = true;
	} else if (!clock->read_only) {
		/* See if this clock is in the list of clocks specified to be
		   disciplined and mark clock accordingly. */
		for (i = 0; i < cfg->clocks.num_clocks; i++) {
			rc = configure_clock_list(clock, cfg, i);
		}
	}

	return rc;
}

static void fixup_clock(struct sfptpd_clock *clock, struct sfptpd_config_general *cfg) {
        int i;
        /* If any clock_readonly names is an interface associated with the clock, mark the clock as readonly. */
        for (i = 0; i < cfg->clocks.num_readonly_clocks; i++) {
                configure_clock_readonly(clock, cfg, i);
        }

        /* Do the same for clock_list */
        for (i = 0; i < cfg->clocks.num_clocks; i++) {
                configure_clock_list(clock, cfg, i);
        }

        /* Now that we have configured the clock's readonly flag, we can finally load saved frequency corrections
           and epoch startup correction.
        */
        sfptpd_clock_correct_new(clock);

        return;
}

void fixup_readonly_and_clock_lists()
{
        int i;
        struct sfptpd_clock *clock;
        struct sfptpd_config_general *cfg = sfptpd_general_config_get(sfptpd_clock_config);
        /*
           Go through each clock and check if the clock's interfaces are in the
           blacklisted interfaces list in clock_readonly.
        */
        clock_lock();
        for (clock = sfptpd_clock_list_head; clock != NULL; clock = clock->next) {
                if (clock->read_only) {
                        continue;
                }
                assert(clock->magic == SFPTPD_CLOCK_MAGIC);
                fixup_clock(clock, cfg);
        }
        clock_unlock();

        /* Check which readonly configs have not been applied */
        for (i = 0; i < cfg->clocks.num_readonly_clocks; i++) {
                char* cfg_name = cfg->clocks.readonly_clocks[i];
                if (cfg->clocks.readonly_clocks_applied[i] == CLOCK_OPTION_NOT_APPLIED) {
                        WARNING("clock_readonly argument %s was not applied \n", cfg_name);
                } else if (cfg->clocks.readonly_clocks_applied[i] == CLOCK_OPTION_ALREADY_APPLIED) {
                        INFO("clock_readonly argument %s is redundant \n", cfg_name);
                }
        }

        /* Do the same for clock_list */
        for (i = 0; i < cfg->clocks.num_clocks; i++) {
                char* cfg_name = cfg->clocks.clocks[i];
                if (cfg->clocks.clock_list_applied[i] == CLOCK_OPTION_NOT_APPLIED) {
                        WARNING("clock_list argument %s was not applied \n", cfg_name);
                } else if (cfg->clocks.clock_list_applied[i] == CLOCK_OPTION_ALREADY_APPLIED) {
                        INFO("clock_list argument %s is redundant \n", cfg_name);
                }
        }
}

static int new_system_clock(struct sfptpd_config_general *config,
			    struct sfptpd_clock **clock)
{
	struct sfptpd_clock *new;
	struct timex t;
	int rc;

	assert(config != NULL);
	assert(clock != NULL);

	new = (struct sfptpd_clock *)calloc(1, sizeof(*new));
	if (new == NULL) {
		*clock = NULL;
		return ENOMEM;
	}

	rc = clock_init_common(new, config, SFPTPD_CLOCK_TYPE_SYSTEM);
	if (rc != 0) {
		free(new);
		*clock = NULL;
		return rc;
	}

	new->posix_id = CLOCK_REALTIME;

	sfptpd_strncpy(new->short_name, "system", sizeof(new->short_name));
	sfptpd_strncpy(new->long_name, "system", sizeof(new->long_name));
	memset(&new->hw_id, 0, sizeof(new->hw_id));
	sfptpd_strncpy(new->hw_id_string, "system", sizeof(new->hw_id_string));
	sfptpd_strncpy(new->fname_string, "system", sizeof(new->fname_string));

	/* Work out some parameters for adjusting the clock using tick length
	 * adjustment for cases where the frequency adjustment exceeds the
	 * maximum */
	t.modes = 0;
	adjtimex(&t);
	new->u.system.tick_freq_hz = (long double)sysconf(_SC_CLK_TCK);
	new->u.system.max_freq_adj = (long double)t.tolerance / (((1 << 16) + 0.0) / 1000.0);
	new->u.system.tick_resolution_ppb = new->u.system.tick_freq_hz * 1000.0;
	new->u.system.min_tick = -100000.0 / new->u.system.tick_freq_hz;
	new->u.system.max_tick = 100000.0 / new->u.system.tick_freq_hz;
	new->u.system.kernel_status = STA_UNSYNC;

	/* Set a nominal value for the NIC clock accuracy and maximum frequency
	 * adjustment */
	new->spec = &sfptpd_clock_specifications[SFPTPD_SYSTEM_CLOCK_STRATUM];

	/* Calculate the combined maximum frequency adjustment (frequency 
	 * adjustment plus tick length adjustment */
	new->max_freq_adj_ppb = new->u.system.max_tick * new->u.system.tick_resolution_ppb
			      + new->u.system.max_freq_adj;

	/* Keep a pointer to the singleton */
	assert(sfptpd_clock_system == NULL);
	sfptpd_clock_system = new;

	configure_new_clock(new, config);
	sfptpd_clock_correct_new(new);

	*clock = new;
	return 0;
}


static void clock_determine_stratum(struct sfptpd_clock *clock)
{
	/* Currently the only Solarflare adapter that does not have a TCXO is
	 * the SFN7002. We assume that all non-Solarflare adapters have
	 * standard crystals too. */
	enum sfptpd_clock_stratum stratum;

	assert(clock != NULL);

	stratum = sfptpd_interface_get_clock_stratum(clock->u.nic.primary_if);
	if (stratum == SFPTPD_CLOCK_STRATUM_MAX) {
		if (clock->type == SFPTPD_CLOCK_TYPE_XNET ||
		    clock->type == SFPTPD_CLOCK_TYPE_SFC) {
			stratum = SFPTPD_NIC_TCXO_CLOCK_STRATUM;
		} else {
			stratum = SFPTPD_NIC_XO_CLOCK_STRATUM;
		}
	}

	assert(stratum < SFPTPD_CLOCK_STRATUM_MAX);
	clock->spec = &sfptpd_clock_specifications[stratum];
}


static void clock_determine_max_freq_adj(struct sfptpd_clock *clock)
{
	int max_freq_adj;
	bool success;
	struct sfptpd_config_general *general_config;

	assert(clock != NULL);
	assert(clock->u.nic.phc != NULL);
	assert(clock->type != SFPTPD_CLOCK_TYPE_SYSTEM);

	clock->max_freq_adj_ppb = 0.0;
	success = false;

	/* Get the maximum frequency adjustment using an IOCTL operation
	 * to the device. */
	max_freq_adj = sfptpd_phc_get_max_freq_adj(clock->u.nic.phc);
	clock->max_freq_adj_ppb = (long double)max_freq_adj;
	success = true;

	/* If previous attempts to find the maximum frequency adjustment have
	 * failed, fallback to a default value. */
	if (!success) {
		clock->max_freq_adj_ppb = SFPTPD_NIC_CLOCK_MAX_FREQ_ADJ;
		WARNING("clock %s: failed to determine max frequency adjustment- "
			"assuming %Lf\n",
			clock->short_name, clock->max_freq_adj_ppb);
	}

	/* Apply overriding limit if set by user */
	general_config = sfptpd_general_config_get(sfptpd_clock_config);
	if (clock->max_freq_adj_ppb > general_config->limit_freq_adj) {
		INFO("clock %s: limiting discovered max freq adj of %Lf to "
		     "configured limit of %Lf\n",
		     clock->short_name,
		     clock->max_freq_adj_ppb,
		     general_config->limit_freq_adj);
		clock->max_freq_adj_ppb = general_config->limit_freq_adj;
	}
}


static int clock_compare_using_efx(void *context, struct sfptpd_timespec *diff)
{
	struct sfptpd_clock *clock = (struct sfptpd_clock *) context;
	struct efx_sock_ioctl req = { .cmd = EFX_TS_SYNC };
	int rc;

	rc = sfptpd_interface_ioctl(clock->u.nic.primary_if, SIOCEFX, &req);
	if (rc == 0)
		/* Store the difference- this is (Tptp - Tsys) */
		sfptpd_time_init(diff, req.u.ts_sync.ts.tv_sec, req.u.ts_sync.ts.tv_nsec, 0);

	return rc;
}


static int renew_clock(struct sfptpd_clock *clock)
{
	struct sfptpd_db_query_result interface_index_snapshot;
	struct sfptpd_config_general *general_config;
	struct sfptpd_interface *interface, *primary;
	int i, nic_id;
	int phc_idx;
	bool supports_phc = false;
	bool supports_efx;
	bool change = false;
	int rc = 0;

	assert(clock != NULL);

	if (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM)
		return 0;

	TRACE_L4("Renewing clock nic%d (currently phc%d, interface %s)\n",
		 clock->u.nic.nic_id,
		 clock->u.nic.device_idx,
		 sfptpd_interface_get_name(clock->u.nic.primary_if));

	general_config = sfptpd_general_config_get(sfptpd_clock_config);

	/* Get an index of the active PTP capable interfaces. Note that the
	 * index is ordered by NIC ID and then by increasing MAC address.
	 * This hugely reduces the pain of initialising the clock. */
	interface_index_snapshot = sfptpd_interface_get_active_ptp_snapshot();

	/* Find the primary interface associated with the clock. This will be the
	 * first interface we come to with the matching NIC ID. */
	primary = NULL;
	for (i = 0; (i < interface_index_snapshot.num_records) &&
		    ((primary == NULL) || !supports_phc); i++) {
		struct sfptpd_interface **intfp = interface_index_snapshot.record_ptrs[i];
		interface = *intfp;

		nic_id = sfptpd_interface_get_nic_id(interface);
		if (nic_id == clock->u.nic.nic_id) {
			primary = interface;
			sfptpd_interface_get_clock_device_idx(interface,
							      &supports_phc,
							      &phc_idx,
							      &supports_efx);
		}
	}

	if (primary != clock->u.nic.primary_if ||
	    (supports_phc && clock->u.nic.phc == NULL) ||
	    (!supports_phc && clock->u.nic.phc != NULL) ||
	    supports_efx != clock->u.nic.supports_efx ||
	    phc_idx != clock->u.nic.device_idx)
		change = true;

	/* If we found an interface associated with the clock then the clock
	 * is alive and active. If we didn't find an interface then this clock
	 * and the interfaces associated with it have been deleted. */
	if (primary != NULL && supports_phc) {
		int name_len;
		sfptpd_mac_addr_t mac;

		if (clock->deleted) {
			change = true;
			clock->deleted = false;
		}

		/* Set the primary interface for the clock and get the PHC
		 * supported flag and device index. */
		clock->u.nic.primary_if = primary;
		clock->u.nic.supports_sync_status_reporting = !clock->cfg_avoid_efx;
		clock->u.nic.device_idx = phc_idx;
		clock->u.nic.supports_efx = supports_efx;

		sfptpd_format(clock_format_specifiers, clock, clock->short_name,
			      sizeof clock->short_name, general_config->clocks.format_short);

		if (clock->u.nic.phc == NULL) {
			rc = sfptpd_phc_open(clock->u.nic.device_idx,
					     &clock->u.nic.phc);
			if (rc != 0) {
				ERROR("clock %s: failed to open PHC device %d, %s\n",
				      clock->short_name, clock->u.nic.device_idx,
				      strerror(errno));
				clock->u.nic.device_idx = -1;
				clock->posix_id = POSIX_ID_NULL;
				return errno;
			}
			clock->posix_id = sfptpd_phc_get_clock_id(clock->u.nic.phc);
			if (clock->u.nic.supports_efx) {
				sfptpd_phc_define_diff_method(clock->u.nic.phc,
							      SFPTPD_DIFF_METHOD_EFX,
							      clock_compare_using_efx,
							      clock);
			}

			/* Capture the return code to propagate critical startup
			   failures but continue function to complete
			   housekeeping: caller can decide what to do. */
			rc = sfptpd_phc_start(clock->u.nic.phc);
		}

		/* Create the interfaces list typically used in the long clock name. */
		name_len = snprintf(clock->intfs_list, sizeof(clock->intfs_list),
				    "%s", sfptpd_interface_get_name(clock->u.nic.primary_if));
		if (name_len > sizeof(clock->intfs_list)) name_len = sizeof(clock->intfs_list);
		for ( ; i < interface_index_snapshot.num_records; i++) {
			struct sfptpd_interface **intfp = interface_index_snapshot.record_ptrs[i];
			interface = *intfp;
			nic_id = sfptpd_interface_get_nic_id(interface);
			if (nic_id == clock->u.nic.nic_id) {
				name_len += snprintf(clock->intfs_list + name_len,
						     sizeof(clock->intfs_list) - name_len,
						     "/%s",
						     sfptpd_interface_get_name(interface));
				if (name_len > sizeof(clock->intfs_list))
					name_len = sizeof(clock->intfs_list);
			}
		}

		/* Write out the long clock name. */
		sfptpd_format(clock_format_specifiers, clock, clock->long_name,
			      sizeof clock->long_name, general_config->clocks.format_long);

		/* IEEEE Std 1588-2019 7.5.2.2.2.2 requires clock IDs (EUI-64)
		 * constructed from EUI-48 MAC addresses to left align the
		 * MAC address and add unique bits to the RHS (LSBs). This
		 * differs from the 1588-2008 __:__:__:ff:fe:__:__:__ style.
		 *
		 * The unique bits are configurable, which supports use cases
		 * such as multiple PTP daemons which might be operating in
		 * different time domains simultaneously or would otherwise
		 * need to avoid clashing port IDs. */
		sfptpd_interface_get_mac_addr(clock->u.nic.primary_if, &mac);

		if (general_config->legacy_clockids && mac.len == 6) {
			memcpy(clock->hw_id.id, mac.addr, 3);
			clock->hw_id.id[3] = 0xff;
			clock->hw_id.id[4] = 0xfe;
			memcpy(clock->hw_id.id + 5, mac.addr + 3, 3);
		} else {
			memcpy(clock->hw_id.id, general_config->unique_clockid_bits, 8);
			memcpy(clock->hw_id.id, mac.addr, mac.len < sizeof clock->hw_id.id ?
							  mac.len : sizeof clock->hw_id.id);
		}

		sfptpd_format(clock_format_specifiers, clock, clock->hw_id_string,
			      sizeof clock->hw_id_string, general_config->clocks.format_hwid);
		sfptpd_format(clock_format_specifiers, clock, clock->fname_string,
			      sizeof clock->fname_string, general_config->clocks.format_fnam);

		/* Get the NIC clock specification if known. Otherwise assume
		 * stratum 4 i.e. a normal crystal oscillator. */
		clock_determine_stratum(clock);

		/* Determine the maximum frequency adjustment for the clock. */
		clock_determine_max_freq_adj(clock);

		if (change) {
			TRACE_L3("clock %s: stratum %s, accuracy %.3Lf ppb, holdover %.3Lf ppb\n",
				 clock->short_name, clock->spec->name,
				 clock->spec->accuracy, clock->spec->holdover);
			TRACE_L3("clock %s: id %s, max freq adj %.3Lf ppb\n",
				 clock->short_name,
				 clock->hw_id_string,
				 clock->max_freq_adj_ppb);
		}
	} else {
		if (!clock->deleted) {
			change = true;
			clock->deleted = true;
		}

		/* Use an old interface as primary placeholder */
		interface = sfptpd_interface_find_first_by_nic(clock->u.nic.nic_id);
		clock->u.nic.primary_if = interface;
		clock->u.nic.supports_sync_status_reporting = false;
		clock->u.nic.supports_efx = false;
		clock->u.nic.device_idx = -1;

		/* If we have a PHC device open, close it */
		if (clock->u.nic.phc)
			sfptpd_phc_close(clock->u.nic.phc);
		clock->u.nic.phc = NULL;
		clock->posix_id = POSIX_ID_NULL;

		snprintf(clock->short_name, sizeof(clock->short_name), "(deleted)");
		snprintf(clock->long_name, sizeof(clock->long_name), "(deleted)");
		memset(clock->hw_id.id, 0, sizeof(clock->hw_id.id));
		sfptpd_clock_init_hw_id_string(clock->hw_id_string, clock->hw_id,
					       sizeof(clock->hw_id_string));

		TRACE_L4("clock %s: is deleted\n", clock->short_name);
	}

	interface_index_snapshot.free(&interface_index_snapshot);

	return rc;
}


static int new_nic_clock(int nic_id, sfptpd_clock_type_t type,
			 struct sfptpd_config_general *config,
			 struct sfptpd_clock **clock)
{
	struct sfptpd_clock *new;
	int rc = 0;

	assert(nic_id >= 0);
	assert((type == SFPTPD_CLOCK_TYPE_SFC) ||
	       (type == SFPTPD_CLOCK_TYPE_XNET) ||
	       (type == SFPTPD_CLOCK_TYPE_NON_SFC));
	assert(config != NULL);
	assert(clock != NULL);

	*clock = NULL;

	/* Allocate a clock instance */
	new = (struct sfptpd_clock *)calloc(1, sizeof(*new));
	if (new == NULL) {
		rc = ENOMEM;
		goto finish;
	}

	rc = clock_init_common(new, config, type);
	if (rc != 0) {
		free(new);
		goto finish;
	}

	new->u.nic.nic_id = nic_id;
	new->u.nic.phc = NULL;

	rc = renew_clock(new);
	if (rc != 0) {
		sfptpd_stats_collection_free(&new->stats);
		free(new);
		goto finish;
	}

	*clock = new;

finish:
	return rc;
}


static void clock_delete(struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	/* If this is not the system clock we need to close the PHC device */
	if ((clock->type != SFPTPD_CLOCK_TYPE_SYSTEM) && (clock->u.nic.phc != NULL)) {
		sfptpd_phc_close(clock->u.nic.phc);
	}

	sfptpd_stats_collection_free(&clock->stats);
	free(clock);
}


struct sfptpd_clock *sfptpd_clock_find_by_nic_id(int nic_id)
{
	struct sfptpd_clock *clock = NULL;

	clock_lock();
	for (clock = sfptpd_clock_list_head; clock != NULL; clock = clock->next) {
		assert(clock->magic == SFPTPD_CLOCK_MAGIC);
		if (clock->type != SFPTPD_CLOCK_TYPE_SYSTEM &&
		    clock->u.nic.nic_id == nic_id) {
			break;
		}
	}
	clock_unlock();

	return clock;
}


static void sfptpd_clock_init_interface(int nic_id,
					struct sfptpd_interface *interface) {
	struct sfptpd_clock *clock;
	bool supports_phc;
	bool supports_efx;
	int clock_dev_idx, rc;
	struct sfptpd_config_general *general_config;
	sfptpd_clock_type_t type = SFPTPD_CLOCK_TYPE_NON_SFC;
	bool is_new = false;

	assert(nic_id >= 0);
	assert(interface != NULL);

	general_config = sfptpd_general_config_get(sfptpd_clock_config);

	if (sfptpd_interface_supports_ptp(interface)) {
		clock = sfptpd_clock_find_by_nic_id(nic_id);
		if (clock == NULL) {
			is_new = true;
			sfptpd_interface_get_clock_device_idx(interface,
							      &supports_phc,
							      &clock_dev_idx,
							      &supports_efx);
			if (clock_dev_idx < 0) {
				WARNING("clock: interface %s of nic %d is no longer PTP capable",
					sfptpd_interface_get_name(interface),
					nic_id);
				sfptpd_interface_set_clock(interface, sfptpd_clock_system);
				return;
			}

			switch (sfptpd_interface_get_class(interface)) {
			case SFPTPD_INTERFACE_SFC:
				type = SFPTPD_CLOCK_TYPE_SFC;
				break;
			case SFPTPD_INTERFACE_XNET:
				type = SFPTPD_CLOCK_TYPE_XNET;
				break;
			case SFPTPD_INTERFACE_OTHER:
				type = SFPTPD_CLOCK_TYPE_NON_SFC;
			}

			rc = new_nic_clock(nic_id, type, general_config, &clock);
			if (rc != 0) {
				CRITICAL("failed to create nic clock idx %d, %s\n",
					 clock_dev_idx, strerror(rc));
				return;
			}

			clock->next = sfptpd_clock_list_head;
			sfptpd_clock_list_head = clock;
		} else if (clock->deleted) {
			/* If a previously-removed NIC is re-inserted we
			   may need to step the clock */
			sfptpd_clock_correct_new(clock);
		}

		/* Link the existing or newly created clock to the interface */
		sfptpd_interface_set_clock(interface, clock);

		if (is_new) {
			/* Set disciplining state for new clock */
			rc = configure_new_clock(clock, general_config);
			if (rc != 0) {
				sfptpd_clock_shutdown();
				return;
			}
		}

	} else {
		/* For interfaces that don't have a hardware PTP
		 * function, point the interface at the system clock */
		sfptpd_interface_set_clock(interface, sfptpd_clock_system);
	}
}


static void clock_record_step(void)
{
	struct sfptpd_clock *clock = NULL;

	clock_lock();
	for (clock = sfptpd_clock_list_head; clock != NULL; clock = clock->next) {
		assert(clock->magic == SFPTPD_CLOCK_MAGIC);
		if (clock->type != SFPTPD_CLOCK_TYPE_SYSTEM)
			sfptpd_phc_record_step(clock->u.nic.phc);
	}
	clock_unlock();
}


/****************************************************************************
 * Public Functions
 *
 * In general all public functions in this module will acquire and release
 * the hardware state lock.
 *
 * There are some exceptions:
 *
 *   - functions which do not alter any state reflecting the hardware that
 *     their users would require to be in a consistent state.
 *
 *   - functions involved in iterating through the list: this is because
 *     it is safe to operate on an old version of the list as we do not
 *     delete the nodes, the next pointers remain valid and at the only
 *     current call site, the process will be triggered again once operations
 *     resulting from the underlying hardware change have completed.
 ****************************************************************************/

/* Not locked because this is called from startup and indeed saves the mutex */
int sfptpd_clock_initialise(struct sfptpd_config *config, pthread_mutex_t *hardware_state_lock)
{
	int rc = 0;
	struct sfptpd_config_general *general_config;

	sfptpd_clock_lock = hardware_state_lock;

	clock_lock();

	sfptpd_clock_config = config;
	general_config = sfptpd_general_config_get(config);
	
	/* Create a clock instance for the system clock */
	rc = new_system_clock(general_config, &sfptpd_clock_system);
	if (rc != 0) {
		CRITICAL("failed to create system clock instance\n");
		goto finish;
	}

	/* Initialise the linked list of clocks */
	sfptpd_clock_list_head = sfptpd_clock_system;

	/* Initialise the PHC subsystem */
	sfptpd_phc_set_diff_methods(general_config->phc_diff_methods);
	sfptpd_phc_set_pps_methods(general_config->phc_pps_method);

 finish:
	clock_unlock();
	return rc;
}


void sfptpd_clock_rescan_interfaces(void) {
	struct sfptpd_clock *clock;
	struct sfptpd_db_query_result interface_index_snapshot;
	int i;

	clock_lock();
	interface_index_snapshot = sfptpd_interface_get_active_ptp_snapshot();

	/* Iterate over the interfaces, find the clock associated with each
	 * interface and link the two. Where a clock doesn't exist, create it.
	 */
	for (i = 0; i < interface_index_snapshot.num_records; i++) {
		struct sfptpd_interface **intfp = interface_index_snapshot.record_ptrs[i];
		struct sfptpd_interface *intf = *intfp;
		int nic_id = sfptpd_interface_get_nic_id(intf);

		assert(!sfptpd_interface_is_deleted(intf));
		assert(sfptpd_interface_get_nic_id(intf) != -1);

		sfptpd_clock_init_interface(nic_id, intf);
	}

	for (clock = sfptpd_clock_list_head; clock != NULL; clock = clock->next) {
		/* Renew clock can theory fail if we are unable to open the PHC
		 * device. This may well happen in the hot-plug/breaker scenarios
		 * that Stratus have although it is likely to be a transitory
		 * condition. The result of failure is that the clock will not be
		 * usable for PTP but we have to carry on as best we can. */
		(void)renew_clock(clock);
	}

	interface_index_snapshot.free(&interface_index_snapshot);

	clock_dump_list("all", sfptpd_clock_list_head, 4);
	clock_unlock();
}


void sfptpd_clock_shutdown(void)
{
	struct sfptpd_clock *clock, *next;

	/* This happens if main exits before calling our initialise. */
	if (sfptpd_clock_list_head == NULL) return;

	clock_lock();
	for (clock = sfptpd_clock_list_head; clock != NULL; clock = next) {
		assert(clock->magic == SFPTPD_CLOCK_MAGIC);
		next = clock->next;
		clock_delete(clock);
	}
	sfptpd_clock_list_head = NULL;
	clock_unlock();
}


int sfptpd_clock_get_total(void)
{
	int count = 0;
	struct sfptpd_clock *node;

	clock_lock();
	for (node = sfptpd_clock_list_head; node != NULL; node = node->next)
		count++;
	clock_unlock();

	return count;
}

static int ptr_compar(const void *a, const void *b)
{
	const void **pa = (const void **) a;
	const void **pb = (const void **) b;

	if (*pa < *pb)
		return -1;
	else if (*pa > *pb)
		return 1;
	else
		return 0;
}

struct sfptpd_clock **sfptpd_clock_get_active_snapshot(size_t *num_clocks)
{
	struct sfptpd_clock **snapshot;
	struct sfptpd_clock *node;
	size_t count;
	int index;

	clock_lock();
	count = 0;
	for (node = sfptpd_clock_list_head; node != NULL; node = node->next) {
		assert(node->magic == SFPTPD_CLOCK_MAGIC);
		if (!node->deleted) count++;
	}

	snapshot = calloc(count, sizeof *snapshot);
	if (!snapshot) {
		ERROR("clock: error allocating space for active clock snapshot, %s\n",
		      strerror(errno));
		goto finish;
	}

	index = 0;
	for (node = sfptpd_clock_list_head; node != NULL; node = node->next) {
		assert(node->magic == SFPTPD_CLOCK_MAGIC);
		if (!node->deleted) {
			assert(index < count);
			snapshot[index++] = node;
		}
	}
	assert(index == count);

	/* Sort the active clock list by pointer as part of contract with
	 * caller to make it easier for the caller to analyse changes. */
	qsort(snapshot, count, sizeof *snapshot, ptr_compar);

finish:
	clock_unlock();
	if (num_clocks)
		*num_clocks = count;
	return snapshot;
}

void sfptpd_clock_free_active_snapshot(struct sfptpd_clock **snapshot)
{
	free(snapshot);
}

struct sfptpd_clock *sfptpd_clock_first_active(void)
{
	struct sfptpd_clock *clock;

	clock_lock();
	for (clock = sfptpd_clock_list_head;
	     clock != NULL;
	     clock = clock->next) {
		assert(clock->magic == SFPTPD_CLOCK_MAGIC);
		if (!clock->deleted) {
			break;
		}
	}
	clock_unlock();

	return clock;
}

struct sfptpd_clock *sfptpd_clock_next_active(struct sfptpd_clock *clock)
{
	clock_lock();

	assert(clock != NULL);
	clock = clock->next;

	for (; clock != NULL;
	     clock = clock->next) {
		assert(clock->magic == SFPTPD_CLOCK_MAGIC);
		if (!clock->deleted) {
			break;
		}
	}
	clock_unlock();

	return clock;
}

struct sfptpd_clock *sfptpd_clock_find_by_name(const char *name)
{
	struct sfptpd_clock *clock = NULL;
	assert(name != NULL);

	clock_lock();
	for (clock = sfptpd_clock_list_head; clock != NULL; clock = clock->next) {
		assert(clock->magic == SFPTPD_CLOCK_MAGIC);
		if ((strcmp(clock->short_name, name) == 0) ||
		    (strcmp(clock->long_name, name) == 0) ||
		    (strcmp(clock->hw_id_string, name) == 0))
			break;
	}
	clock_unlock();

	return clock;
}


struct sfptpd_clock *sfptpd_clock_find_by_hw_id(sfptpd_clock_id_t *hw_id)
{
	struct sfptpd_clock *clock = NULL;

	clock_lock();
	for (clock = sfptpd_clock_list_head; clock != NULL; clock = clock->next) {
		assert(clock->magic == SFPTPD_CLOCK_MAGIC);
		if (memcmp(&clock->hw_id, hw_id, sizeof(clock->hw_id) == 0)) {
			break;
		}
	}
	clock_unlock();

	return clock;
}


/* Not locked because to do so would be ineffective and unnecessary.
   The caller's operation is likely to be repeated shortly either because
   it is periodic or because it will be triggered by any current change.
*/
struct sfptpd_clock *sfptpd_clock_get_system_clock(void)
{
	return sfptpd_clock_system;
}

bool sfptpd_clock_get_been_locked(const struct sfptpd_clock *clock)
{
        return clock->lrc_been_locked;
}

void sfptpd_clock_set_been_locked(struct sfptpd_clock *clock, bool value)
{
        clock->lrc_been_locked = value;
}

/* Not locked because we do not expect clock ids to be changed in place */
bool sfptpd_clock_ids_equal(sfptpd_clock_id_t *id1,
			    sfptpd_clock_id_t *id2)
{
	assert(id1 != NULL);
	assert(id2 != NULL);
	return (memcmp(id1->id, id2->id, sizeof(id1->id)) == 0);
}


/* Not locked because shadow hardware state is not accessed. */
const char *sfptpd_clock_class_text(enum sfptpd_clock_class clock_class)
{
	const char *text_map[SFPTPD_CLOCK_CLASS_MAX] =
	{
		[SFPTPD_CLOCK_CLASS_LOCKED] = "locked",
		[SFPTPD_CLOCK_CLASS_HOLDOVER] = "holdover",
		[SFPTPD_CLOCK_CLASS_FREERUNNING] = "freerunning",
		[SFPTPD_CLOCK_CLASS_UNKNOWN] = "unknown"
	};

	assert(clock_class < SFPTPD_CLOCK_CLASS_MAX);
	return text_map[clock_class];
}


/* Not locked because shadow hardware state is not accessed. */
const char *sfptpd_clock_time_source_text(enum sfptpd_time_source time_source)
{
	const char *text = NULL;

	switch (time_source) {
	case SFPTPD_TIME_SOURCE_ATOMIC_CLOCK: text = "atomic clock"; break;
	case SFPTPD_TIME_SOURCE_GPS: text = "gps"; break;
	case SFPTPD_TIME_SOURCE_TERRESTRIAL_RADIO: text = "terrestrial radio"; break;
	case SFPTPD_TIME_SOURCE_PTP: text = "ptp"; break;
	case SFPTPD_TIME_SOURCE_NTP: text = "ntp"; break;
	case SFPTPD_TIME_SOURCE_HANDSET: text = "handset"; break;
	case SFPTPD_TIME_SOURCE_OTHER: text = "other"; break;
	case SFPTPD_TIME_SOURCE_INTERNAL_OSCILLATOR: text = "internal oscillator"; break;
	}

	return text;
}


/****************************************************************************/

int sfptpd_clock_load_freq_correction(struct sfptpd_clock *clock,
				      long double *freq_correction_ppb)
{
	int rc = 0;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	assert(clock->spec != NULL);
	assert(freq_correction_ppb != NULL);

	/* If configured to use save clock corrections, try to read the
	 * frequency correction from file. Otherwise, use the value 0 */
	if (clock->use_clock_correction) {
		rc = sfptpd_log_read_freq_correction(clock, &clock->freq_correction_ppb);
		if (rc != 0) {
			/* No saved frequency correction is available */
			clock->freq_correction_ppb = 0.0;
			rc = ENODATA;
			TRACE_L1("clock %s: no saved freq adj available\n",
				 clock->short_name);
		} else {
			TRACE_L1("clock %s: restored freq adj %0.3Lf from file\n",
				 clock->short_name, clock->freq_correction_ppb);
		}
	} else {
		clock->freq_correction_ppb = 0.0;
		TRACE_L4("clock %s: persistent clock correction disabled\n",
			 clock->short_name);
	}

	if ((clock->freq_correction_ppb > clock->max_freq_adj_ppb) ||
	    (clock->freq_correction_ppb < -clock->max_freq_adj_ppb)) {
		/* Check that the frequency adjustment is within the accuracy
		 * of this clock. If not, then don't use the data */
		WARNING("clock %s: saved frequency correction %0.3Lf is outside valid range [%0.3Lf,%0.3Lf]\n",
			clock->long_name, clock->freq_correction_ppb,
			-clock->max_freq_adj_ppb, clock->max_freq_adj_ppb);
		clock->freq_correction_ppb = 0.0;
		/* Delete the offending file! */
		sfptpd_log_delete_freq_correction(clock);
		rc = ENODATA;
	}

	/* Set the frequency adjustment according to the restored value */
	(void)sfptpd_clock_adjust_frequency(clock, clock->freq_correction_ppb);

	*freq_correction_ppb = clock->freq_correction_ppb;
	clock_unlock();
	return rc;
}


int sfptpd_clock_save_freq_correction(struct sfptpd_clock *clock, long double freq_correction_ppb)
{
	int rc = 0;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	assert(clock->spec != NULL);

	/* If the current frequency adjustment is outside the valid range
	 * of this clock, don't save it. */
	if ((freq_correction_ppb > clock->max_freq_adj_ppb) ||
	    (freq_correction_ppb < -clock->max_freq_adj_ppb)) {
		TRACE_L2("clock %s: freq adj %0.3Lf is outside valid range [%0.3Lf,%0.3Lf] - not saving\n",
			 clock->short_name, freq_correction_ppb,
			 -clock->max_freq_adj_ppb, clock->max_freq_adj_ppb);
		rc = ERANGE;
		goto finish;
	}

	/* Write the file and update our local copy */
	rc = sfptpd_log_write_freq_correction(clock, freq_correction_ppb);
	clock->freq_correction_ppb = freq_correction_ppb;

	TRACE_L1("clock %s: %s freq adj %0.3Lf to file\n",
		 clock->short_name,
		 rc == 0 ? "saved" : "could not save",
		 freq_correction_ppb);
 finish:
	clock_unlock();
	return rc;
}


/* Not locked because it is an atomic operation. */
long double sfptpd_clock_get_freq_correction(struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	return clock->freq_correction_ppb;
}


/* Not locked because it is an atomic operation. */
bool sfptpd_clock_get_discipline(struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	return clock->discipline;
}


/* Not locked because it only accesses unchanging shadow configuration */
bool sfptpd_clock_is_writable(struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	return clock->discipline && !clock->read_only && (clock->blocked_count < 1);
}


/* Adjust ref count for temporary block */
bool sfptpd_clock_set_blocked(struct sfptpd_clock *clock, bool block)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	return (clock->blocked_count += block ? 1 : -1) > 0;
}

bool sfptpd_clock_is_blocked(const struct sfptpd_clock *clock)
{
	return clock->blocked_count > 0;
}


/****************************************************************************/

void sfptpd_clock_stats_record_offset(struct sfptpd_clock *clock,
				      long double offset, bool synchronized)
{
	struct sfptpd_stats_collection *stats;
	struct sfptpd_timespec now;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	stats = &clock->stats;
	sfclock_gettime(CLOCK_REALTIME, &now);
	sfptpd_stats_collection_update_range(stats, CLOCK_STATS_ID_OFFSET,
					     offset, now, true);
	sfptpd_stats_collection_update_count(stats, CLOCK_STATS_ID_SYNCHRONIZED,
					     synchronized? 1: 0);
	clock_unlock();
}


void sfptpd_clock_stats_record_epoch_alarm(struct sfptpd_clock *clock,
					   bool near_epoch)
{
	struct sfptpd_stats_collection *stats;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	stats = &clock->stats;
	sfptpd_stats_collection_update_count(stats, CLOCK_STATS_ID_NEAR_EPOCH,
					     near_epoch? 1: 0);
	clock_unlock();
}


void sfptpd_clock_stats_record_clustering_alarm(struct sfptpd_clock *clock,
					   bool out_of_threshold)
{
	struct sfptpd_stats_collection *stats;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	stats = &clock->stats;
	sfptpd_stats_collection_update_count(stats, CLOCK_STATS_ID_CLUSTERING,
					     out_of_threshold? 1: 0);
	clock_unlock();
}


void sfptpd_clock_stats_end_period(struct sfptpd_clock *clock,
				   struct sfptpd_timespec *time)
{
	struct sfptpd_stats_collection *stats;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	assert(time != NULL);

	stats = &clock->stats;
	sfptpd_stats_collection_end_period(stats, time);

	/* Write the historical statistics to file */
	sfptpd_stats_collection_dump(stats, clock, NULL);

	clock_unlock();
}


/****************************************************************************/

/* Not locked because it is an atomic operation. */
const char *sfptpd_clock_get_short_name(const struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	return clock->short_name;
}


/* Not locked because it is an atomic operation. */
const char *sfptpd_clock_get_long_name(const struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	return clock->long_name;
}


/* Not locked because it is an atomic operation. */
const char *sfptpd_clock_get_hw_id_string(const struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	return clock->hw_id_string;
}

int sfptpd_clock_get_hw_id(const struct sfptpd_clock *clock,
			   sfptpd_clock_id_t *hw_id)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	assert(hw_id != NULL);

	*hw_id = clock->hw_id;

	if (!clock->deleted &&
	    memcmp(&clock->hw_id, &SFPTPD_CLOCK_ID_UNINITIALISED, sizeof clock->hw_id))
		return 0;
	else
		return ENODATA;
}

/** Get the clock ID associated with this clock
 * @param clock Pointer to clock instance
 * @return 0 or errno
 */
int sfptpd_clock_get_hw_id(const struct sfptpd_clock *clock,
			   sfptpd_clock_id_t *hw_id);


/* Not locked because it is an atomic operation. */
const char *sfptpd_clock_get_fname_string(const struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	return clock->fname_string;
}


/* Not locked because it is an atomic operation. */
void sfptpd_clock_init_hw_id_string(char *buf, const sfptpd_clock_id_t hw_id,
				    int max_len)
{
	assert(buf != NULL);
	snprintf(buf, max_len, SFPTPD_FORMAT_EUI64,
		 hw_id.id[0], hw_id.id[1], hw_id.id[2], hw_id.id[3],
		 hw_id.id[4], hw_id.id[5], hw_id.id[6], hw_id.id[7]);
}


/* Not locked because system clocks cannot become NIC clocks and the rest
   is an atomic operation. */
struct sfptpd_interface *sfptpd_clock_get_primary_interface(const struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	if (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM)
		return NULL;
	assert(clock->u.nic.primary_if != NULL);
	return clock->u.nic.primary_if;
}


/* Not locked. TODO: re-evaluate whether this is safe. */
void sfptpd_clock_get_accuracy(struct sfptpd_clock *clock,
			       enum sfptpd_clock_stratum *stratum,
			       long double *accuracy,
			       long double *holdover)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	assert(clock->spec != NULL);
	assert(stratum != NULL);
	assert(accuracy != NULL);
	assert(holdover != NULL);

	clock_lock();
	*stratum = clock->spec->stratum;
	*accuracy = clock->spec->accuracy;
	*holdover = clock->spec->holdover;
	clock_unlock();
}


/* Not locked because it is an atomic operation. */
long double sfptpd_clock_get_max_frequency_adjustment(struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	return clock->max_freq_adj_ppb;
}


/****************************************************************************/

int sfptpd_clock_adjust_time(struct sfptpd_clock *clock, struct sfptpd_timespec *offset)
{
	int rc = 0;
	struct timex t;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	assert(offset != NULL);

	if (clock->type != SFPTPD_CLOCK_TYPE_SYSTEM &&
	    clock->u.nic.phc == NULL) {
		ERROR("clock %s: unable to step clock - no phc device\n",
		      clock->long_name);
		rc = ENODEV;
		goto finish;
	}

	if (clock->read_only) {
		NOTICE("clock %s: adjust time blocked by \"clock-control no-adjust\" or \"clock-readonly\"\n",
		       clock->long_name);
		goto finish;
	}

	if (clock->blocked_count > 0) {
		NOTICE("clock %s: adjust time temporarily blocked\n",
		       clock->long_name);
		goto finish;
	}

	INFO("clock %s: applying offset %0.9Lf seconds\n",
	     clock->short_name, sfptpd_time_timespec_to_float_s(offset));

	memset(&t, 0, sizeof(t));
	t.modes = ADJ_SETOFFSET | ADJ_NANO;
	t.time.tv_sec  = offset->sec;
	t.time.tv_usec = offset->nsec;

	if (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM &&
	    clock->cfg_rtc_adjust) {
		t.modes |= ADJ_STATUS;
		t.status = clock->u.system.kernel_status;
	}

	rc = clock_adjtime(clock->posix_id, &t);
	if (rc < 0) {
		WARNING("clock %s: failed to step clock using clock_adjtime(), %s\n",
			clock->long_name, strerror(errno));
		rc = errno;
		goto finish;
	}

	/* Record step for all PHC clocks to avoid stale comparisons */
	clock_record_step();

	/* clock_adjtime() returns a non-negative value on success */
	rc = 0;
finish:
	clock_unlock();
	return rc;
}


int sfptpd_clock_adjust_frequency(struct sfptpd_clock *clock, long double freq_adj_ppb)
{
	int rc = 0;
	struct sfptpd_timespec now;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	if (clock->type != SFPTPD_CLOCK_TYPE_SYSTEM &&
	    clock->u.nic.phc == NULL) {
		ERROR("clock %s: unable to adjust frequency - no phc device\n",
		      clock->long_name);
		rc = ENODEV;
		goto finish;
	}

	if (clock->read_only) {
		TRACE_L4("clock %s: adjust freq blocked by \"clock-control no-adjust\" or \"clock-readonly\"\n",
			 clock->long_name);
		goto finish;
	}

	if (clock->blocked_count > 0) {
		TRACE_L4("clock %s: adjust freq temporarily blocked\n",
			 clock->long_name);
		goto finish;
	}

	TRACE_L4("clock %s: applying freq adjustment %0.3Lf ppb\n",
		 clock->short_name, freq_adj_ppb);

	/* Record the new frequency */
	sfclock_gettime(CLOCK_REALTIME, &now);
	sfptpd_stats_collection_update_range(&clock->stats,
					     CLOCK_STATS_ID_FREQ_ADJ,
					     freq_adj_ppb,
					     now, true);

	/* Saturate the frequency adjustment */
	if (freq_adj_ppb > clock->max_freq_adj_ppb)
		freq_adj_ppb = clock->max_freq_adj_ppb;
	if (freq_adj_ppb < -clock->max_freq_adj_ppb)
		freq_adj_ppb = -clock->max_freq_adj_ppb;

	struct timex t;
	long double freq = freq_adj_ppb;

	memset(&t, 0, sizeof(t));

	if (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM) {
		long double tick;
		struct sfptpd_clock_system *system = &clock->u.system;

		/* If the frequency adjustment is large, this is achieved by
		* adjusting the kernel tick length (t.tick) and placing the
		* remainder in the frequency adjustment (t.freq).
		*/
		tick = 0.0;
		if (freq > system->max_freq_adj) {
			tick = roundl((freq_adj_ppb - system->max_freq_adj) / system->tick_resolution_ppb);
			if (tick > system->max_tick)
				tick = system->max_tick;
			freq -= tick * system->tick_resolution_ppb;
		} else if (freq < -system->max_freq_adj) {
			tick= -roundl((-freq_adj_ppb - system->max_freq_adj) / system->tick_resolution_ppb);
			if (tick < system->min_tick)
				tick = system->min_tick;
			freq -= tick * system->tick_resolution_ppb;
		}

		/* Saturate the frequency adjustment */
		if (freq > system->max_freq_adj)
			freq = system->max_freq_adj;
		else if (freq < -system->max_freq_adj)
			freq = -system->max_freq_adj;

		t.modes |= ADJ_TICK;
		t.tick = (long)roundl(tick + (1000000.0 / system->tick_freq_hz));

		if (clock->cfg_rtc_adjust) {
			t.modes |= ADJ_STATUS;
			t.status = clock->u.system.kernel_status;
		}
	}

	/* The 'freq' field in the 'struct timex' is in parts per
	* million, but with a 16 bit binary fractional field. */
	t.modes |= ADJ_FREQUENCY;
	t.freq = (long)roundl(freq * (((1 << 16) + 0.0) / 1000.0));

	rc = clock_adjtime(clock->posix_id, &t);
	if (rc < 0) {
		WARNING("clock %s: failed to adjust frequency using clock_adjtime(), %s\n",
			clock->long_name, strerror(errno));
		rc = errno;
		goto finish;
	}

	/* clock_adjtime() returns a non-negative value on success */
	rc = 0;
 finish:
	clock_unlock();
	return rc;
}


int sfptpd_clock_schedule_leap_second(enum sfptpd_leap_second_type type)
{
	struct sfptpd_clock *clock;
	struct timex t;
	int rc = 0;

	clock_lock();
	assert(type < SFPTPD_LEAP_SECOND_MAX);

	/* Only the system clock supports leap second scheduling */
	clock = sfptpd_clock_system;

	if (clock->read_only) {
		TRACE_L3("clock %s: schedule leap second blocked by \"clock-control no-adjust\"\n",
			 clock->short_name);
		goto finish;
	}

	TRACE_L4("clock %s: scheduling %s leap second for midnight today UTC\n",
		 clock->short_name,
		 (type == SFPTPD_LEAP_SECOND_NONE)? "no":
			(type == SFPTPD_LEAP_SECOND_59)? "59": "61");

	/* Adjust master copy of status flags */
	clock->u.system.kernel_status &= ~(STA_DEL | STA_INS);
	switch (type) {
	case SFPTPD_LEAP_SECOND_NONE:
		/* Cancelled leap second */
		break;
	case SFPTPD_LEAP_SECOND_59:
		clock->u.system.kernel_status |= STA_DEL;
		break;
	case SFPTPD_LEAP_SECOND_61:
		clock->u.system.kernel_status |= STA_INS;
		break;
	default:
		assert(!"missing case");
	}

	/* Write the adjtimex flags */
	t.modes = ADJ_STATUS;
	t.status = clock->u.system.kernel_status;
	rc = adjtimex(&t);
	if (rc < 0) {
		ERROR("couldn't set/clear adjtimex status, %s\n", strerror(errno));
		rc = errno;
		goto finish;
	}

 finish:
	clock_unlock();
	return rc;
}


int sfptpd_clock_leap_second_now(enum sfptpd_leap_second_type type)
{
	struct sfptpd_clock *clock;
	struct sfptpd_timespec step;

	struct sfptpd_config_general *config = sfptpd_general_config_get(sfptpd_clock_config);

	if ((type != SFPTPD_LEAP_SECOND_59) && (type != SFPTPD_LEAP_SECOND_61)) {
		ERROR("invalid leap second type %d\n", type);
		return EINVAL;
	}

	sfptpd_time_from_s(&step, (type == SFPTPD_LEAP_SECOND_59)? 1: -1);

	/* No action is required for the system clock as it supports leap
	 * second scheduling rather than manually stepping the clock at
	 * midnight */
	clock_lock();
	for (clock = sfptpd_clock_list_head; clock != NULL; clock = clock->next) {
		if ((clock->type == SFPTPD_CLOCK_TYPE_SFC) ||
		    (clock->type == SFPTPD_CLOCK_TYPE_XNET) ||
		    (clock->type == SFPTPD_CLOCK_TYPE_NON_SFC && config->non_sfc_nics)) {
			sfptpd_clock_adjust_time(clock, &step);
		}
	}
	clock_unlock();

	return 0;
}


int sfptpd_clock_get_time(const struct sfptpd_clock *clock, struct sfptpd_timespec *time)
{
	int rc = 0;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	assert(time != NULL);

	if (clock->type != SFPTPD_CLOCK_TYPE_SYSTEM &&
	    clock->u.nic.phc == NULL) {
		ERROR("clock %s: unable to get time - no phc device\n",
		      clock->long_name);
		rc = ENODEV;
		goto finish;
	}

	if (sfclock_gettime(clock->posix_id, time) < 0) {
		ERROR("clock %s: error getting system time, %s\n",
		      clock->long_name, strerror(errno));
		rc = errno;
		goto finish;
	}
 finish:
	clock_unlock();
	return rc;
}


int sfptpd_clock_compare(struct sfptpd_clock *clock1, struct sfptpd_clock *clock2,
			 struct sfptpd_timespec *diff)
{
	struct sfptpd_timespec diff2;
	int rc = 0;

	clock_lock();

	assert(clock1 != NULL);
	assert(clock1->magic == SFPTPD_CLOCK_MAGIC);
	assert(clock2 != NULL);
	assert(clock2->magic == SFPTPD_CLOCK_MAGIC);
	assert(diff != NULL);

	/* Make sure the difference is zero */
	sfptpd_time_zero(diff);

	/* If either clock has been deleted we can't proceed */
	if (clock1->deleted || clock2->deleted) {
		/* Bypass any later error-checking as
		   this is a clean no-op condition */
		clock_unlock();
		return ENOENT;
	}

	/* We expect all clocks other than the system clock to have a PHC
	 * device */
	if (((clock1->type != SFPTPD_CLOCK_TYPE_SYSTEM) && (clock1->u.nic.phc == NULL)) |
	    ((clock2->type != SFPTPD_CLOCK_TYPE_SYSTEM) && (clock2->u.nic.phc == NULL))) {
		clock_unlock();
		return ENOSYS;
	}

	if (clock1->type != SFPTPD_CLOCK_TYPE_SYSTEM) {
		rc = sfptpd_phc_compare_to_sys_clk(clock1->u.nic.phc, diff);

		if (rc != 0) {
			sfptpd_stats_collection_update_count(&clock1->stats,
							     CLOCK_STATS_ID_SYNC_FAIL, 1);

			/* If this is second comparison failure in a short
			 * time issue a warning, otherwise just trace */
			if (clock1->good_compare_count < CLOCK_BAD_COMPARE_WARN_THRESHOLD)
				WARNING("failed to compare clock %s and system clock, error %s\n",
					clock1->long_name, strerror(rc));
			else
				TRACE_L4("failed to compare clock %s and system clock, error %s\n",
					 clock1->long_name, strerror(rc));
			clock1->good_compare_count = 0;
			goto finish;
		}

		/* Count the number of sequential good compares */
		clock1->good_compare_count++;

		sfptpd_stats_collection_update_count(&clock1->stats,
						     CLOCK_STATS_ID_SYNC_FAIL, 0);
	}

	if (clock2->type != SFPTPD_CLOCK_TYPE_SYSTEM) {
		rc = sfptpd_phc_compare_to_sys_clk(clock2->u.nic.phc, &diff2);

		if (rc != 0) {
			/* We always update the stats of clock1 even if this
			 * intermediate comparison does not involve clock1
			 * because clock 1 is the consumer of the comparison
			 * i.e. the slave clock */
			sfptpd_stats_collection_update_count(&clock1->stats,
							     CLOCK_STATS_ID_SYNC_FAIL, 1);

			/* If this is second comparison failure in a short
			 * time issue a warning, otherwise just trace */
			if (clock2->good_compare_count < CLOCK_BAD_COMPARE_WARN_THRESHOLD)
				WARNING("failed to compare clock %s and system clock, error %s\n",
					clock2->long_name, strerror(rc));
			else
				TRACE_L4("failed to compare clock %s and system clock, error %s\n",
					 clock2->long_name, strerror(rc));
			clock2->good_compare_count = 0;
			goto finish;
		}

		/* Count the number of sequential good compares */
		clock2->good_compare_count++;

		/* Store the difference. There are two cases:
		 *   clock1 = sys, clock2 = ptp   =>   -(Tptp - Tsys)
		 *   clock1 = ptp, clock2 = ptp   =>   (Tptp1 - Tsys - (Tptp2 - Tsys)
		 * which equate to the same subtraction operation. */
		sfptpd_time_subtract(diff, diff, &diff2);

		sfptpd_stats_collection_update_count(&clock2->stats,
						     CLOCK_STATS_ID_SYNC_FAIL, 0);
	}

 finish:
	clock_unlock();

	if (rc == EOPNOTSUPP) {
		/* This should never happen in any serious configuration (there's
		   no excuse not to include the 'read-time' method for fallback)
		   and we can't be sure this will be found in startup call sequence
		   so we'd better force an exit. */
		rc = sfptpd_thread_error(rc);
	}

	return rc;
}


int sfptpd_clock_set_time(struct sfptpd_clock *clock_to,
			  struct sfptpd_clock *clock_from,
			  const struct sfptpd_timespec *threshold,
			  bool is_initial_correction)
{
	struct sfptpd_timespec diff;
	int rc;

	if (clock_to == clock_from ||
	    (is_initial_correction &&
	     clock_to->initial_correction_applied))
		return 0;

	clock_lock();

	assert(clock_to != NULL);
	assert(clock_from != NULL);

	/* Rather than set the NIC time, get an
	 * accurate difference between the two clocks
	 * and apply this offset */

	rc = sfptpd_clock_compare(clock_from, clock_to, &diff);

	if (rc == 0 &&
	    (threshold == NULL ||
	     sfptpd_time_cmp(&diff, threshold) >= 0)) {
		rc = sfptpd_clock_adjust_time(clock_to, &diff);
		if (rc == 0 && is_initial_correction)
			clock_to->initial_correction_applied = true;
	}

	clock_unlock();
	return rc;
}


int sfptpd_clock_set_sync_status(struct sfptpd_clock *clock, bool in_sync,
				 unsigned int timeout)
{
	struct efx_sock_ioctl sfc_req;
	int rc = 0;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	if (clock->read_only) {
		TRACE_L4("clock %s: set sync status blocked by \"clock-control no-adjust\" or \"clock-readonly\"\n",
			 clock->long_name);
		goto finish;
	}

	if (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM &&
	    clock->cfg_rtc_adjust) {
		if (in_sync)
			clock->u.system.kernel_status &= ~STA_UNSYNC;
		else
			clock->u.system.kernel_status |= STA_UNSYNC;
		goto finish;
	}

	if (clock->type != SFPTPD_CLOCK_TYPE_SFC && clock->type != SFPTPD_CLOCK_TYPE_XNET) {
		rc = ENOSYS;
		goto finish;
	}

	if (!clock->u.nic.supports_sync_status_reporting) {
		rc = EOPNOTSUPP;
		goto finish;
	}

	/* Update the sync status via a private IOCTL unless inhibited. */
	memset(&sfc_req, 0, sizeof(sfc_req));
	sfc_req.cmd = EFX_TS_SET_SYNC_STATUS;
	sfc_req.u.ts_set_sync_status.in_sync = in_sync? 1: 0;
	sfc_req.u.ts_set_sync_status.timeout = timeout;

	rc = sfptpd_interface_ioctl(clock->u.nic.primary_if, SIOCEFX, &sfc_req);
	if (rc == 0) {
		TRACE_L6("clock %s: set sync status to %d\n", 
		         clock->long_name, in_sync);
		goto finish;
	}

	/* If the request failed because the clock doesn't support sync status
	 * reporting then record this and don't try to make the call again */
	if (rc == EOPNOTSUPP) {
		clock->u.nic.supports_sync_status_reporting = false;
		TRACE_L3("clock %s: set sync status not supported\n",
			 clock->long_name);
	} else {
		WARNING("clock %s: failed to set sync status: %s\n",
			clock->long_name, strerror(rc));
	}
 finish:
	clock_unlock();
	return rc;
}


/****************************************************************************/

int sfptpd_clock_pps_enable(struct sfptpd_clock *clock)
{
	struct efx_sock_ioctl sfc_req;
	int rc = 0;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	if (clock->u.nic.phc != NULL &&
	    ((clock->type == SFPTPD_CLOCK_TYPE_NON_SFC && clock->cfg_non_sfc_nics) ||
	     (clock->type == SFPTPD_CLOCK_TYPE_XNET) ||
	     (clock->type == SFPTPD_CLOCK_TYPE_SFC &&
	      (!clock->u.nic.supports_efx || clock->cfg_avoid_efx)))) {
		rc = sfptpd_phc_enable_pps(clock->u.nic.phc, true);
		goto finish;
	}

	if (clock->type != SFPTPD_CLOCK_TYPE_SFC) {
		rc = ENOSYS;
		goto finish;
	}

	/* Enable the PPS input via a private IOCTL */
	memset(&sfc_req, 0, sizeof(sfc_req));
	sfc_req.cmd = EFX_TS_ENABLE_HW_PPS;
	sfc_req.u.pps_enable.enable = 1;

	rc = sfptpd_interface_ioctl(clock->u.nic.primary_if, SIOCEFX, &sfc_req);
	if (rc == 0) {
		INFO("clock %s: SFC PPS input enabled\n",
		     clock->long_name);
	} else {
		ERROR("clock %s: failed to enable PPS input: %s\n",
		      clock->long_name, strerror(rc));
	}

 finish:
	clock_unlock();
	return rc;
}


int sfptpd_clock_pps_disable(struct sfptpd_clock *clock)
{
	struct efx_sock_ioctl sfc_req;
	int rc = 0;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	if (clock->u.nic.phc != NULL &&
	    ((clock->type == SFPTPD_CLOCK_TYPE_NON_SFC && clock->cfg_non_sfc_nics) ||
	     (clock->type == SFPTPD_CLOCK_TYPE_XNET) ||
	     (clock->type == SFPTPD_CLOCK_TYPE_SFC &&
	      (!clock->u.nic.supports_efx || clock->cfg_avoid_efx)))) {
		rc = sfptpd_phc_enable_pps(clock->u.nic.phc, false);
		goto finish;
	}

	if (clock->type != SFPTPD_CLOCK_TYPE_SFC) {
		rc = ENOSYS;
		goto finish;
	}
	
	/* Disable the PPS input via a private IOCTL */
	memset(&sfc_req, 0, sizeof(sfc_req));
	sfc_req.cmd = EFX_TS_ENABLE_HW_PPS;
	sfc_req.u.pps_enable.enable = 0;
	(void)sfptpd_interface_ioctl(clock->u.nic.primary_if, SIOCEFX, &sfc_req);

 finish:
	clock_unlock();
	return rc;
}


void sfptpd_clock_correct_new(struct sfptpd_clock *clock)
{
	long double not_used;
	struct sfptpd_timespec time;
	int rc;

	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	(void)sfptpd_clock_load_freq_correction(clock, &not_used);

	/* Don't attempt to correct the system clock */
	if ((clock->type == SFPTPD_CLOCK_TYPE_SFC) ||
	    (clock->type == SFPTPD_CLOCK_TYPE_XNET) ||
	    ((clock->type == SFPTPD_CLOCK_TYPE_NON_SFC) && clock->cfg_non_sfc_nics)) {
		/* Read the NIC clock time. If it is near the epoch
		 * then we conclude that it has never been set. Set
		* it to the current system time */
		rc = sfptpd_clock_get_time(clock, &time);
		if (rc != 0) {
			ERROR("failed to read clock %s time, %s\n",
			      clock->long_name, strerror(rc));
		} else {
		        struct sfptpd_config_general *gconf;
			gconf = sfptpd_general_config_get(sfptpd_clock_config);

			if (time.sec < SFPTPD_NIC_TIME_VALID_THRESHOLD ||
			    gconf->initial_clock_correction == SFPTPD_CLOCK_INITIAL_CORRECTION_ALWAYS) {
				sfptpd_clock_set_time(clock, sfptpd_clock_system, NULL, true);
			}
		}
	}
}


int sfptpd_clock_pps_get_fd(struct sfptpd_clock *clock)
{

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	if (clock->u.nic.phc != NULL &&
	    ((clock->type == SFPTPD_CLOCK_TYPE_NON_SFC && clock->cfg_non_sfc_nics) ||
	     (clock->type == SFPTPD_CLOCK_TYPE_XNET) ||
	     (clock->type == SFPTPD_CLOCK_TYPE_SFC &&
	      (!clock->u.nic.supports_efx || clock->cfg_avoid_efx)))) {
		return sfptpd_phc_get_pps_fd(clock->u.nic.phc);
	} else {
		return -1;
	}
}


int sfptpd_clock_pps_get(struct sfptpd_clock *clock, uint32_t *sequence_num,
			 struct sfptpd_timespec *time)
{
	struct efx_sock_ioctl sfc_req;
	int rc = 0;

	clock_lock();

	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);
	assert(sequence_num != NULL);
	assert(time != NULL);

	if (clock->u.nic.phc != NULL &&
	    ((clock->type == SFPTPD_CLOCK_TYPE_NON_SFC && clock->cfg_non_sfc_nics) ||
	     (clock->type == SFPTPD_CLOCK_TYPE_XNET) ||
	     (clock->type == SFPTPD_CLOCK_TYPE_SFC &&
	      (!clock->u.nic.supports_efx || clock->cfg_avoid_efx)))) {
		rc = sfptpd_phc_get_pps_event(clock->u.nic.phc, time, sequence_num);
		goto finish;
	}

	if (clock->type != SFPTPD_CLOCK_TYPE_SFC) {
		rc = ENOSYS;
		goto finish;
	}

	/* Read a PPS event via a private IOCTL */
	memset(&sfc_req, 0, sizeof(sfc_req));
	sfc_req.cmd = EFX_TS_GET_PPS;
	sfc_req.u.pps_event.timeout = 0;

	rc = sfptpd_interface_ioctl(clock->u.nic.primary_if, SIOCEFX, &sfc_req);
	if (rc == 0) {
		*sequence_num = sfc_req.u.pps_event.sequence;
		sfptpd_time_init(time,
				 sfc_req.u.pps_event.nic_assert.tv_sec,
				 sfc_req.u.pps_event.nic_assert.tv_nsec, 0);

		TRACE_L5("clock %s: external timestamp at " SFPTPD_FMT_SFTIMESPEC "\n",
			 clock->short_name, SFPTPD_ARGS_SFTIMESPEC(*time));
		goto finish;
	}

	if ((rc == ETIMEDOUT) || (rc == EINTR)) {
		rc = EAGAIN;
		goto finish;
	}

	ERROR("clock %s: failed to get PPS event: %s\n",
              clock->long_name, strerror(rc));
 finish:
	clock_unlock();
	return rc;
}


const char *sfptpd_clock_get_diff_method(struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	if (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM) {
		return "zero";
	} else if (clock->u.nic.phc != NULL) {
		return sfptpd_phc_get_diff_method_name(clock->u.nic.phc);
	} else {
		return "none";
	}
}


const char *sfptpd_clock_get_pps_method(struct sfptpd_clock *clock)
{
	assert(clock != NULL);
	assert(clock->magic == SFPTPD_CLOCK_MAGIC);

	if (clock->type == SFPTPD_CLOCK_TYPE_SYSTEM) {
		return "n/a";
	} else if (clock->u.nic.supports_efx &&
		   !clock->cfg_avoid_efx) {
		return "efx";
	} else if (clock->u.nic.phc != NULL) {
		return sfptpd_phc_get_pps_method_name(clock->u.nic.phc);
	} else {
		return "none";
	}
}

bool sfptpd_clock_is_system(const struct sfptpd_clock *clock)
{
	return is_system_clock(clock);
}

bool sfptpd_clock_is_active(const struct sfptpd_clock *clock)
{
	assert(!clock || clock->magic == SFPTPD_CLOCK_MAGIC);

	return clock && !clock->deleted;
}

/* fin */
