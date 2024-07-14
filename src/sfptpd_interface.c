/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2023 Xilinx, Inc. */

/**
 * @file   sfptpd_interface.c
 * @brief  Interface abstraction
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
#include <net/if_arp.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <math.h>
#include <assert.h>
#include <fts.h>
#include <sys/stat.h>
#include <linux/sockios.h>
#include <linux/socket.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>
#include <linux/pci.h>
#include <arpa/inet.h>
#include <regex.h>

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
#include "sfptpd_db.h"


/****************************************************************************
 * Types, Defines and Structures
 ****************************************************************************/

#define SFPTPD_INTERFACE_MAGIC (0xFACED0CE)

#define SFPTPD_SYSFS_NET_PATH "/sys/class/net/"
#define SFPTPD_PROC_VLAN_PATH "/proc/net/vlan/"
#define SFPTPD_SYSFS_VIRTUAL_NET_PATH "/sys/devices/virtual/net/"

#define VPD_TAG_RO (0x90)
#define VPD_TAG_STR (0x82)
#define VPD_TAG_END (0x78)
#define VPD_LARGE_TAG_MSK (0x80)
#define VPD_SMALL_TAG_LEN_MSK (0x07)
#define VPD_MAX_SIZE (PCI_VPD_ADDR_MASK + 1)


/** Structure to hold known NIC capabilities */
struct nic_model_caps {
	uint16_t vendor;
	uint16_t device;
	enum sfptpd_clock_stratum stratum;
};

/** Methods for retrieving driver stats */
enum drv_stat_method {
	DRV_STAT_NOT_AVAILABLE,
	DRV_STAT_ETHTOOL,
	DRV_STAT_SYSFS,
};

/** Structure describing a driver statistic type */
struct drv_stat_type {
	const char *sysfs_name;
	bool counter;
};

/** Structure to hold details of interfaces.
 *
 * The objects are created, update and deleted in this module
 * but are registered in a database table by reference. Some
 * interfaces may have been removed since creation; these are
 * marked with the 'deleted' flag.
 *
 * Resurrected interfaces will have the deleted flag cleared and
 * the if_index value updated. The nic_id is meant to be consistent
 * across interfaces for a given nic even when reinserted.
 *
 * If an interface is renamed, any deleted interface with that
 * name will be aliased to this one using the canonical field. If
 * references to the old object are maintained, then interface API
 * functions will chase this chain of aliases before performing
 * the requested function.
 *
 * Operations on deleted interface objects will yield friendly
 * null responses, e.g. "(no-interface)" in the case of a request
 * for the interface name.
 *
 * The suitable flag indicates if the interface would be available
 * for PTP. Note that as an interface is brought up, it's
 * capabilities can change so need to be re-evaluated in the object.
*/
struct sfptpd_interface {
	/* Magic number */
	uint32_t magic;

	/* A unique identifier assigned by the daemon for this NIC
	   that can't be changed when an adapter is later reinserted
	   unlike the clock device index */
	int nic_id;

	/* Interface index according to OS */
	int if_index;

	/* Interface name */
	char name[IF_NAMESIZE];

	/* Canonical interface of which this is an alias */
	struct sfptpd_interface *canonical;

	/* Adapter permanent MAC address */
	sfptpd_mac_addr_t mac_addr;
	char mac_string[SFPTPD_L2ADDR_STR_MAX];

	/* PCI device ID */
	uint16_t pci_device_id;
	uint16_t pci_vendor_id;
	char bus_addr[ETHTOOL_BUSINFO_LEN];

	/* Portion of bus address identifying the NIC */
	char bus_addr_nic[ETHTOOL_BUSINFO_LEN];

	/* Firmware and driver versions and other driver info */
	char driver_version[SFPTPD_VERSION_STRING_MAX];
	char fw_version[SFPTPD_VERSION_STRING_MAX];
	char driver[32];
	uint32_t n_stats;

	/* Indicates that the associated PTP clock supports the PHC API */
	bool clock_supports_phc;

	/* Indicates that the driver supports the EFX IOCTL */
	bool driver_supports_efx;

	/* Timestamping capabilities structure */
	struct ethtool_ts_info ts_info;

	/* Indicates if this interface is deleted */
	bool deleted;

	/* Indicates if this interface is suitable,
	   i.e. is a physical Ethernet interface */
	bool suitable;

	/* Indicates if timestamping is currently enabled for this interface */
	bool ts_enabled;
        
	/* Indicates whether if is in use for sfptpd */
	bool if_active;

	/* Pointer to PTP clock associated with the interface */
	struct sfptpd_clock *clock;

	/* Indicates the class of interface */
	sfptpd_interface_class_t class;

	/* Static capabilities of NIC model */
	struct nic_model_caps static_caps;

	/* Methods for driver statistic recovery */
	enum drv_stat_method drv_stat_method[SFPTPD_DRVSTAT_MAX];
	int drv_stat_ethtool_index[SFPTPD_DRVSTAT_MAX];

	/* Bitfield of methods needed for driver stats */
	int drv_stat_methods;

	/* Raw driver stats buffer */
	struct ethtool_stats *ethtool_stats;

	/* Zero adjustment for driver counters */
	int64_t stat_zero_adjustment[SFPTPD_DRVSTAT_MAX];

	/* A copy of the link table object, not necessarily current */
	struct sfptpd_link link;
};


/****************************************************************************
 * Constants
 ****************************************************************************/

const uint32_t rx_filters_min = (1 << HWTSTAMP_FILTER_ALL)
			      | (1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT)
			      | (1 << HWTSTAMP_FILTER_PTP_V2_EVENT);

static const struct ethtool_ts_info ts_info_sw_only =
{
	.so_timestamping = SOF_TIMESTAMPING_RX_SOFTWARE
			 | SOF_TIMESTAMPING_SOFTWARE,
	.phc_index = -1,
	.tx_types = (1 << HWTSTAMP_TX_OFF),
	.rx_filters = (1 << HWTSTAMP_FILTER_NONE)
};

static const uint32_t so_timestamping_raw = SOF_TIMESTAMPING_TX_HARDWARE
					  | SOF_TIMESTAMPING_RX_HARDWARE
					  | SOF_TIMESTAMPING_RAW_HARDWARE;

static const uint32_t so_timestamping_sw = SOF_TIMESTAMPING_TX_SOFTWARE
					 | SOF_TIMESTAMPING_RX_SOFTWARE
					 | SOF_TIMESTAMPING_SOFTWARE;

static const uint16_t xilinx_ptp_nics[] = {
	0x5084, /*!< X3522 */
};

static const struct nic_model_caps all_nic_models[] = {
	/* Err on the safe side with all SFN7xxx NIC clocks */
	{ SFPTPD_SOLARFLARE_PCI_VENDOR_ID , 0x0903, SFPTPD_NIC_XO_CLOCK_STRATUM },
};

static const struct drv_stat_type drv_stats[SFPTPD_DRVSTAT_MAX] = {
	[ SFPTPD_DRVSTAT_PPS_OFLOW ]    = { "pps_stats/pps_oflow", true },
	[ SFPTPD_DRVSTAT_PPS_BAD ]      = { "pps_stats/pps_bad", true },
	[ SFPTPD_DRVSTAT_PPS_OFF_LAST ] = { "pps_stats/pps_off_last", false },
	[ SFPTPD_DRVSTAT_PPS_OFF_MEAN ] = { "pps_stats/pps_off_mean", false },
	[ SFPTPD_DRVSTAT_PPS_OFF_MIN ]  = { "pps_stats/pps_off_min", false },
	[ SFPTPD_DRVSTAT_PPS_OFF_MAX ]  = { "pps_stats/pps_off_max", false },
	[ SFPTPD_DRVSTAT_PPS_PER_LAST ] = { "pps_stats/pps_per_last", false },
	[ SFPTPD_DRVSTAT_PPS_PER_MEAN ] = { "pps_stats/pps_per_mean", false },
	[ SFPTPD_DRVSTAT_PPS_PER_MIN ]  = { "pps_stats/pps_per_min", false },
	[ SFPTPD_DRVSTAT_PPS_PER_MAX ]  = { "pps_stats/pps_per_max", false },
};


/****************************************************************************
 * Searching and Sorting
 *
 * This section provides macros to help users perform searches of the
 * interface table.
 *
 * The FIND_ANY and FIND_FIRST macro look for entries in an index by any
 * number of keys. There can be more than one match when the search key does
 * not uniquely identify an interface, in which case the FIND_FIRST macro
 * returns the first one according to the supplied sort key whereas FIND_ANY
 * will return any matching element.
 *
 * The *_FN macros help construct the functions to enable the database module
 * to provide subsets of indexes in order as required.
 *
 * The searching and sorting capabilities are provided in the database module
 * as a convenience and to separate responsibilities; they are not efficient.
 * If needed, they could be optimised by the database module maintaininng
 * indexes.
 *
 ****************************************************************************/

/* Macro to find any exact match for the given key values */
#define FIND_ANY(...) interface_find_any(sfptpd_db_table_find(sfptpd_interface_table, \
							      __VA_ARGS__))

/* Macro to find the first match for the given key in the interface index of a given type */
#define FIND_FIRST(sort_key, ...) interface_find_first(sfptpd_db_table_query(sfptpd_interface_table, \
									     __VA_ARGS__, \
									     SFPTPD_DB_SEL_ORDER_BY, \
									     sort_key))

/* Macro to generate an interface comparison function suitable for qsort */
#define SORT_COMPAR_FN(name, intf, expr)				\
	static int compar_sort_ ## name(const void *raw_a, const void *raw_b) { \
		const struct sfptpd_interface *intf = *((struct sfptpd_interface **) raw_a); \
		return compar_search_ ## name(expr, raw_b);		\
	}

/* Macro to generate an interface comparison function suitable for bsearch */
#define SEARCH_COMPAR_FN(name, type, key, intf, expr)			\
	static int compar_search_ ## name(const void *raw_a, const void *raw_b) { \
		const type *key = (const type *) raw_a;			\
		const struct sfptpd_interface *intf = *((struct sfptpd_interface **) raw_b); \
		return expr;						\
	}

/* Macro to generate a print function for a key */
#define SNPRINT_FN(name, intf, fmt, ...)				\
	static int snprint_ ## name(char *buf, size_t size, int width, const void *raw_rec) { \
		const struct sfptpd_interface *intf = *((struct sfptpd_interface **) raw_rec); \
		return snprintf(buf, size, fmt, width, __VA_ARGS__); \
	}

/* Create search comparison functions */
SEARCH_COMPAR_FN(clock, int, key, rec, *key - rec->ts_info.phc_index)
SEARCH_COMPAR_FN(if_index, int, key, rec, *key - rec->if_index)
SEARCH_COMPAR_FN(name, char, key, rec, strcmp(key, rec->name))
SEARCH_COMPAR_FN(mac, sfptpd_mac_addr_t, key, rec, memcmp(key, &rec->mac_addr, sizeof rec->mac_addr))
SEARCH_COMPAR_FN(nic, int, key, rec, *key - rec->nic_id)
SEARCH_COMPAR_FN(deleted, int, key, rec, *key ? (rec->deleted ? 0 : 1) : (rec->deleted ? -1 : 0))
SEARCH_COMPAR_FN(ptp, int, key, rec, *key != -1 ? ((rec->nic_id == -1) ? -1 : 0) : ((rec->nic_id == -1) ? 0 : 1))
SEARCH_COMPAR_FN(type, int, key, rec, *key - rec->link.type)
SEARCH_COMPAR_FN(bus_addr_nic, char, key, rec, strcmp(key, rec->bus_addr_nic))

/* Create sort comparison functions */
SORT_COMPAR_FN(clock, rec, &rec->ts_info.phc_index)
SORT_COMPAR_FN(if_index, rec, &rec->if_index)
SORT_COMPAR_FN(name, rec, rec->name)
SORT_COMPAR_FN(mac, rec, &rec->mac_addr)
SORT_COMPAR_FN(nic, rec, &rec->nic_id)
SORT_COMPAR_FN(deleted, rec, &rec->deleted)
SORT_COMPAR_FN(ptp, rec, &rec->nic_id)
SORT_COMPAR_FN(type, rec, &rec->link.type)
SORT_COMPAR_FN(bus_addr_nic, rec, rec->bus_addr_nic)

/* Create print functions */
SNPRINT_FN(clock, rec, "%*d", rec->ts_info.phc_index)
SNPRINT_FN(if_index, rec, "%*d", rec->if_index)
SNPRINT_FN(name, rec, "%*s", rec->name)
SNPRINT_FN(mac, rec, "%*s", rec->mac_string)
SNPRINT_FN(nic, rec, "%*d", rec->nic_id)
SNPRINT_FN(deleted, rec, "%*s", rec->deleted ? "deleted" : "")
SNPRINT_FN(ptp, rec, "%*s", (rec->nic_id != -1) ? "ptp" : "")
SNPRINT_FN(type, rec, "%*s", sfptpd_link_type_str(rec->link.type))
SNPRINT_FN(bus_addr_nic, rec, "%*s", rec->bus_addr_nic)

#define ADD_KEY(enum, name) [enum] = { # name, compar_search_ ## name, compar_sort_ ## name, snprint_ ## name },


/****************************************************************************
 * Static data
 ****************************************************************************/

/* The interface table */
static struct sfptpd_db_table *sfptpd_interface_table;

enum interface_fields {
	INTF_KEY_IF_INDEX,
	INTF_KEY_NAME,
	INTF_KEY_MAC,
	INTF_KEY_CLOCK,
	INTF_KEY_NIC,
	INTF_KEY_DELETED,
	INTF_KEY_PTP,
	INTF_KEY_TYPE,
	INTF_KEY_BUS_ADDR_NIC,
	INTF_KEY_MAX
};

struct sfptpd_db_field interface_fields[] = {
	ADD_KEY(INTF_KEY_IF_INDEX, if_index)
	ADD_KEY(INTF_KEY_NAME, name)
	ADD_KEY(INTF_KEY_MAC, mac)
	ADD_KEY(INTF_KEY_CLOCK, clock)
	ADD_KEY(INTF_KEY_NIC, nic)
	ADD_KEY(INTF_KEY_DELETED, deleted)
	ADD_KEY(INTF_KEY_PTP, ptp)
	ADD_KEY(INTF_KEY_TYPE, type)
	ADD_KEY(INTF_KEY_BUS_ADDR_NIC, bus_addr_nic)
};

static struct sfptpd_config *sfptpd_interface_config;

/* A definition for the interface database table.
 * The intention of the database module is to copy entire
 * records into the table but in this case the record is
 * a pointer to the interface object because the object's
 * pointer is already used ubiquitously in the application. */
struct sfptpd_db_table_def interface_table_def = {
	.num_fields = INTF_KEY_MAX,
	.fields = interface_fields,
	.record_size = sizeof(struct sfptpd_interface *),
};

static int sfptpd_interface_socket = -1;

static int sfptpd_next_nic_id = 0;

/* Shared with the clocks module */
static pthread_mutex_t *sfptpd_interface_lock;

static struct utsname sysinfo = { .release = "uname-failed" };

/****************************************************************************
 * Forward declarations
 ****************************************************************************/


/****************************************************************************
 * Interface Operations
 ****************************************************************************/

static inline void interface_lock(void) {
	int rc = sfptpd_interface_lock == NULL ? 0 : pthread_mutex_lock(sfptpd_interface_lock);
	if (rc != 0) {
		CRITICAL("interface: could not acquire hardware state lock\n");
		exit(1);
	}
}


static inline void interface_unlock(void) {
	int rc = sfptpd_interface_lock == NULL ? 0 : pthread_mutex_unlock(sfptpd_interface_lock);
	if (rc != 0) {
		CRITICAL("interface: could not release hardware state lock\n");
		exit(1);
	}
}

static struct sfptpd_interface *interface_find_any(struct sfptpd_db_record_ref record_ref)
{
	if (sfptpd_db_record_exists(&record_ref)) {
		struct sfptpd_interface *interface;

		sfptpd_db_record_get_data(&record_ref, &interface, sizeof interface);
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
		return interface;
	} else {
		return NULL;
	}
}


static struct sfptpd_interface *interface_find_first(struct sfptpd_db_query_result query_result)
{
	struct sfptpd_interface *intf = NULL;

	if (query_result.num_records != 0) {
		intf = *((struct sfptpd_interface **) query_result.record_ptrs[0]);
		assert(intf->magic == SFPTPD_INTERFACE_MAGIC);
	}
	query_result.free(&query_result);

	return intf;
}

bool sfptpd_check_clock_interfaces(const int phc_index, const char* cfg_name)
{
        return FIND_ANY(INTF_KEY_CLOCK, &phc_index,
                INTF_KEY_NAME, cfg_name,
                SFPTPD_DB_SEL_END) != NULL;
}

static bool sysfs_file_exists(const char *base, const char *interface,
			      const char *filename)
{
	char path[PATH_MAX];
	struct stat stat_buf;

	assert(base != NULL);
	assert(interface != NULL);
	assert(filename != NULL);

	/* Create the path name of the file */
	snprintf(path, sizeof(path), "%s%s/%s", base, interface, filename);

	/* Check whether the file exists */
	return (stat(path, &stat_buf) == 0);
}

static bool sysfs_read_int(const char *base, const char *interface,
			   const char *filename, int *value)
{
	char path[PATH_MAX];
	FILE *file;
	int tokens;

	assert(base != NULL);
	assert(interface != NULL);
	assert(filename != NULL);
	assert(value != NULL);

	tokens = 0;
	*value = 0;

	/* Create the path name of the file */
	snprintf(path, sizeof(path), "%s%s/%s", base, interface, filename);

	file = fopen(path, "r");
	if (file == NULL) {
		TRACE_L4("interface %s: couldn't open %s\n", interface, path);
	} else {
		/* Try to read the integer from the file */
		tokens = fscanf(file, "%i", value);
		if (tokens != 1)
			TRACE_L4("interface %s: didn't find an integer in file %s\n",
				 interface, path);

		fclose(file);
	}

	return (tokens == 1);
}


static bool interface_check_suitability(const struct sfptpd_link *link,
					const char *sysfs_dir,
					sfptpd_interface_class_t *class)
{
	int vendor_id = 0;
	int device_id = 0;
	int i;
	const char *name;

	assert(sysfs_dir != NULL);
	assert(link != NULL);
	assert(class != NULL);

	name = link->if_name;

	/* Check what type of interface this is i.e. ethernet, ppp,
	 * infiniband etc and ignore all non-ethernet types.
	 *
	 * Even if the interface is ethernet type but we want to exclude
	 * devices that are wireless, bridges, vlan interfaces, bonds,
	 * tap devices and virtual interfaces */

	switch(link->type) {
	case SFPTPD_LINK_BRIDGE:
		TRACE_L2("interface %s: is a bridge - ignoring\n", name);
		return false;
	case SFPTPD_LINK_BOND:
		TRACE_L2("interface %s: is a bond - ignoring\n", name);
		return false;
	case SFPTPD_LINK_TEAM:
		TRACE_L2("interface %s: is a team - ignoring\n", name);
		return false;
	case SFPTPD_LINK_TUNNEL:
		TRACE_L2("interface %s: is a tunnel or tap interface - ignoring\n", name);
		return false;
	case SFPTPD_LINK_VLAN:
		TRACE_L2("interface %s: is a VLAN - ignoring\n", name);
		return false;
	case SFPTPD_LINK_IPVLAN:
	case SFPTPD_LINK_VETH:
	case SFPTPD_LINK_DUMMY:
	case SFPTPD_LINK_OTHER:
		TRACE_L2("interface %s: is virtual/other - ignoring\n", name);
		return false;
	case SFPTPD_LINK_MACVLAN:
	case SFPTPD_LINK_PHYSICAL:
		if (sysfs_file_exists(sysfs_dir, name, "wireless") ||
		    sysfs_file_exists(sysfs_dir, name, "phy80211")) {
			TRACE_L2("interface %s: is wireless - ignoring\n", name);
			return false;
		}
		if (link->type != SFPTPD_LINK_MACVLAN &&
		    sysfs_file_exists(SFPTPD_SYSFS_VIRTUAL_NET_PATH, "", name)) {
			TRACE_L2("interface %s: is virtual - ignoring\n", name);
			return false;
		}
		if (link->if_type != ARPHRD_ETHER) {
			TRACE_L2("interface %s: not ethernet (type %d) - ignoring\n",
				 name, link->if_type);
			return false;
		}
		break;
	default:
		assert(!"invalid link type");
	}

	/* Finally, get the vendor and device ID to determine if it is
	 * a Solarflare device or not and other static properties */
	if (!sysfs_read_int(sysfs_dir, name, "device/vendor", &vendor_id)) {
		WARNING("interface %s: couldn't read sysfs vendor ID\n", name);
	} else if (!sysfs_read_int(sysfs_dir, name, "device/device", &device_id)) {
		WARNING("interface %s: couldn't read sysfs device ID\n", name);
	}

	if (vendor_id == SFPTPD_SOLARFLARE_PCI_VENDOR_ID) {
		*class = SFPTPD_INTERFACE_SFC;
	} else {
		*class = SFPTPD_INTERFACE_OTHER;

		if (vendor_id == SFPTPD_XILINX_PCI_VENDOR_ID) {
			for (i = 0; i < sizeof xilinx_ptp_nics / sizeof *xilinx_ptp_nics; i++) {
				if (xilinx_ptp_nics[i] == device_id)
					*class = SFPTPD_INTERFACE_XNET;
			}
		}
	}

	return true;
}


static bool interface_is_ptp_capable(const char *name,
				     struct ethtool_ts_info *ts_info)
{
	assert(ts_info != NULL);

	/* We need a clock to use. If there isn't one then this port doesn't
	 * support ptp. */
	if (ts_info->phc_index == -1)
		return false;

	/* For SO_TIMESTAMPING we want raw hardware timestamps */
	if ((ts_info->so_timestamping & so_timestamping_raw) !=
	    so_timestamping_raw) {
		WARNING("interface %s: insufficient so_timestamping options, 0x%x\n",
			name, ts_info->so_timestamping);
		return false;
	}

	/* We need transmit timestamping support */
	if ((ts_info->tx_types & (1 << HWTSTAMP_TX_ON)) == 0) {
		WARNING("interface %s: transmit timestamping not supported, 0x%x\n",
			name, ts_info->tx_types);
		return false;
	}

	/* We need receive timestamping support */
	if ((ts_info->rx_filters & rx_filters_min) == 0) {
		WARNING("interface %s: receive timestamping not supported, 0x%x\n",
			name, ts_info->rx_filters);
		return false;
	}

	return true;
}


static int interface_get_hw_address(struct sfptpd_interface *interface)
{
	int rc;

	assert(interface != NULL);

	if (interface->link.perm_addr.len > 0) {
		/* Method 1. Already have acquired via netlink. */
		TRACE_L4("interface %s: got permanent hardware address via netlink\n",
			 interface->name);

		assert(interface->link.perm_addr.len <= sizeof interface->mac_addr.addr);
		interface->mac_addr.len = interface->link.perm_addr.len;
		memcpy(interface->mac_addr.addr, interface->link.perm_addr.addr, interface->link.perm_addr.len);
		sfptpd_strncpy(interface->mac_string, interface->link.perm_addr.string, sizeof interface->mac_string);
	} else {
		uint8_t buf[sizeof(struct ethtool_perm_addr) + ETH_ALEN];
		struct ethtool_perm_addr *req = (struct ethtool_perm_addr *)buf;

		/* Method 2. Use the ethtool interface to get the timestamping
		 * capabilities of the NIC. */
		TRACE_L4("interface %s: getting permanent hardware address\n",
			 interface->name);

		memset(buf, 0, sizeof(buf));
		req->cmd = ETHTOOL_GPERMADDR;
		req->size = ETH_ALEN;
		rc = sfptpd_interface_ioctl(interface, SIOCETHTOOL, req);
		if (rc != 0) {
			TRACE_L3("interface %s: failed to get permanent hardware address, %s\n",
				 interface->name, strerror(rc));
			return rc;
		}

		snprintf(interface->mac_string, sizeof(interface->mac_string),
			 "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
			 req->data[0], req->data[1], req->data[2],
			 req->data[3], req->data[4], req->data[5]);
		interface->mac_addr.len = req->size;
		if (interface->mac_addr.len > sizeof interface->mac_addr.addr)
			interface->mac_addr.len = sizeof interface->mac_addr.addr;
		memcpy(interface->mac_addr.addr, req->data, interface->mac_addr.len);
	}

	return 0;
}


static void interface_get_pci_ids(struct sfptpd_interface *interface,
				  const char *sysfs_dir)
{
	int id;
	int i;

	assert(interface != NULL);
	assert(sysfs_dir != NULL);

	/* Get the PCI vendor and device ID for this interface */
	if (sysfs_read_int(sysfs_dir, interface->name, "device/vendor", &id))
		interface->pci_vendor_id = id;
	else
		interface->pci_vendor_id = 0;
	if (sysfs_read_int(sysfs_dir, interface->name, "device/device", &id))
		interface->pci_device_id = id;
	else
		interface->pci_device_id = 0;

	for (i = 0; i < sizeof all_nic_models / sizeof *all_nic_models; i++) {
		const struct nic_model_caps *model = all_nic_models + i;
		if (interface->pci_vendor_id == model->vendor &&
		    interface->pci_device_id == model->device) {
			interface->static_caps = *model;
			break;
		}
	}
}


static int interface_get_versions(struct sfptpd_interface *interface)
{
	struct ethtool_drvinfo drv_info;
	bool have_bus_addr;
	bool have_stats_strs;
	int rc = 0;

	assert(interface != NULL);

	/* Only make the ioctl() if we actually need it, i.e.
	   we haven't got everything essential via netlink already. */
	have_bus_addr = interface->link.bus_addr[0] != '\0';
	have_stats_strs = interface->link.drv_stats_ids_state == QRY_POPULATED;

	if (!have_bus_addr || !have_stats_strs) {
		/* Set up the ethtool request */
		memset(&drv_info, 0, sizeof(drv_info));
		drv_info.cmd = ETHTOOL_GDRVINFO;

		rc = sfptpd_interface_ioctl(interface, SIOCETHTOOL, &drv_info);

		sfptpd_strncpy(interface->driver_version, drv_info.version,
			       sizeof(interface->driver_version));
		sfptpd_strncpy(interface->fw_version, drv_info.fw_version,
			       sizeof(interface->fw_version));
		sfptpd_strncpy(interface->driver, drv_info.driver,
			       sizeof(interface->driver));
	}

	if (!have_stats_strs) {
		interface->n_stats = drv_info.n_stats;
	} else {
		interface->n_stats = interface->link.drv_stats.all_count;
	}

	if (!have_bus_addr) {
		sfptpd_strncpy(interface->bus_addr, drv_info.bus_info,
			       sizeof(interface->bus_addr));
	} else  {
		sfptpd_strncpy(interface->bus_addr, interface->link.bus_addr,
			       sizeof(interface->bus_addr));
	}

	if (rc != 0)
		TRACE_L4("interface %s: failed to get driver info via ethtool, %s\n",
		      interface->name, strerror(rc));

	return rc;
}


static void interface_get_ts_info(struct sfptpd_interface *interface)
{
	int rc = 0;

	assert(interface != NULL);

	/* Method 1. Already have acquired via netlink. */
	if (interface->link.ts_info_state == QRY_POPULATED) {
		TRACE_L4("interface %s: got timestamping caps via ethtool netlink\n",
			 interface->name);
		interface->ts_info = interface->link.ts_info;
	} else {
		/* Method 2. Use the ethtool interface to get the timestamping
		 * capabilities of the NIC. */
		TRACE_L4("interface %s: getting timestamping caps via ethtool\n",
			 interface->name);

		/* Set up the ethtool request */
		memset(&interface->ts_info, 0, sizeof(interface->ts_info));
		interface->ts_info.cmd = ETHTOOL_GET_TS_INFO;

		rc = sfptpd_interface_ioctl(interface, SIOCETHTOOL, &interface->ts_info);
	}

	if (rc == 0) {
		interface->clock_supports_phc = (interface->ts_info.phc_index >= 0);
	} else {
		/* We aren't able to support timestamping on this interface so set the
		 * timestamping info to indicate software only and no PHC support */
		interface->clock_supports_phc = false;
		interface->ts_info = ts_info_sw_only;
	}
}


static void interface_check_efx_support(struct sfptpd_interface *interface)
{
	struct efx_sock_ioctl req;
	int rc;

	assert(interface != NULL);

	if ((interface->class == SFPTPD_INTERFACE_SFC ||
	    interface->link.type == SFPTPD_LINK_MACVLAN) &&
	    !interface->driver_supports_efx) {

		memset(&req, 0, sizeof(req));
		req.cmd = EFX_TS_SETTIME;
		req.u.ts_settime.iswrite = 0;

		rc = sfptpd_interface_ioctl(interface, SIOCEFX, &req);
		if (rc != EOPNOTSUPP) {
			interface->driver_supports_efx = true;
		}
	}
}


/* Must be called after interface_get_versions to populate driver info */
static void interface_driver_stats_init(struct sfptpd_interface *interface)
{
	struct ethtool_gstrings *gstrings = NULL;
	int rc, i, j, found;
	char path[PATH_MAX];

	assert(interface != NULL);

	TRACE_L4("interface %s: initialising driver stats-getting\n",
		 interface->name);

	/* Method 1. Get strings from ethtool netlink */
	if (interface->link.drv_stats_ids_state == QRY_POPULATED) {
		for (found = 0, i = 0; i < SFPTPD_DRVSTAT_MAX; i++) {
			if (interface->link.drv_stats.requested_ids[i] != -1) {
				interface->drv_stat_method[i] = DRV_STAT_ETHTOOL;
				interface->drv_stat_ethtool_index[i] = interface->link.drv_stats.requested_ids[i];
				interface->drv_stat_methods |= 1 << DRV_STAT_ETHTOOL;
				found++;
			}
		}
		TRACE_L5("interface %s: found %d/%d stats strings via ethtool netlink\n",
			 interface->name, found, SFPTPD_DRVSTAT_MAX);
		goto skip_ioctl;
	}

	/* Method 2. Get strings from ethtool ioctl */
	gstrings = calloc(1, ETH_GSTRING_LEN * interface->n_stats + sizeof *gstrings);
	if (gstrings == NULL) {
		ERROR("interface %s: could not allocate ethtool stat strings buffer, %s\n",
		      interface->name, strerror(errno));
		goto skip_ioctl;
	}

	gstrings->cmd = ETHTOOL_GSTRINGS;
	gstrings->string_set = ETH_SS_STATS;
	gstrings->len = interface->n_stats;

	rc = sfptpd_interface_ioctl(interface, SIOCETHTOOL, gstrings);
	if (rc != 0) {
		TRACE_L3("interface %s: failed to obtain ethtool stat strings, %s\n",
			 interface->name, strerror(errno));
		interface->n_stats = 0;
	}

	for (found = 0, i = 0; i < interface->n_stats; i++) {
		const char *name = (char *) gstrings->data + i * ETH_GSTRING_LEN;

		for (j = 0; j < SFPTPD_DRVSTAT_MAX; j++) {
			if (sfptpd_stats_ethtool_names[j] &&
			    !strncmp(name, sfptpd_stats_ethtool_names[j],
				     ETH_GSTRING_LEN))
				break;
		}

		if (j != SFPTPD_DRVSTAT_MAX) {
			interface->drv_stat_method[j] = DRV_STAT_ETHTOOL;
			interface->drv_stat_ethtool_index[j] = i;
			interface->drv_stat_methods |= 1 << DRV_STAT_ETHTOOL;
			found++;
		}
	}
	TRACE_L5("interface %s: found %d/%d stats strings via ethtool ioctl\n",
		 interface->name, found, SFPTPD_DRVSTAT_MAX);

skip_ioctl:
	/* Method 3. Use sysfs for stats */
	for (found = 0, j = 0; j < SFPTPD_DRVSTAT_MAX; j++) {
		if (interface->drv_stat_method[j] == DRV_STAT_NOT_AVAILABLE) {

			rc = snprintf(path, sizeof path, "/sys/class/net/%s/device/%s",
				      interface->name, drv_stats[j].sysfs_name);
			assert(rc > 0 && rc < sizeof path);

			if (!access(path, R_OK)) {
				interface->drv_stat_method[j] = DRV_STAT_SYSFS;
				interface->drv_stat_methods |= 1 << DRV_STAT_SYSFS;
				found++;
			}
		}

		TRACE_L5("interface %s: driver stat %s available by %s\n",
			 interface->name, sfptpd_stats_ethtool_names[j],
			 interface->drv_stat_method[j] == DRV_STAT_ETHTOOL ? "ethtool" :
			 (interface->drv_stat_method[j] == DRV_STAT_SYSFS ? "sysfs" :
			 "no method"));
	}
	TRACE_L5("interface %s: found %d/%d stats strings via sysfs\n",
		 interface->name, found, SFPTPD_DRVSTAT_MAX);

	if (gstrings)
		free(gstrings);

	if (interface->drv_stat_methods & (1 << DRV_STAT_ETHTOOL))
		interface->ethtool_stats = (struct ethtool_stats *) malloc(sizeof(struct ethtool_stats) + interface->n_stats * 8);
}


/****************************************************************************
 * Interface Internal Functions
 ****************************************************************************/

static int rescan_interfaces(void)
{
	sfptpd_interface_diagnostics(4);
	sfptpd_clock_rescan_interfaces();

	return 0;
}

static void free_interface_table(void)
{
	if (sfptpd_interface_table != NULL) {
		sfptpd_db_table_free(sfptpd_interface_table);
		sfptpd_interface_table = NULL;
	}
}

/* Assign a permanent unique identifier to any interfaces that have
   physical clock numbers. The sole purpose of the 'nic id' is to work
   around the unfortunate fact that 'phc' numbers can be reallocated,
   otherwise they refer to the same quantity, the physical clock. */
static int interface_assign_nic_id(struct sfptpd_interface *interface)
{
	regmatch_t matches[2];
	regex_t regex;
	int rc;

	/* Extract a portion of the bus address that identifies the NIC,
	   i.e. excluding the PCI function. Failure is not fatal. */
	rc = regcomp(&regex, "^([[:xdigit:]:]{8,}).[[:xdigit:]]+$", REG_EXTENDED);
	if (rc != 0) {
		TRACE_L3("interface %s: regcomp() failed\n",
			 interface->name);
	} else {
		rc = regexec(&regex, interface->bus_addr,
			     sizeof matches / sizeof *matches,
			     matches, 0);
		if (rc == 0 &&
		    matches[1].rm_so != -1) {
			/* Assumes destination already zeroed. */
			strncpy(interface->bus_addr_nic,
				interface->bus_addr + matches[1].rm_so,
				matches[1].rm_eo - matches[1].rm_so);
		} else {
			strncpy(interface->bus_addr_nic, interface->bus_addr, sizeof interface->bus_addr_nic);
		}

		regfree(&regex);
	}

	/* If there is no PHC number then there is no purpose for the
	   NIC number so leave as the -1 it is initialised to. */
	if (interface->ts_info.phc_index != -1 &&
	    interface->nic_id == -1) {
		struct sfptpd_interface *other_intf;
		int my_false = 0;

		/* First find any LIVE interfaces with the same clock id.

		   If we have found another *live* with the same clock id
		   then we are from the same NIC otherwise the OS would have
		   just assigned us a different clock id.
		*/
		other_intf = FIND_ANY(INTF_KEY_CLOCK, &interface->ts_info.phc_index,
				      SFPTPD_DB_SEL_NOT, INTF_KEY_IF_INDEX, &interface->if_index,
				      INTF_KEY_DELETED, &my_false);
		if (other_intf != NULL) {
			TRACE_L4("while trying to assign a permanently-unique nic id to new "
				 "interface %s,%s, found an already-active interface (%s,%s) "
				 "on the same nic (%d) because it shares the same phc index (%d=%d)\n",
				 interface->name,
				 interface->mac_string,
				 other_intf->name,
				 other_intf->mac_string,
				 other_intf->nic_id,
				 interface->ts_info.phc_index,
				 other_intf->ts_info.phc_index);
			interface->nic_id = other_intf->nic_id;
		} else {
			/* Then look for any DEAD interfaces with the same mac address */
			other_intf = FIND_ANY(INTF_KEY_MAC, &interface->mac_addr,
					      SFPTPD_DB_SEL_NOT,
					      INTF_KEY_IF_INDEX, &interface->if_index);
			if (other_intf != NULL &&
			    other_intf->ts_info.phc_index != -1) {
				TRACE_L4("while trying to assign a permanently-unique nic id to new "
					 "interface %s, found a previously-deleted interface %s (%d) "
					 "with the same mac address (%s) and therefore part of the same nic (%d)\n",
					 interface->name,
					 other_intf->name,
					 other_intf->if_index,
					 other_intf->mac_string,
					 other_intf->nic_id);
				interface->nic_id = other_intf->nic_id;
			} else {
				/* Then look for any LIVE interfaces with the same bus address,
				   not considering function. */
				if (interface->bus_addr_nic[0] &&
				    sfptpd_general_config_get(sfptpd_interface_config)->assume_one_phc_per_nic &&
				    (other_intf = FIND_ANY(INTF_KEY_BUS_ADDR_NIC,
							   &interface->bus_addr_nic,
							   SFPTPD_DB_SEL_NOT,
							   INTF_KEY_IF_INDEX, &interface->if_index,
							   INTF_KEY_DELETED,
							   &my_false)) != NULL &&
				    other_intf->ts_info.phc_index != -1) {
					TRACE_L4("while trying to assign a permanently-unique nic id to new "
						 "interface %s, found an already-active interface %s (%d) with the same bus "
						 "address (%s) and therefore part of the same nic (%d)\n",
						 interface->name,
						 other_intf->name,
						 other_intf->if_index,
						 other_intf->bus_addr_nic,
						 other_intf->nic_id);
					interface->nic_id = other_intf->nic_id;
					if (!other_intf->clock_supports_phc) {
						interface->ts_info.phc_index = other_intf->ts_info.phc_index;
						interface->clock_supports_phc = false;
					}
				} else {
					interface->nic_id = sfptpd_next_nic_id++;
					TRACE_L4("interface %s: allocated new nic id (%d)\n",
						 interface->name, interface->nic_id);
				}
			}
		}
	}
	return 0;
}

static int interface_init(const struct sfptpd_link *link, const char *sysfs_dir,
			  struct sfptpd_interface *interface,
			  sfptpd_interface_class_t class)
{
	int ret = 0;
	int rc;
	const char *name;
	char phc_num[16] = "";
	int if_index;

	assert(link != NULL);
	assert(sysfs_dir != NULL);
	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	name = link->if_name;
	if_index = link->if_index;

	interface->ts_enabled = false;
	interface->class = class;
	interface->link = *link;

	/* Default to system clock */
	sfptpd_interface_set_clock(interface, sfptpd_clock_get_system_clock());

	/* Take a copy of the interface name */
	sfptpd_strncpy(interface->name, name, sizeof(interface->name));

	interface->if_index = if_index;
	interface->deleted = false;
	interface->suitable = true;
	interface->static_caps.stratum = SFPTPD_CLOCK_STRATUM_MAX;

	/* Get the permanent hardware address of the interface */
	ret = interface_get_hw_address(interface);

	/* Get the PCI IDs */
	interface_get_pci_ids(interface, sysfs_dir);

	/* Get the driver and firmware versions */
	rc = interface_get_versions(interface);
	if (ret == 0)
		ret = rc;

	/* Get the timestamping capabilities of the interface */
	interface_get_ts_info(interface);

	/* Check whether the driver supports the EFX ioctl */
	interface_check_efx_support(interface);

	/* Check whether the device supports PHC */
	(void) interface_is_ptp_capable(interface->name, &interface->ts_info);
	snprintf(phc_num, sizeof phc_num, "(%d)", interface->ts_info.phc_index);

	TRACE_L3("interface %s: hw %s, flags%s%s%s\n",
		 interface->name, interface->mac_string,
		 interface->driver_supports_efx ? " efx" : "",
		 interface->ts_info.phc_index != -1 ? " phc" :"",
		 interface->ts_info.phc_index != -1 ? phc_num : "");
	if (interface->pci_vendor_id != 0)
		TRACE_L3("interface %s: device %hx:%hx%s at %s\n",
			 interface->name,
			 interface->pci_vendor_id,
			 interface->pci_device_id,
			 (interface->class == SFPTPD_INTERFACE_SFC ||
			  interface->class == SFPTPD_INTERFACE_XNET) ? " (Xilinx)" : "",
			 interface->bus_addr);
	if (interface->driver[0] != '\0')
		TRACE_L3("interface %s: %s %s, fw %s\n",
			 interface->name,
			 interface->driver,
			 (strcmp(interface->driver_version, sysinfo.release) == 0 ?
			  "in-tree" : interface->driver_version),
			 interface->fw_version);

	/* Assign NIC ID */
	interface_assign_nic_id(interface);

	/* Initialise stat-getting capability */
	interface_driver_stats_init(interface);

	return ret;
}

static void interface_delete(struct sfptpd_interface *interface,
			     bool disable_timestamping)
{
	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	if (interface->deleted)
		return;

	if (disable_timestamping)
		sfptpd_interface_hw_timestamping_disable(interface);

	interface->deleted = true;
	interface->clock = NULL;
}


static int interface_alloc(struct sfptpd_interface **interface)
{
	struct sfptpd_interface *new;

	assert(interface != NULL);

	/* Allocate an interface instance */
	new = (struct sfptpd_interface *)calloc(1, sizeof(*new));
	if (new == NULL) {
		*interface = NULL;
		return ENOMEM;
	}

	new->magic = SFPTPD_INTERFACE_MAGIC;
	new->deleted = true;
	new->if_index = -1;
	new->nic_id = -1;

	*interface = new;
	return 0;
}


static void interface_free(struct sfptpd_interface *interface)
{
	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	assert(interface->deleted);

	free(interface);
}


/* Updates the passed variable with the canonical interface object.
   Acquires the lock if found.
   Returns false if no interface was specified or it was deleted,
   otherwise true.  */
static bool interface_get_canonical_with_lock(struct sfptpd_interface **interface)
{
	struct sfptpd_interface *ptr;

	interface_lock();

	assert(interface != NULL);

	for (ptr = *interface; ptr != NULL; ptr = ptr->canonical) {
		if (ptr->canonical == NULL &&
		    !ptr->deleted) {
			*interface = ptr;
			return true;
		}
	}
	interface_unlock();
	return false;
}


/* Finds the interface with the given OS interface index */
static struct sfptpd_interface *interface_find_by_if_index(int index)
{
	return FIND_ANY(INTF_KEY_IF_INDEX, &index);
}


static struct sfptpd_interface *interface_find_by_name(const char *name)
{
	return FIND_ANY(INTF_KEY_NAME, name);
}


struct sfptpd_interface *sfptpd_interface_find_first_by_nic(int nic_id)
{
	struct sfptpd_interface *intf;

	interface_lock();

	intf = FIND_FIRST(INTF_KEY_MAC, INTF_KEY_NIC, &nic_id);

	if (interface_get_canonical_with_lock(&intf))
		interface_unlock();

	/* N.B. a second unlock is needed because it is a recursive mutex,
	   the canonicalising function acquires the lock if a live interface
	   is found and we need to pair up the locking and unlocking. */
	interface_unlock();

	return intf;
}


/* Enable timestamping on an interface if supported. To be used with a database
 * 'foreach' operation. On error, set shared return code. */
static void interface_enable_ts_if_supported(void *record, void *context) {
	struct sfptpd_interface *interface = *((struct sfptpd_interface **) record);
	int *rc = (int *) context;

	if (sfptpd_interface_rx_ts_caps(interface) & SFPTPD_INTERFACE_TS_CAPS_HW) {
		int local_rc = sfptpd_interface_hw_timestamping_enable(interface);
		if (local_rc != 0)
			*rc = local_rc;
	}
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_interface_initialise(struct sfptpd_config *config,
				pthread_mutex_t *hardware_state_lock,
				const struct sfptpd_link_table *link_table)
{
	struct sfptpd_interface *new, *interface;
	int rc, i, flags;
	sfptpd_config_timestamping_t *ts;
	sfptpd_interface_class_t class;
	int row;
	const struct sfptpd_link *link;

	assert(config != NULL);

	(void) uname(&sysinfo);
	sfptpd_interface_config = config;
	sfptpd_interface_lock = hardware_state_lock;

	sfptpd_interface_table = sfptpd_db_table_new(&interface_table_def, STORE_DEFAULT);

	/* Create a socket to access the ethernet interfaces. */
	sfptpd_interface_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (sfptpd_interface_socket < 0) {
		CRITICAL("failed to open socket to use for ifreq, %s\n",
			 strerror(errno));
		sfptpd_interface_shutdown(config);
		return errno;
	}

	flags = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE
	      | SOF_TIMESTAMPING_RAW_HARDWARE;
	if (setsockopt(sfptpd_interface_socket, SOL_SOCKET, SO_TIMESTAMPING,
		       &flags, sizeof(flags)) < 0) {
		CRITICAL("operating system does not support SO_TIMESTAMPING api\n");
		sfptpd_interface_shutdown(config);
		return errno;
	}

	/* Iterate through the interfaces in the system */
	for (row = 0; row < link_table->count; row++) {
		link = link_table->rows + row;

		/* Check that the interface is suitable i.e. an ethernet device
		 * that isn't wireless or a bridge or virtual etc */
		if (!interface_check_suitability(link, SFPTPD_SYSFS_NET_PATH, &class))
			continue;

		/* Create a new interface */
		rc = interface_alloc(&new);
		if (rc != 0) {
			ERROR("failed to allocate interface object for %s, %s\n",
			      link->if_name, strerror(rc));
			return rc;
		}

		rc = interface_init(link, SFPTPD_SYSFS_NET_PATH, new, class);
		if (rc != 0) {
			if (rc == ENOTSUP || rc == EOPNOTSUPP) {
				WARNING("skipping over insufficiently capable interface %s\n",
					link->if_name);
				interface_free(new);
				continue;
			} else {
				ERROR("failed to create interface instance for %s, %s\n",
				      link->if_name, strerror(rc));
				return rc;
			}
		}

		/* Add the interface to the database */
		sfptpd_db_table_insert(sfptpd_interface_table, &new);

		rescan_interfaces();
	}

	fixup_readonly_and_clock_lists();

	/* For each interface specified in the config file, enable packet timestamping */
	ts = &sfptpd_general_config_get(config)->timestamping;
	if (ts->all) {
		int rc = 0;
		sfptpd_db_table_foreach(sfptpd_interface_table,
					interface_enable_ts_if_supported, &rc);
		if (rc != 0)
			return rc;
	} else {
		for (i = 0; i < ts->num_interfaces; i++) {
			interface = sfptpd_interface_find_by_name(ts->interfaces[i]);
			if (interface == NULL) {
				ERROR("rx-timestamping: interface \"%s\" not found\n",
				      ts->interfaces[i]);
				return ENOENT;
			}

			if ((sfptpd_interface_rx_ts_caps(interface) & SFPTPD_INTERFACE_TS_CAPS_HW) == 0) {
				ERROR("interface %s does not support receive timestamping\n",
				      interface->name);
				return EOPNOTSUPP;
			}

			rc = sfptpd_interface_hw_timestamping_enable(interface);
			if (rc != 0)
				return rc;
		}
	}

	/* Output initial interface and clock lists */
	sfptpd_interface_diagnostics(3);
	sfptpd_clock_diagnostics(3);

	return 0;
}


void sfptpd_interface_diagnostics(int trace_level)
{
	if (trace_level > 4) {
		sfptpd_db_table_dump(trace_level, "interfaces-dead-or-alive", false,
				     sfptpd_interface_table,
				     SFPTPD_DB_SEL_ORDER_BY,
				     INTF_KEY_IF_INDEX,
				     SFPTPD_DB_SEL_END);
		return;
	}

	sfptpd_db_table_dump(trace_level, "interfaces", false,
			     sfptpd_interface_table,
			     INTF_KEY_DELETED, &(int){ false },
			     SFPTPD_DB_SEL_ORDER_BY,
			     INTF_KEY_IF_INDEX,
			     SFPTPD_DB_SEL_END);
}


static void interface_record_delete_fn(void *record, void *context) {
	struct sfptpd_interface *interface = *((struct sfptpd_interface **) record);
	bool *disable_on_exit = context;

	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	interface_delete(interface, *disable_on_exit);
}


static void interface_record_free_fn(void *record, void *context) {
	struct sfptpd_interface *interface = *((struct sfptpd_interface **) record);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	if (interface->ethtool_stats)
		free(interface->ethtool_stats);
	interface_free(interface);
}


void sfptpd_interface_shutdown(struct sfptpd_config *config)
{
	bool disable_on_exit = sfptpd_general_config_get(config)->timestamping.disable_on_exit;

	/* This happens if main exits before calling our initialise. */
	if (sfptpd_interface_socket == -1) return;

	interface_lock();
	sfptpd_interface_diagnostics(4);

	/* Mark all the interfaces as dead */
	sfptpd_db_table_foreach(sfptpd_interface_table,
				interface_record_delete_fn,
				&disable_on_exit);

	/* Let the system clean-up tidily now that all the
	   interfaces are marked as deleted. */
	rescan_interfaces();

	/* Free the interfaces */
	sfptpd_db_table_foreach(sfptpd_interface_table, interface_record_free_fn, NULL);

	/* Free the interface table */
	free_interface_table();

	if (sfptpd_interface_socket > 0)
		close(sfptpd_interface_socket);
	sfptpd_interface_socket = -1;
	interface_unlock();
}


static int interface_handle_rename(struct sfptpd_interface *interface,
				   const char *if_name)
{
	struct sfptpd_interface *other;

	INFO("interface: handling detected rename: %s -> %s (if_index %d)\n",
	     interface->name, if_name, interface->if_index);
	other = interface_find_by_name(if_name);
	if (other != NULL && other->deleted) {
		TRACE_L3("interface: aliasing deleted interface %d to %d\n",
			 other->if_index, interface->if_index);
		other->name[0] = '\0';
		other->canonical = interface;
	} else if (other != NULL) {
		CRITICAL("interface: renamed interface apparently still active as if_index %d\n",
			 other->if_index);
		return EEXIST;
	}
	return 0;
}


int sfptpd_interface_hotplug_insert(const struct sfptpd_link *link)
{
	struct sfptpd_interface *interface;
	int rc = 0;
	sfptpd_interface_class_t class;
	const char *if_name = link->if_name;
	const int if_index = link->if_index;
	bool change = false;

	interface_lock();

	interface = interface_find_by_if_index(if_index);
	if (interface == NULL) {
		/* Avoid searching by name - that would be unreliable */
		INFO("interface: hotplug insert for %s (%d)\n", if_name, if_index);
		if (interface != NULL) {

			/* Handle the case of an old interface in the list with the same name */
			if (interface->deleted) {

				/* Overwrite this interface object for the new interface */
				INFO("interface: replacing deleted interface %d\n", interface->if_index);
			} else {
				WARNING("interface: cannot process insertion of interface %s (%d) while undeleted interface of the same name (%d) still exists\n",
					 if_name, if_index, interface->if_index);
				rc = EINVAL;
				goto finish;
			}
		} else {

			/* Create the new interface object */
			rc = interface_alloc(&interface);
			if (rc != 0) {
				CRITICAL("failed to allocate interface object for %d, %s\n",
					 if_name, strerror(rc));
				goto finish;
			}

			/* Add the interface to the database */
			sfptpd_db_table_insert(sfptpd_interface_table, &interface);
		}
	} else if (0 != strcmp(interface->name, if_name)) {
		rc = interface_handle_rename(interface, if_name);
	} else {
		INFO("interface: handling detected changes: %s (if_index %d)\n",
		     if_name, if_index);
		change = true;
	}

	/* Check that the interface is suitable i.e. an ethernet device
	 * that isn't wireless or a bridge or virtual etc */
	if (!interface_check_suitability(link, SFPTPD_SYSFS_NET_PATH, &class)) {
		TRACE_L4("interface: ignoring interface %s of irrelevant type\n", if_name);
		sfptpd_strncpy(interface->name, if_name, sizeof(interface->name));
		interface->if_index = if_index;
		interface_delete(interface, false);
		rescan_interfaces();
		goto finish;
	}

	rc = interface_init(link, SFPTPD_SYSFS_NET_PATH, interface, class);
	if (rc == ENODEV) {
		WARNING("interface %s seems to have disappeared, deleting\n",
			if_name);
		interface_delete(interface, false);
		if (change)
			rc = 0;
	}

	rescan_interfaces();

	/* Now that we have configured the clock's readonly flag,
	 *  we can finally apply frequency correction, stepping etc.
	 */
	struct sfptpd_clock* clock = sfptpd_interface_get_clock(interface);
	sfptpd_clock_correct_new(clock);

	if (rc == ENOTSUP || rc == EOPNOTSUPP) {
		INFO("skipped over insufficiently capable interface %s\n", if_name);
	} else if (rc != 0) {
		ERROR("failed to create interface instance for %s, %s\n",
		      if_name, strerror(rc));
	}

 finish:
	interface_unlock();
	return rc;
}


int sfptpd_interface_hotplug_remove(const struct sfptpd_link *link)
{
	struct sfptpd_interface *interface;
	int rc = 0;
	const char *if_name = link->if_name;
	const int if_index = link->if_index;

	interface_lock();

	INFO("interface: hotplug remove for %s (%d)\n", link->if_name, if_index);

	/* If the ifindex has been provided, find the interface by index. If not,
	 * try to find the interface by name. */
	if (if_index >= 0) {
		interface = interface_find_by_if_index(if_index);
	} else {
		interface = interface_find_by_name(if_name);
	}

	if (interface == NULL) {
		TRACE_L3("interface: could not find interface to be deleted\n");
		/* No need for rc = ENOENT as we only record relevant links */;
	} else {
		if (interface->deleted) {
			TRACE_L3("interface: interface %s already deleted\n", if_name);
			/* No need for rc = ENOENT as we only record relevant links */;
		} else {
			interface_delete(interface, false);
			rescan_interfaces();
		}
	}

	interface_unlock();
	return rc;
}


/****************************************************************************/


struct sfptpd_interface *sfptpd_interface_find_by_if_index(int if_index)
{
	struct sfptpd_interface *interface;

	interface = interface_find_by_if_index(if_index);

	return interface;
}


struct sfptpd_interface *sfptpd_interface_find_by_name(const char *name)
{
	struct sfptpd_interface *interface;

	interface_lock();
	interface = interface_find_by_name(name);
	interface_unlock();

	return interface;
}


/****************************************************************************/

bool sfptpd_interface_is_deleted(struct sfptpd_interface *interface) {
	return interface->deleted;
}


int sfptpd_interface_get_nic_id(struct sfptpd_interface *interface) {
	return interface->nic_id;
}


const char *sfptpd_interface_get_mac_string(struct sfptpd_interface *interface)
{
	return interface->mac_string;
}

enum sfptpd_clock_stratum sfptpd_interface_get_clock_stratum(struct sfptpd_interface *interface)
{
	return interface->static_caps.stratum;
}

const char *sfptpd_interface_get_name(struct sfptpd_interface *interface)
{
	const char *name;

	if (interface_get_canonical_with_lock(&interface)) {
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
		name = interface->name;
		interface_unlock();
	} else {
		name = "(no-interface)";
	}
	return name;
}


void sfptpd_interface_get_mac_addr(struct sfptpd_interface *interface,
				   sfptpd_mac_addr_t *mac)
{
	if (!interface_get_canonical_with_lock(&interface)) {
		memset(mac, '\0', sizeof *mac);
		return;
	}
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	assert(mac != NULL);
	memcpy(mac, &interface->mac_addr, sizeof(*mac));
	interface_unlock();
}


const char *sfptpd_interface_get_fw_version(struct sfptpd_interface *interface)
{
	const char *fw_version;

	if (interface_get_canonical_with_lock(&interface)) {
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
		fw_version = interface->fw_version;
		interface_unlock();
	} else {
		fw_version = "(no-version)";
	}
	return fw_version;
}


int sfptpd_interface_get_ifindex(struct sfptpd_interface *interface)
{
	int ifindex = 0;

	if (interface_get_canonical_with_lock(&interface)) {
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
		ifindex = interface->if_index;
		interface_unlock();
	}

	return ifindex;
}


void sfptpd_interface_set_clock(struct sfptpd_interface *interface,
				struct sfptpd_clock *clock)
{
	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	assert(clock != NULL);

	interface->clock = clock;
}


struct sfptpd_clock *sfptpd_interface_get_clock(struct sfptpd_interface *interface)
{
	struct sfptpd_clock *clock;

	if (!interface_get_canonical_with_lock(&interface))
		return sfptpd_clock_get_system_clock();
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	clock = interface->clock;
	interface_unlock();
	return clock;
}


void sfptpd_interface_get_clock_device_idx(const struct sfptpd_interface *interface,
					   bool *supports_phc, int *device_idx,
					   bool *supports_efx)
{
	assert(interface != NULL);
	assert(supports_phc != NULL);
	assert(supports_efx != NULL);
	assert(device_idx != NULL);

	if (!interface_get_canonical_with_lock((struct sfptpd_interface **) &interface)) {
		*supports_phc = false;
		*supports_efx = false;
		*device_idx = -1;
		return;
	}

	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	*supports_phc = interface->clock_supports_phc;
	*supports_efx = interface->driver_supports_efx;
	*device_idx = interface->ts_info.phc_index;
	interface_unlock();
}


int sfptpd_interface_phc_unavailable(struct sfptpd_interface *interface)
{
	int device_idx;
	if (!interface_get_canonical_with_lock(&interface))
		return false;
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	interface->clock_supports_phc = false;
	device_idx = interface->ts_info.phc_index = interface->if_index;
	interface_unlock();
	return device_idx;
}


sfptpd_interface_class_t sfptpd_interface_get_class(struct sfptpd_interface *interface)
{
	sfptpd_interface_class_t class;
	if (!interface_get_canonical_with_lock(&interface))
		return SFPTPD_INTERFACE_OTHER;
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	class = interface->class;
	interface_unlock();
	return class;
}


sfptpd_interface_ts_caps_t sfptpd_interface_ptp_caps(struct sfptpd_interface *interface)
{
	sfptpd_interface_ts_caps_t caps = 0;
	struct ethtool_ts_info *ts_info;

	if (interface_get_canonical_with_lock(&interface)) {
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

		ts_info = &interface->ts_info;

		/* Check if the interface supports SO_TIMESTAMPING using software timestamps */
		if ((ts_info->so_timestamping & so_timestamping_sw) == so_timestamping_sw) {
			caps |= SFPTPD_INTERFACE_TS_CAPS_SW;
		}

		/* Check if the interface supports SO_TIMESTAMPING using hardware
		 * timestamps supports transmit timestamping and supports receive
		 * filtering of either all packets or just PTP packets */
		if (((ts_info->so_timestamping & so_timestamping_raw) == so_timestamping_raw) &&
		    (ts_info->phc_index != -1) &&
		    (((ts_info->rx_filters & (1 << HWTSTAMP_FILTER_ALL)) != 0) ||
		     ((ts_info->rx_filters & (1 << HWTSTAMP_FILTER_PTP_V2_EVENT)) != 0) ||
		     ((ts_info->rx_filters & (1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT)) != 0))) {
			caps |= SFPTPD_INTERFACE_TS_CAPS_HW;
		}
		interface_unlock();
	}

	return caps;
}


sfptpd_interface_ts_caps_t sfptpd_interface_rx_ts_caps(struct sfptpd_interface *interface)
{
	sfptpd_interface_ts_caps_t caps = 0;
	struct ethtool_ts_info *ts_info;

	if (interface_get_canonical_with_lock(&interface)) {
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

		ts_info = &interface->ts_info;

		/* Check if the interface supports SO_TIMESTAMPING using software timestamps */
		if ((ts_info->so_timestamping & so_timestamping_sw) == so_timestamping_sw) {
			caps |= SFPTPD_INTERFACE_TS_CAPS_SW;
		}

		/* Check if the interface supports SO_TIMESTAMPING using hardware timestamps
		 * and supports receive filtering of all packets */
		if (((ts_info->so_timestamping & so_timestamping_raw) == so_timestamping_raw) &&
		    (ts_info->phc_index != -1) &&
		    ((ts_info->rx_filters & (1 << HWTSTAMP_FILTER_ALL)) != 0)) {
			caps |= SFPTPD_INTERFACE_TS_CAPS_HW;
		}
		interface_unlock();
	}

	return caps;
}


bool sfptpd_interface_supports_ptp(struct sfptpd_interface *interface)
{
	sfptpd_interface_ts_caps_t ptp_caps;
	bool support;
	struct sfptpd_config_general *general_config;

	if (!interface_get_canonical_with_lock(&interface))
		return false;
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	general_config = sfptpd_general_config_get(sfptpd_interface_config);

	/* Just use regular software timestamping if non_sfc_nics is disabled. */
	if (interface->class != SFPTPD_INTERFACE_SFC &&
	    interface->class != SFPTPD_INTERFACE_XNET &&
	    !general_config->non_sfc_nics) {
		support = false;
	} else {
		ptp_caps = sfptpd_interface_ptp_caps(interface);
		support = (ptp_caps & SFPTPD_INTERFACE_TS_CAPS_HW) &&
			(interface->ts_info.phc_index != -1);
	}
	interface_unlock();
	return support;
}


bool sfptpd_interface_supports_pps(struct sfptpd_interface *interface)
{
	bool support;
	struct sfptpd_config_general *general_config;

	if (!interface_get_canonical_with_lock(&interface))
		return false;
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	general_config = sfptpd_general_config_get(sfptpd_interface_config);

	support = (interface->ts_info.phc_index != -1) &&
		  ((interface->class == SFPTPD_INTERFACE_SFC) ||
		   (interface->class == SFPTPD_INTERFACE_XNET) ||
		   general_config->non_sfc_nics);
	interface_unlock();
	return support;
}


int sfptpd_interface_is_link_detected(struct sfptpd_interface *interface,
				      bool *link_detected)
{
	assert(link_detected != NULL);
	*link_detected = false;

	if (!interface_get_canonical_with_lock(&interface))
		return EINVAL;

	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	*link_detected = !!(interface->link.if_flags & IFF_UP);
	interface_unlock();
	return 0;
}


/****************************************************************************/

struct sfptpd_db_query_result sfptpd_interface_get_all_snapshot(void)
{
	return sfptpd_db_table_query(sfptpd_interface_table,
				     SFPTPD_DB_SEL_ORDER_BY,
				     INTF_KEY_NIC,
				     INTF_KEY_TYPE,
				     INTF_KEY_MAC);
}


struct sfptpd_db_query_result sfptpd_interface_get_active_ptp_snapshot(void)
{
	return sfptpd_db_table_query(sfptpd_interface_table,
				     INTF_KEY_DELETED, &(int){ false },
				     INTF_KEY_PTP, &(int){ true },
				     SFPTPD_DB_SEL_ORDER_BY,
				     INTF_KEY_NIC,
				     INTF_KEY_TYPE,
				     INTF_KEY_MAC);
}


/****************************************************************************/

static int interface_check_hotplug_rename(struct sfptpd_interface *interface)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_ifindex = interface->if_index;

	/* bug74449: hotplug may have renamed the interface while we weren't
	 * looking. Check for this, and update interface name as required. */
	if (ioctl(sfptpd_interface_socket, SIOCGIFNAME, &ifr) < 0) {
		return errno;
	}

	if (0 == strncmp(ifr.ifr_name, interface->name, sizeof ifr.ifr_name)) {
		return 0;
	}

	INFO("interface %s: hotplug changed name during ioctl -> %s (%d)\n",
	     interface->name, ifr.ifr_name, interface->if_index);

	interface_handle_rename(interface, ifr.ifr_name);
	sfptpd_strncpy(interface->name, ifr.ifr_name, sizeof(interface->name));

	return EAGAIN;
}

int sfptpd_interface_ioctl(struct sfptpd_interface *interface,
			   int request, void *data)
{
	struct ifreq ifr;
	int rc = 0;

	if (!interface_get_canonical_with_lock(&interface))
		return EINVAL;

	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	assert(data != NULL);

	/* Set up the ifrequest structure with the interface name */
	memset(&ifr, 0, sizeof(ifr));
	sfptpd_strncpy(ifr.ifr_name, interface->name, sizeof ifr.ifr_name);
	ifr.ifr_data = data;

	/* bug74449: check for hotplug renames before & after ioctl
	 *
	 * There is still a small window of opportunity for things to go
	 * catastrophically wrong iif :
	 *   a) The interface is renamed right after this call (but before ioctl)
	 *   b) Another interface is hotplugged and is assigned the same name
	 *   c) The ioctl is destructive (e.g. apply clock offset)
	 *
	 * The netlink code tries to avoid this, so raise an error if it happens
	 */

	/* This call will update interface->name if it has changed. */
	(void)interface_check_hotplug_rename(interface);

	if (ioctl(sfptpd_interface_socket, request, &ifr) < 0)
		rc = errno;

	if (EAGAIN == interface_check_hotplug_rename(interface)) {
		ERROR("interface %s (%d): renamed during ioctl %d, things may be in a bad state!\n",
				interface->name, interface->if_index, request);

	}

	interface_unlock();
	return rc;
}


int sfptpd_interface_hw_timestamping_enable(struct sfptpd_interface *interface)
{
	int rx_filter, rc;

	if (!interface_get_canonical_with_lock(&interface))
		return EINVAL;

	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	/* We enable timestamping of all packets if this is supported. Otherwise,
	 * just enable timestamping of PTP event packets */
	if (interface->ts_info.rx_filters & (1 << HWTSTAMP_FILTER_ALL))
		rx_filter = HWTSTAMP_FILTER_ALL;
	else if (interface->ts_info.rx_filters & (1 << HWTSTAMP_FILTER_PTP_V2_EVENT))
		rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
	else
		rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;

	if (interface->ts_enabled) {
		TRACE_L4("interface: timestamping already enabled for %s\n",
			 interface->name);
		rc = 0;
	} else {
		/* (The method to enable timestamping depends on the kernel version) */

		/* Use SO_TIMESTAMPING */
		struct hwtstamp_config so_ts_req;
		int n_retries = 0;

		memset(&so_ts_req, 0, sizeof(so_ts_req));
		so_ts_req.tx_type = HWTSTAMP_TX_ON;
		so_ts_req.rx_filter = rx_filter;

		for (n_retries = 0; n_retries < 5; n_retries++) {
			rc = sfptpd_interface_ioctl(interface, SIOCSHWTSTAMP, &so_ts_req);
			if (rc == 0) {
				INFO("interface %s: SO_TIMESTAMPING enabled\n",
					interface->name);
				interface->ts_enabled = true;
				break;
			}

			/* If we get EBUSY, retry a few times to mitigate SWFWHUNT-2829.
			 * On SFC NICs, enabling hw timestamping requires a clock sync op,
			 * which in turn relies on the system not being overloaded. This
			 * can be especially problematic at system startup, i.e. now. */
			if (rc == EBUSY || rc == EAGAIN) {
				usleep(100000);
			} else {
				break;
			}
		}

		if (n_retries > 0 && rc == 0) {
			WARNING("interface %s: enabling timestamping took %d retries\n",
				interface->name, n_retries);
		}
	}

	if (rc == EOPNOTSUPP || rc == EPERM || rc == EACCES) {
		struct hwtstamp_config so_ts_req;
		int rc2;

		rc2 = sfptpd_interface_ioctl(interface, SIOCGHWTSTAMP, &so_ts_req);
		if (rc2 == 0 &&
		    so_ts_req.tx_type == HWTSTAMP_TX_ON &&
		    ((1 << so_ts_req.rx_filter) & rx_filters_min) != 0) {
			INFO("interface %s: using already-enabled hardware timestamping\n",
			     interface->name);
			interface->ts_enabled = true;
			rc = 0;
		}
	}

	if (rc != 0) {
		ERROR("interface %s: failed to enable packet timestamping: %s\n",
		      interface->name, strerror(rc));
	}

	interface_unlock();
	return rc;
}


void sfptpd_interface_hw_timestamping_disable(struct sfptpd_interface *interface)
{
	struct hwtstamp_config so_ts_req;

	if (!interface_get_canonical_with_lock(&interface)) {
		TRACE_L4("interface: can't disable timestamping on missing interface\n");
		return;
	}

	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	interface->ts_enabled = false;

	/* Disable timestamping via the SO_TIMESTAMPING API */
	memset(&so_ts_req, 0, sizeof(so_ts_req));
	so_ts_req.tx_type = HWTSTAMP_TX_OFF;
	so_ts_req.rx_filter = HWTSTAMP_FILTER_NONE;
	(void)sfptpd_interface_ioctl(interface, SIOCSHWTSTAMP, &so_ts_req);

	interface_unlock();
}


int sfptpd_interface_driver_stats_read(struct sfptpd_interface *interface,
				       uint64_t stats[SFPTPD_DRVSTAT_MAX])
{
	int rc;
	int i;
	char path[PATH_MAX];
	FILE *file;
	struct ethtool_stats *estats;
	int sysfs_stat_value;

	assert(interface != NULL);

	if (interface->drv_stat_methods == 0)
		return ENODATA;

	estats = interface->ethtool_stats;

	if (interface->drv_stat_methods & (1 << DRV_STAT_ETHTOOL)) {
		assert(estats != NULL);

		estats->cmd = ETHTOOL_GSTATS;
		estats->n_stats = interface->n_stats;

		rc = sfptpd_interface_ioctl(interface, SIOCETHTOOL, estats);
		if (rc != 0) {
			TRACE_L3("interface %s: failed to obtain ethtool stats, %s\n",
				 interface->name, strerror(errno));
			return errno;
		}
	}

	for (i = 0; i < SFPTPD_DRVSTAT_MAX; i++) {
		enum drv_stat_method method = interface->drv_stat_method[i];
		switch (method) {
		case DRV_STAT_ETHTOOL:
			stats[i] = estats->data[interface->drv_stat_ethtool_index[i]];
			break;
		case DRV_STAT_SYSFS:
			rc = snprintf(path, sizeof path, "/sys/class/net/%s/device/%s",
				      interface->name, drv_stats[i].sysfs_name);
			if (rc < 0 || rc >= sizeof path)
				return ENAMETOOLONG;
			file = fopen(path, "r");
			if (file == NULL) {
				TRACE_L1("failed to open PPS stats file %s, %s\n",
					 path, errno);
				return errno;
			}
			if (fscanf(file, "%d", &sysfs_stat_value) != 1) {
				ERROR("couldn't read statistic from %s\n", path);
				fclose(file);
				return EIO;
			}
			stats[i] = sysfs_stat_value;
			fclose(file);
			break;
		case DRV_STAT_NOT_AVAILABLE:
			TRACE_L4("no method available to collect %s stat\n",
				 sfptpd_stats_ethtool_names[i]);
		}

		/* Adjust for virtual resets */
		stats[i] += interface->stat_zero_adjustment[i];
	}

	return 0;
}

static int interface_sysfs_stats_reset(struct sfptpd_interface *interface)
{
	char path[128];
	FILE *file;
	int rc;

	assert(interface);

	rc = snprintf(path, sizeof(path), "/sys/class/net/%s/device/ptp_stats",
		      sfptpd_interface_get_name(interface));
	if (rc > 0 && rc < sizeof(path)) {
		file = fopen(path, "w");
		if (file) {
			rc = fputs("1\n", file) >= 0 ? 0 : errno;
			fclose(file);
		} else {
			rc = errno;
		}
	} else {
		rc = errno;
	}

	return rc;
}

int sfptpd_interface_driver_stats_reset(struct sfptpd_interface *interface)
{
	uint64_t sample[SFPTPD_DRVSTAT_MAX];
	bool sysfs_stats_reset = false;
	bool stats_sampled = false;
	bool reset_failed = false;
	bool read_failed = false;
	int ret = 0;
	int rc;
	int i;

	assert(interface != NULL);

	for (i = 0; i < SFPTPD_DRVSTAT_MAX; i++) {
		if (!drv_stats[i].counter)
			continue;

		/* If any stats are from sysfs, perform the stats reset through
		 * sysfs. If this fails due to permissions, e.g. not running
		 * as root, then fall through to the ethtool stats method of
		 * a virtual reset where we subtract the count as it was
		 * at reset. */

		switch (interface->drv_stat_method[i]) {
		case DRV_STAT_SYSFS:
			if (!sysfs_stats_reset && !reset_failed) {
				rc = interface_sysfs_stats_reset(interface);
				if (rc != 0) {
					reset_failed = true;
					if (rc != EACCES)
						ret = rc;
				} else {
					sysfs_stats_reset = true;
				}
			}
			if (sysfs_stats_reset)
				break;
			/* Fall through to virtual reset */
		case DRV_STAT_ETHTOOL:
			if (!stats_sampled && !read_failed) {
				rc = sfptpd_interface_driver_stats_read(interface, sample);
				if (rc != 0) {
					read_failed = true;
					ret = rc;
				} else {
					stats_sampled = true;
				}
			}
			if (stats_sampled)
				interface->stat_zero_adjustment[i] -= sample[i];
			break;
		case DRV_STAT_NOT_AVAILABLE:
			break;
		}
	}

	return ret;
}


/* fin */
