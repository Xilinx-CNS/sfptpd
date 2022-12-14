/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2021 Xilinx, Inc. */

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
#include <math.h>
#include <assert.h>
#include <fts.h>
#include <sys/stat.h>
#include <linux/sockios.h>
#include <linux/socket.h>
#include <linux/if_ether.h>
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
 * Defines for PTP functionality
 ****************************************************************************/

/* SIOCSHWTSTAMP is defined in linux/sockios.h in recent kernels.
 * Define it for compilation with older kernels.
 */
#ifndef SIOCSHWTSTAMP
#define SIOCSHWTSTAMP 0x89b0
#endif

/* SO_TIMESTAMPING is defined in asm/socket.h in recent kernels.
 * Define it for compilation with older kernels.
 */
#ifndef SO_TIMESTAMPING
#define SO_TIMESTAMPING  37
#endif


/****************************************************************************
 * Types, Defines and Structures
 ****************************************************************************/

#define VERSION_DECL(n) { n ## _VERSION_A, n ## _VERSION_B, \
			  n ## _VERSION_C, n ## _VERSION_D }

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

struct sfptpd_version_number {
	uint32_t major;
	uint32_t minor;
	uint32_t revision;
	uint32_t build;
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
	char mac_string[SFPTPD_CONFIG_MAC_STRING_MAX];

	/* PCI device ID */
	uint16_t pci_device_id;
	uint16_t pci_vendor_id;
	char bus_addr[ETHTOOL_BUSINFO_LEN];

	/* Portion of bus address identifying the NIC */
	char bus_addr_nic[ETHTOOL_BUSINFO_LEN];

	/* Firmware and driver versions */
	char driver_version[SFPTPD_VERSION_STRING_MAX];
	char fw_version[SFPTPD_VERSION_STRING_MAX];

	/* NIC model and serial numbers */
	char product[SFPTPD_NIC_PRODUCT_NAME_MAX];
	char model[SFPTPD_NIC_MODEL_MAX];
	char serial_num[SFPTPD_NIC_SERIAL_NUM_MAX];

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
};


/****************************************************************************
 * Constants
 ****************************************************************************/

static const struct ethtool_ts_info ts_info_hw_default =
{
	.so_timestamping = SOF_TIMESTAMPING_RX_SOFTWARE
			 | SOF_TIMESTAMPING_SOFTWARE
			 | SOF_TIMESTAMPING_TX_HARDWARE
			 | SOF_TIMESTAMPING_RX_HARDWARE
			 | SOF_TIMESTAMPING_RAW_HARDWARE,
	.phc_index = -1,
	.tx_types = (1 << HWTSTAMP_TX_OFF)
		  | (1 << HWTSTAMP_TX_ON),
	.rx_filters = (1 << HWTSTAMP_FILTER_NONE)
		    | (1 << HWTSTAMP_FILTER_PTP_V1_L4_EVENT)
		    | (1 << HWTSTAMP_FILTER_PTP_V1_L4_SYNC)
		    | (1 << HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ)
		    | (1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT)
		    | (1 << HWTSTAMP_FILTER_PTP_V2_L4_SYNC)
		    | (1 << HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ)
};

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

static const struct sfptpd_version_number hunt_driver_version_min =
	VERSION_DECL(SFPTPD_HUNT_DRIVER);
static const struct sfptpd_version_number hunt_fw_version_min =
	VERSION_DECL(SFPTPD_HUNT_FW);

static const struct sfptpd_version_number siena_driver_version_min =
	VERSION_DECL(SFPTPD_SIENA_DRIVER);
static const struct sfptpd_version_number siena_fw_version_min =
	VERSION_DECL(SFPTPD_SIENA_FW);

static const uint16_t xilinx_ptp_nics[] = {
	0x5084, /*!< X3522 */
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
SEARCH_COMPAR_FN(bus_addr_nic, char, key, rec, strcmp(key, rec->bus_addr_nic))

/* Create sort comparison functions */
SORT_COMPAR_FN(clock, rec, &rec->ts_info.phc_index)
SORT_COMPAR_FN(if_index, rec, &rec->if_index)
SORT_COMPAR_FN(name, rec, rec->name)
SORT_COMPAR_FN(mac, rec, &rec->mac_addr)
SORT_COMPAR_FN(nic, rec, &rec->nic_id)
SORT_COMPAR_FN(deleted, rec, &rec->deleted)
SORT_COMPAR_FN(ptp, rec, &rec->nic_id)
SORT_COMPAR_FN(bus_addr_nic, rec, rec->bus_addr_nic)

/* Create print functions */
SNPRINT_FN(clock, rec, "%*d", rec->ts_info.phc_index)
SNPRINT_FN(if_index, rec, "%*d", rec->if_index)
SNPRINT_FN(name, rec, "%*s", rec->name)
SNPRINT_FN(mac, rec, "%*s", rec->mac_string)
SNPRINT_FN(nic, rec, "%*d", rec->nic_id)
SNPRINT_FN(deleted, rec, "%*s", rec->deleted ? "deleted" : "")
SNPRINT_FN(ptp, rec, "%*s", (rec->nic_id != -1) ? "ptp" : "")
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


/****************************************************************************
 * Forward declarations
 ****************************************************************************/

static void interface_diagnostics(int trace_level);


/****************************************************************************
 * Interface Operations
 ****************************************************************************/

static inline void interface_lock(void) {
	int rc = pthread_mutex_lock(sfptpd_interface_lock);
	if (rc != 0) {
		CRITICAL("interface: could not acquire hardware state lock\n");
		exit(1);
	}
}


static inline void interface_unlock(void) {
	int rc = pthread_mutex_unlock(sfptpd_interface_lock);
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


static bool sysfs_path_exists(const char *base, const char *interface)
{
	char path[PATH_MAX];
	struct stat stat_buf;

	assert(base != NULL);
	assert(interface != NULL);

	/* Create the path name of the file */
	snprintf(path, sizeof(path), "%s%s", base, interface);

	/* Check whether the file exists */
	return (stat(path, &stat_buf) == 0) && (S_ISDIR(stat_buf.st_mode));
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


static bool interface_check_suitability(const char *sysfs_dir, const char *name,
					sfptpd_interface_class_t *class)
{
	int type;
	int vendor_id;
	int device_id;
	int i;

	assert(sysfs_dir != NULL);
	assert(name != NULL);
	assert(class != NULL);

	/* If the interface name isn't a directory then ignore it */
	if (!sysfs_path_exists(sysfs_dir, name))
		return false;

	/* First, check what type of interface this is i.e. ethernet, ppp,
	 * infiniband etc and ignore all non-ethernet types. */
	if (!sysfs_read_int(sysfs_dir, name, "type", &type)) {
		WARNING("interface %s: couldn't read sysfs type file\n", name);
		return false;
	}

	if (type != ARPHRD_ETHER) {
               TRACE_L2("interface %s: not ethernet (type %d) - ignoring\n",
                         name, type);
		return false;
	}

	/* The interface is ethernet type but we want to exclude devices that are
	 * wireless, bridges, vlan interfaces, bonds, tap devices and virtual
	 * interfaces */
	if (sysfs_file_exists(sysfs_dir, name, "wireless") ||
	    sysfs_file_exists(sysfs_dir, name, "phy80211")) {
		TRACE_L2("interface %s: is wireless - ignoring\n", name);
		return false;
	}

	if (sysfs_file_exists(sysfs_dir, name, "bridge")) {
		TRACE_L2("interface %s: is a bridge - ignoring\n", name);
		return false;
	}

	if (sysfs_file_exists(sysfs_dir, name, "bonding")) {
		TRACE_L2("interface %s: is a bond - ignoring\n", name);
		return false;
	}

	if (sysfs_file_exists(sysfs_dir, name, "tun_flags")) {
		TRACE_L2("interface %s: is a tap interface - ignoring\n", name);
		return false;
	}

	if (sysfs_file_exists(SFPTPD_PROC_VLAN_PATH, "", name)) {
		TRACE_L2("interface %s: is a VLAN - ignoring\n", name);
		return false;
	}

	if (sysfs_file_exists(SFPTPD_SYSFS_VIRTUAL_NET_PATH, "", name)) {
		TRACE_L2("interface %s: is virtual - ignoring\n", name);
		return false;
	}

	/* Finally, get the vendor ID of the device and determine if it is
	 * a Solarflare device or not */
	if (!sysfs_read_int(sysfs_dir, name, "device/vendor", &vendor_id)) {
		WARNING("interface %s: couldn't read sysfs vendor ID\n", name);
		return false;
	}

	if (vendor_id == SFPTPD_SOLARFLARE_PCI_VENDOR_ID) {
		*class = SFPTPD_INTERFACE_SFC;
	} else {
		*class = SFPTPD_INTERFACE_OTHER;

		if (vendor_id == SFPTPD_XILINX_PCI_VENDOR_ID) {
			if (!sysfs_read_int(sysfs_dir, name, "device/device", &device_id)) {
				WARNING("interface %s: couldn't read sysfs vendor ID\n", name);
				return false;
			}

			for (i = 0; i < sizeof xilinx_ptp_nics / sizeof *xilinx_ptp_nics; i++) {
				if (xilinx_ptp_nics[i] == device_id)
					*class = SFPTPD_INTERFACE_XNET;
			}
		}
	}

	if (*class == SFPTPD_INTERFACE_SFC || *class == SFPTPD_INTERFACE_XNET)
		TRACE_L2("interface %s: Xilinx%s device\n", name,
			 *class == SFPTPD_INTERFACE_SFC ? " (Solarflare)" : "");

	return true;
}


static bool interface_is_ptp_capable(const char *name,
				     struct ethtool_ts_info *ts_info)
{
	const uint32_t rx_filters_min = (1 << HWTSTAMP_FILTER_ALL)
				      | (1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT)
				      | (1 << HWTSTAMP_FILTER_PTP_V2_EVENT);

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


static void interface_check_versions(struct sfptpd_interface *interface)
{
	int tokens;
	struct sfptpd_version_number have;
	const struct sfptpd_version_number *want_driver, *want_fw;

	assert(interface != NULL);

	/* Check the minimum fw version according to whether this is a
	 * Siena or Huntington/Medford based adapter. */
	if (sfptpd_interface_is_siena(interface)) {
		want_driver = &siena_driver_version_min;
		want_fw = &siena_fw_version_min;
	} else {
		want_driver = &hunt_driver_version_min;
		want_fw = &hunt_fw_version_min;
	}

	tokens = sscanf(interface->driver_version, "%u.%u.%u.%u", &have.major,
			&have.minor, &have.revision, &have.build);
	if (tokens != 4) {
		ERROR("interface %s: unexpected driver version string, %s\n",
		      interface->name, interface->driver_version);
	} else {
		if (memcmp(&have, want_driver, sizeof(have)) < 0) {
			CRITICAL("### interface %s NIC driver is too old ###\n",
				 interface->name);
			INFO("require driver version %d.%d.%d.%d or later\n",
			     want_driver->major, want_driver->minor,
			     want_driver->revision, want_driver->build);
		}
	}

	tokens = sscanf(interface->fw_version, "%u.%u.%u.%u", &have.major,
			&have.minor, &have.revision, &have.build);
	if (tokens != 4) {
		ERROR("interface %s: unexpected firmware version string, %s\n",
		      interface->name, interface->fw_version);
	} else {
		if (memcmp(&have, want_fw, sizeof(have)) < 0) {
			CRITICAL("### interface %s NIC firmware is too old ###\n",
				 interface->name);
			INFO("require firmware version %d.%d.%d.%d or later\n",
			     want_fw->major, want_fw->minor,
			     want_fw->revision, want_fw->build);
		}
	}
}


static int interface_get_hw_address(struct sfptpd_interface *interface)
{
	uint8_t buf[sizeof(struct ethtool_perm_addr) + ETH_ALEN];
	struct ethtool_perm_addr *req = (struct ethtool_perm_addr *)buf;
	int rc;

	assert(interface != NULL);

	memset(buf, 0, sizeof(buf));
	req->cmd = ETHTOOL_GPERMADDR;
	req->size = ETH_ALEN;

	rc = sfptpd_interface_ioctl(interface, SIOCETHTOOL, req);
	if (rc != 0) {
		WARNING("interface %s: failed to get permanent hardware address, %s\n",
			interface->name, strerror(rc));
		return rc;
	}

	snprintf(interface->mac_string, sizeof(interface->mac_string),
		 "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
		 req->data[0], req->data[1], req->data[2],
		 req->data[3], req->data[4], req->data[5]);
	memcpy(interface->mac_addr.addr, req->data, sizeof(interface->mac_addr.addr));

	TRACE_L3("interface %s: hardware address %s\n",
		 interface->name, interface->mac_string);
	return 0;
}


static void interface_get_pci_ids(struct sfptpd_interface *interface,
				  const char *sysfs_dir)
{
	int id;

	assert(interface != NULL);
	assert(sysfs_dir != NULL);

	/* Get the PCI vendor and device ID for this interface */
	if (sysfs_read_int(sysfs_dir, interface->name, "device/vendor", &id))
		interface->pci_vendor_id = id;
	if (sysfs_read_int(sysfs_dir, interface->name, "device/device", &id))
		interface->pci_device_id = id;

	TRACE_L3("interface %s: PCI IDs vendor = 0x%hx, device = 0x%hx\n",
		 interface->name, interface->pci_vendor_id,
		 interface->pci_device_id);
}


static void interface_get_versions(struct sfptpd_interface *interface)
{
	struct ethtool_drvinfo drv_info;
	int rc;

	assert(interface != NULL);

	/* Set up the ethtool request */
	memset(&drv_info, 0, sizeof(drv_info));
	drv_info.cmd = ETHTOOL_GDRVINFO;

	rc = sfptpd_interface_ioctl(interface, SIOCETHTOOL, &drv_info);
	if (rc != 0) {
		ERROR("interface %s: failed to get driver info via ethtool, %s\n",
		      interface->name, strerror(rc));
		return;
	}

	sfptpd_strncpy(interface->driver_version, drv_info.version,
		       sizeof(interface->driver_version));
	sfptpd_strncpy(interface->fw_version, drv_info.fw_version,
		       sizeof(interface->fw_version));
	sfptpd_strncpy(interface->bus_addr, drv_info.bus_info,
		       sizeof(interface->bus_addr));

	TRACE_L3("interface %s: driver version %s, firmware version %s\n",
		 interface->name, interface->driver_version, interface->fw_version);
	TRACE_L3("interface %s: bus address %s\n",
		 interface->name, interface->bus_addr);
}


static void interface_get_ts_info(struct sfptpd_interface *interface,
				  const char *sysfs_dir)
{
	int if_index, rc;
	struct efx_sock_ioctl req;

	assert(interface != NULL);
	assert(sysfs_dir != NULL);

	/* Method 1. Use the ethtool interface to get the timestamping
	 * capabilities of the NIC. */
	TRACE_L4("interface %s: getting timestamping caps via ethtool\n",
		 interface->name);

	/* Set up the ethtool request */
	memset(&interface->ts_info, 0, sizeof(interface->ts_info));
	interface->ts_info.cmd = ETHTOOL_GET_TS_INFO;

	rc = sfptpd_interface_ioctl(interface, SIOCETHTOOL, &interface->ts_info);
	if (rc == 0) {
		interface->clock_supports_phc = (interface->ts_info.phc_index >= 0);
		return;
	}

	if (interface->class == SFPTPD_INTERFACE_SFC) {
		/* Method 2. For Solarflare adapters using newer drivers on
		 * kernels that don't support PHC, try using a private ioctl to
		 * get the timestamping capabilities of the NIC. */
		TRACE_L4("interface %s: getting timestamping caps via private ioctl\n",
			 interface->name);

		memset(&req, 0, sizeof(req));
		req.cmd = EFX_GET_TS_INFO;
		req.u.ts_info.cmd = ETHTOOL_GET_TS_INFO;

		rc = sfptpd_interface_ioctl(interface, SIOCEFX, &req);
		if (rc == 0) {
			interface->clock_supports_phc = false;
			interface->ts_info = req.u.ts_info;
			interface->driver_supports_efx = true;
			return;
		}

		/* Method 3. If we can't find the timestamping capabilities of
		 * the NIC via ethtool or a private ioctl, try looking for a
		 * ptp_caps file. This is the method supported by older
		 * drivers. */
		TRACE_L4("interface %s: getting timestamping caps via sysfs\n",
			 interface->name);

		if (sysfs_file_exists(sysfs_dir, interface->name, "device/ptp_caps") &&
		    sysfs_read_int(sysfs_dir, interface->name, "ifindex", &if_index)) {
			interface->clock_supports_phc = false;
			interface->ts_info = ts_info_hw_default;
			interface->ts_info.phc_index = if_index;
			interface->driver_supports_efx = true;
			return;
		}
	}

	/* We aren't able to support timestamping on this interface so set the
	 * timestamping info to indicate software only and no PHC support */
	interface->clock_supports_phc = false;
	interface->ts_info = ts_info_sw_only;
}


static void interface_check_efx_support(struct sfptpd_interface *interface)
{
	struct efx_sock_ioctl req;
	int rc;

	assert(interface != NULL);

	if (interface->class == SFPTPD_INTERFACE_SFC &&
	    !interface->driver_supports_efx) {

		memset(&req, 0, sizeof(req));
		req.cmd = EFX_TS_SETTIME;
		req.u.ts_settime.iswrite = 0;

		rc = sfptpd_interface_ioctl(interface, SIOCEFX, &req);
		if (rc != EOPNOTSUPP) {
			interface->driver_supports_efx = true;
		}
	}

	TRACE_L2("interface %s: %s efx ioctl\n",
		 interface->name,
		 interface->driver_supports_efx ? "supports" : "does not support");
}


static int get_config_fd(struct sfptpd_interface *interface, int* fdp)
{
	char filename[128];

	snprintf(filename, sizeof(filename), "/sys/class/net/%s/device/config", interface->name);

	*fdp = open(filename, O_SYNC | O_RDWR);
	if(*fdp < 0) {
		ERROR("Failed to open %s, you may have insufficient permissions.\n", filename);
		return errno;
	}

	if (flock(*fdp, LOCK_EX) < 0) {
		close(*fdp);
		return errno;
	}

	return 0;
}

static int read_pci_config(int fd, unsigned addr, void *ptr, unsigned bytes)
{
	off_t off;
	ssize_t readlen;

	off = lseek(fd, addr, SEEK_SET);
	if (off < 0)
		return errno;
	readlen = read(fd, ptr, bytes);
	if (readlen < 0)
		return errno;

	return 0;
}

static int write_pci_config(int fd, unsigned addr, void *ptr, unsigned bytes)
{
	off_t off;
	ssize_t writelen;

	off = lseek(fd, addr, SEEK_SET);
	if (off < 0)
		return errno;
	writelen = write(fd, ptr, bytes);
	if (writelen < 0)
		return errno;

	return 0;
}

static int find_vpd_offset_in_config(int fd, uint8_t* offset)
{
	int rc = 0;
	uint16_t status;
	uint8_t list_item[2];

	rc = read_pci_config(fd, PCI_STATUS, &status, sizeof(status));
	if (rc)
		goto out;
	if (!(status & PCI_STATUS_CAP_LIST)) {
		rc = EOPNOTSUPP;
		goto out;
	}

	rc = read_pci_config(fd, PCI_CAPABILITY_LIST, offset, 1);
	if (rc)
		goto out;

	do {
		rc = read_pci_config(fd, *offset, list_item, 2);
		if (rc)
			goto out;
		if (list_item[0] == PCI_CAP_ID_VPD)
			return 0;
		*offset = list_item[1];
	} while (*offset != 0);

out:
	return rc;
}

static int interface_get_vpd_info_from_pci(struct sfptpd_interface *interface, uint8_t *vpd_data,
		size_t *vpd_len)
{
	int rc = 0;
	uint8_t vpd_cap, tag;
	uint16_t offset, vpd_ctrl, next_tag_at, len;
	int fd;

	rc = get_config_fd(interface, &fd);
	if (rc)
		return rc;

	rc = find_vpd_offset_in_config(fd, &vpd_cap);
	if (rc)
		goto out;

	next_tag_at = 0;
	for (offset = 0; offset < *vpd_len; offset += 4) {
		unsigned retries = 125; /* This roughly matches the kernel timeout. */
		rc = write_pci_config(fd, vpd_cap + PCI_VPD_ADDR, &offset, sizeof(offset));
		if (rc)
			goto out;
		do {
			usleep(1000);
			rc = read_pci_config(fd, vpd_cap + PCI_VPD_ADDR, &vpd_ctrl, sizeof(vpd_ctrl));
			if (rc)
				goto out;
		} while (!(vpd_ctrl & PCI_VPD_ADDR_F) && retries--);

		if (!(vpd_ctrl & PCI_VPD_ADDR_F)) {
			rc = ETIMEDOUT;
			goto out;
		}

		rc = read_pci_config(fd, vpd_cap + PCI_VPD_DATA, vpd_data + offset, 4);
		if (rc)
			goto out;

		if (offset + 4 > next_tag_at) {
			tag = vpd_data[next_tag_at];

			/* If we have found the end tag then stop reading. */
			if (tag == VPD_TAG_END) {
				offset = next_tag_at + 1;
				break;
			}

			if(tag & VPD_LARGE_TAG_MSK) {
				/* If we have read enough data to contain the length field then process
				 * it now, otherwise keep reading until we have enough data. */
				if (offset + 4 > next_tag_at + 2) {
					len = *(vpd_data + next_tag_at + 1);
					len |= *(vpd_data + next_tag_at + 2) << 8;
					next_tag_at += 3 + len;
				}
			}
			else {
				len = tag & VPD_SMALL_TAG_LEN_MSK;
				next_tag_at += 1 + len;
			}
		}
	}

	*vpd_len = offset;
out:
	flock(fd, LOCK_UN);
	close(fd);
	return rc;
}


static void interface_get_vpd_info_from_sysfs(struct sfptpd_interface *interface,
					      const char *sysfs_dir, uint8_t *vpd_ptr,
					      size_t *vpd_len)
{
	char path[PATH_MAX];
	FILE *file;

	assert(interface != NULL);
	assert(sysfs_dir != NULL);
	assert(vpd_ptr != NULL);

	/* Create the path name of the vital product data file */
	snprintf(path, sizeof(path), "%s%s/device/vpd", sysfs_dir, interface->name);

	file = fopen(path, "r");
	if (file == NULL) {
		TRACE_L3("interface %s: couldn't open %s\n",
			 interface->name, path);
		*vpd_len = 0;
		return;
	}

	/* Just slurp it all up. */
	*vpd_len = fread(vpd_ptr, sizeof(char), *vpd_len, file);
	fclose(file);
}

static void interface_get_vpd_info(struct sfptpd_interface *interface,
				   const char *sysfs_dir,
				   sfptpd_interface_class_t class)
{
	size_t vpd_len = VPD_MAX_SIZE;
	uint8_t *vpd_ptr = malloc(vpd_len);
	if (vpd_ptr == NULL) {
		CRITICAL("Out of memory!\n");
		return;
	}

	int rc = interface_get_vpd_info_from_pci(interface, vpd_ptr, &vpd_len);
	if (rc != 0) {
		TRACE_L3("interface %s: failed to read VPD from PCIe config space (%d), trying sysfs instead\n",
			 interface->name, rc);
		interface_get_vpd_info_from_sysfs(interface, sysfs_dir, vpd_ptr, &vpd_len);
		if (vpd_len == 0) {
			goto fail;
		}
	}

	size_t i = 0;
	unsigned int desc_len, entry_len, idx, state;
	uint8_t tag, keyword[2];
	enum { VPD_TAG, VPD_LEN0, VPD_LEN1, VPD_DATA, VPD_OK };
	enum { VPD_ENTRY_KEYWORD0, VPD_ENTRY_KEYWORD1, VPD_ENTRY_LEN, VPD_ENTRY_DATA };

	/* First find the data section of the read only VPD descriptor */
	state = VPD_TAG;
	while ((state != VPD_OK) && (i < vpd_len)) {
		char c = vpd_ptr[i++];
		switch (state) {
		case VPD_TAG:
			tag = (uint8_t)c;
			/* If we reach the end of the VPD without finding the
			 * read-only descriptor then fail */
			if (tag == VPD_TAG_END) {
				TRACE_L3("interface %s: reached end of VPD",
					 interface->name);
				goto fail;
			} else {
				state = VPD_LEN0;
			}
			break;

		case VPD_LEN0:
			desc_len = (unsigned int)c;
			state = VPD_LEN1;
			break;

		case VPD_LEN1:
			desc_len |= ((unsigned int)c << 8);
			state = VPD_DATA;
			idx = 0;
			/* If we have found the read-only descriptor move to the
			 * next loop to parse the entries */
			if (tag == VPD_TAG_RO)
				state = VPD_OK;
			break;

		case VPD_DATA:
			if (tag == VPD_TAG_STR) {
				if (idx < sizeof(interface->product))
					interface->product[idx] = c;

				idx++;

				if (idx == sizeof(interface->product)) {
					interface->product[idx - 1] = '\0';
					WARNING("interface %s: VPD product name too long (%d)\n",
						interface->name, desc_len);
				}

				if (idx >= desc_len) {
					state = VPD_TAG;
					if (idx < sizeof(interface->product))
						interface->product[idx] = '\0';
					TRACE_L3("interface %s: NIC product name is %s\n",
						 interface->name,
						 interface->product);
				}
			} else {
				idx++;
				if (idx >= desc_len)
					state = VPD_TAG;
			}
			break;
		}
	}

	if (state != VPD_OK)
		goto fail;

	TRACE_L4("interface %s: VPD found read-only descriptor\n", interface->name);

	/* Parse each entry in the descriptor looking for the model and serial
	 * numbers */
	state = VPD_ENTRY_KEYWORD0;
	while ((desc_len != 0) && (i < vpd_len)) {
		char c = vpd_ptr[i++];
		switch (state) {
		case VPD_ENTRY_KEYWORD0:
			keyword[0] = (uint8_t)c;
			state = VPD_ENTRY_KEYWORD1;
			break;

		case VPD_ENTRY_KEYWORD1:
			keyword[1] = (uint8_t)c;
			state = VPD_ENTRY_LEN;
			break;

		case VPD_ENTRY_LEN:
			entry_len = (unsigned int)c;
			idx = 0;
			state = VPD_ENTRY_DATA;
			break;

		case VPD_ENTRY_DATA:
			/* If the entry is the part number or serial number,
			 * store the strings */
			if ((keyword[0] == 'P') && (keyword[1] == 'N')) {
				if (idx < sizeof(interface->model))
					interface->model[idx] = c;

				idx++;

				if (idx == sizeof(interface->model)) {
					interface->model[idx - 1] = '\0';
					WARNING("interface %s: VPD part number too long (%d)\n",
						interface->name, entry_len);
				}

				if (idx >= entry_len) {
					state = VPD_ENTRY_KEYWORD0;
					if (idx < sizeof(interface->model))
						interface->model[idx] = '\0';
					TRACE_L3("interface %s: NIC part number is %s\n",
						 interface->name, interface->model);
				}
			} else if ((keyword[0] == 'S') && (keyword[1] == 'N')) {
				if (idx < sizeof(interface->serial_num))
					interface->serial_num[idx] = c;

				idx++;

				if (idx == sizeof(interface->serial_num)) {
					interface->serial_num[idx - 1] = '\0';
					WARNING("interface %s: VPD serial number too long (%d)\n",
						interface->name, entry_len);
				}

				if (idx >= entry_len) {
					state = VPD_ENTRY_KEYWORD0;
					if (idx < sizeof(interface->serial_num))
						interface->serial_num[idx] = '\0';
					TRACE_L3("interface %s: NIC serial number is %s\n",
						 interface->name, interface->serial_num);
				}
			} else {
				idx++;
				if (idx >= entry_len)
					state = VPD_ENTRY_KEYWORD0;
			}
			break;
		}

		desc_len--;
	}

fail:
	/* Check whether we got the product name, part and serial numbers */
	if (class == SFPTPD_INTERFACE_SFC || class == SFPTPD_INTERFACE_XNET) {
		if (interface->product[0] == '\0')
			WARNING("interface %s: no product name found in VPD\n",
				interface->name);
		if (interface->model[0] == '\0')
			WARNING("interface %s: no part number found in VPD\n",
					interface->name);
		if (interface->serial_num[0] == '\0')
			WARNING("interface %s: no serial number found in VPD\n",
				interface->name);
	}

	free(vpd_ptr);
}


/****************************************************************************
 * Interface Internal Functions
 ****************************************************************************/

static int rescan_interfaces(void)
{
	interface_diagnostics(4);
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
	regmatch_t matches[1];
	regex_t regex;
	int rc;

	/* Extract a portion of the bus address that identifies the NIC,
	   i.e. excluding the PCI function. Failure is not fatal. */
	rc = regcomp(&regex, "([[:xdigit:]:]+)", REG_EXTENDED);
	if (rc != 0) {
		TRACE_L3("interface %s: regcomp() failed\n",
			 interface->name);
	} else {
		rc = regexec(&regex, interface->bus_addr,
			     sizeof matches / sizeof *matches,
			     matches, 0);
		if (rc == 0 &&
		    matches[0].rm_so != -1) {
			/* Assumes destination already zeroed. */
			strncpy(interface->bus_addr_nic,
				interface->bus_addr + matches[0].rm_so,
				matches[0].rm_eo - matches[0].rm_so);
		}
	}

	/* If there is no PHC number then there is no purpose for the
	   NIC number so leave as the -1 it is initialised to. */
	if (interface->ts_info.phc_index != -1) {
		struct sfptpd_interface *other_intf;
		int my_false = 0;

		/* First find any LIVE interfaces with the same clock id.

		   If we have found another *live* with the same clock id
		   then we are from the same NIC otherwise the OS would have
		   just assigned us a different clock id.
		*/
		other_intf = FIND_ANY(INTF_KEY_CLOCK, &interface->ts_info.phc_index,
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
			other_intf = FIND_ANY(INTF_KEY_MAC, &interface->mac_addr);
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

static int interface_init(const char *name, const char *sysfs_dir,
			  struct sfptpd_interface *interface,
			  sfptpd_interface_class_t class,
			  int if_index)
{
	int rc;

	assert(name != NULL);
	assert(sysfs_dir != NULL);
	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	interface->ts_enabled = false;
	interface->class = class;

	/* Default to system clock */
	sfptpd_interface_set_clock(interface, sfptpd_clock_get_system_clock());

	/* Take a copy of the interface name */
	sfptpd_strncpy(interface->name, name, sizeof(interface->name));

	/* Get the ifindex for this interface */
	if (if_index < 0) {
		if (!sysfs_read_int(sysfs_dir, name, "ifindex", &if_index)) {
			ERROR("interface %s: couldn't read sysfs ifindex file\n", name);
			return EINVAL;
		}
	}

	interface->if_index = if_index;
	interface->deleted = false;
	interface->suitable = true;

	/* Get the permanent hardware address of the interface */
	rc = interface_get_hw_address(interface);
	if (rc != 0) {
		ERROR("interface %s: couldn't get hardware address\n", name);
		return rc;
	}

	/* Get the PCI IDs */
	interface_get_pci_ids(interface, sysfs_dir);

	/* Get the driver and firmware versions */
	interface_get_versions(interface);

	/* Get the timestamping capabilities of the interface */
	interface_get_ts_info(interface, sysfs_dir);

	/* Check whether the driver supports the EFX ioctl */
	interface_check_efx_support(interface);

	/* Assign NIC ID */
	interface_assign_nic_id(interface);

	/* Get the model number and serial number of the NIC */
	interface_get_vpd_info(interface, sysfs_dir, class);

	return 0;
}

static void interface_delete(struct sfptpd_interface *interface,
			     bool disable_timestamping)
{
	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

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


static void interface_diagnostics(int trace_level)
{
	sfptpd_db_table_dump(trace_level, "interfaces", false,
			     sfptpd_interface_table,
			     INTF_KEY_DELETED, &(int){ false },
			     SFPTPD_DB_SEL_ORDER_BY,
			     INTF_KEY_IF_INDEX,
			     SFPTPD_DB_SEL_END);
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


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_interface_initialise(struct sfptpd_config *config, pthread_mutex_t *hardware_state_lock)
{
	struct sfptpd_interface *new, *interface;
	FTS *fts;
	FTSENT *fts_entry;
	int rc, i, flags;
	sfptpd_config_timestamping_t *ts;
	sfptpd_interface_class_t class;
	char * const search_path[] = {SFPTPD_SYSFS_NET_PATH, NULL};

	assert(config != NULL);

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

	/* If we are operating with hotplug detect mode set to manual, we don't
	 * scan any interfaces- the user must add them explicitly using the
	 * sfptpdctl hotpluginsert=ethX command. */
	if ((sfptpd_general_config_get(config)->hotplug_detection & SFPTPD_HOTPLUG_DETECTION_INITIAL_SCAN) == 0) {
		INFO("hotplug detection set to manual - not scanning interfaces\n");
		return 0;
	}

	/* Iterate through the interfaces in the system */
	fts = fts_open(search_path, FTS_COMFOLLOW, NULL);
	if (fts == NULL) {
		CRITICAL("failed to open sysfs net devices directory, %s\n",
			 strerror(errno));
		sfptpd_interface_shutdown(config);
		return errno;
	}

	fts_entry = fts_read(fts);
	if (fts_entry == NULL) {
		CRITICAL("failed to read sysfs directory, %s\n",
			 strerror(errno));
		fts_close(fts);
		sfptpd_interface_shutdown(config);
		return errno;
	}

	fts_entry = fts_children(fts, 0);
	if (fts_entry == NULL) {
		CRITICAL("failed to get sysfs directory listing, %s\n",
			 strerror(errno));
		fts_close(fts);
		sfptpd_interface_shutdown(config);
		return errno;
	}

	/* Iterate through the linked list of files within the directory... */
	for ( ; fts_entry != NULL; fts_entry = fts_entry->fts_link) {

		/* Check that the interface is suitable i.e. an ethernet device
		 * that isn't wireless or a bridge or virtual etc */
		if (!interface_check_suitability(fts_entry->fts_path,
						 fts_entry->fts_name, &class))
			continue;

		/* Create a new interface */
		rc = interface_alloc(&new);
		if (rc != 0) {
			ERROR("failed to allocate interface object for %s, %s\n",
			      fts_entry->fts_name, strerror(rc));
			fts_close(fts);
			return rc;
		}

		rc = interface_init(fts_entry->fts_name, fts_entry->fts_path, new, class, -1);
		if (rc != 0) {
			if (rc == ENOTSUP || rc == EOPNOTSUPP) {
				WARNING("skipping over insufficiently capable interface %s\n",
					fts_entry->fts_name);
				interface_free(new);
				continue;
			} else {
				ERROR("failed to create interface instance for %s, %s\n",
				      fts_entry->fts_name, strerror(rc));
				fts_close(fts);
				return rc;
			}
		}

		/* If this is one of our adapters then check the firmware
		 * version and driver version and warn the user if too old.
		 * Note that we do this whether or not the NIC appears to be
		 * PTP-capable because the early versions of the driver do not
		 * include the PTP capability indicator in sysfs (see bug
		 * 34445) */
		if (class == SFPTPD_INTERFACE_SFC)
			(void)interface_check_versions(new);

		if (!interface_is_ptp_capable(new->name, &new->ts_info))
			TRACE_L3("interface %s: not PTP capable\n",
				 new->name);
		else
			TRACE_L1("interface %s: PTP capable, clock idx %d\n",
				 new->name, new->ts_info.phc_index);

		/* Add the interface to the database */
		sfptpd_db_table_insert(sfptpd_interface_table, &new);

		rescan_interfaces();
	}

	fts_close(fts);

	fixup_readonly_and_clock_lists();

	/* For each interface specified in the config file, enable packet timestamping */
	ts = &sfptpd_general_config_get(config)->timestamping;
	if (ts->all) {
		int rc = 0;
		void fn(void *record, void *rcp) {
			struct sfptpd_interface *interface = *((struct sfptpd_interface **) record);
			if (sfptpd_interface_rx_ts_caps(interface) & SFPTPD_INTERFACE_TS_CAPS_HW) {
				int local_rc = sfptpd_interface_hw_timestamping_enable(interface);
				if (local_rc != 0)
					*((int *) rcp) = local_rc;
			}
		}
		sfptpd_db_table_foreach(sfptpd_interface_table, fn, &rc);
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
	interface_diagnostics(3);
	sfptpd_clock_diagnostics(3);

	return 0;
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
	interface_free(interface);
}


void sfptpd_interface_shutdown(struct sfptpd_config *config)
{
	bool disable_on_exit = sfptpd_general_config_get(config)->timestamping.disable_on_exit;

	/* This happens if main exits before calling our initialise. */
	if (sfptpd_interface_socket == -1) return;

	interface_lock();
	interface_diagnostics(4);

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


int sfptpd_interface_hotplug_insert(int if_index, const char *if_name) {
	struct sfptpd_interface *interface;
	int rc = 0;
	sfptpd_interface_class_t class;

	interface_lock();

	interface = interface_find_by_if_index(if_index);
	if (interface == NULL) {
		interface = interface_find_by_name(if_name);

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
		struct sfptpd_interface *other;

		INFO("interface: handling detected rename: %s -> %s (if_index %d)\n",
		     interface->name, if_name, if_index);

		other = interface_find_by_name(if_name);
		if (other != NULL && other->deleted) {
			TRACE_L3("interface: aliasing deleted interface %d to %d\n",
				 other->if_index, if_index);
			other->name[0] = '\0';
			other->canonical = interface;
		} else if (other != NULL) {
			CRITICAL("interface: cannot process insertion of interface %s (%d) while undeleted interface of the same name (%d) still exists\n",
				 if_name, if_index, interface->if_index);
			rc = EINVAL;
			goto finish;
		}
	} else {
		INFO("interface: handling detected changes: %s (if_index %d)\n",
		     if_name, if_index);
	}

	/* Check that the interface is suitable i.e. an ethernet device
	 * that isn't wireless or a bridge or virtual etc */
	if (!interface_check_suitability(SFPTPD_SYSFS_NET_PATH, if_name, &class)) {
		TRACE_L4("interface: ignoring interface %s of irrelevant type\n", if_name);
		sfptpd_strncpy(interface->name, if_name, sizeof(interface->name));
		interface->if_index = if_index;
		interface_delete(interface, false);
		rescan_interfaces();
		goto finish;
	}

	rc = interface_init(if_name, SFPTPD_SYSFS_NET_PATH, interface, class, if_index);

	rescan_interfaces();

	/* Now that we have configured the clock's readonly flag, we can finally apply frequency correction,
	   stepping etc.
	*/
	struct sfptpd_clock* clock = sfptpd_interface_get_clock(interface);
	sfptpd_clock_correct_new(clock);

	if (rc == ENOTSUP || rc == EOPNOTSUPP) {
		INFO("skipped over insufficiently capable interface %s\n", if_name);
		goto finish;
	} else if (rc != 0) {
		ERROR("failed to create interface instance for %s, %s\n",
		      if_name, strerror(rc));
		goto finish;
	}

	if (class == SFPTPD_INTERFACE_SFC)
		(void)interface_check_versions(interface);

	if (!interface_is_ptp_capable(interface->name, &interface->ts_info))
		TRACE_L3("interface %s: not PTP capable\n",
			 interface->name);
	else
		TRACE_L1("interface %s: PTP capable, clock idx %d\n",
			 interface->name, interface->ts_info.phc_index);

 finish:
	interface_unlock();
	return rc;
}


int sfptpd_interface_hotplug_remove(int if_index, const char *if_name) {
	struct sfptpd_interface *interface;
	int rc = 0;

	interface_lock();

	INFO("interface: hotplug remove for %s (%d)\n", if_name, if_index);

	/* If the ifindex has been provided, find the interface by index. If not,
	 * try to find the interface by name. */
	if (if_index >= 0) {
		interface = interface_find_by_if_index(if_index);
	} else {
		interface = interface_find_by_name(if_name);
	}

	if (interface == NULL) {
		WARNING("interface: could not find interface to be deleted\n");
		rc = ENOENT;
	} else {
		if (interface->deleted) {
			WARNING("interface: interface %s already deleted\n", if_name);
			rc = ENOENT;
		} else {
			interface_delete(interface, false);
			rescan_interfaces();
		}
	}

	interface_unlock();
	return rc;
}


/****************************************************************************/


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


const char *sfptpd_interface_get_product_name(struct sfptpd_interface *interface)
{
	const char *product;

	if (interface_get_canonical_with_lock(&interface)) {
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
		product = interface->product;
		interface_unlock();
	} else {
		product = "(no-product-name)";
	}
	return product;
}


const char *sfptpd_interface_get_serial_no(struct sfptpd_interface *interface)
{
	const char *serial_num;

	if (interface_get_canonical_with_lock(&interface)) {
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
		serial_num = interface->serial_num;
		interface_unlock();
	} else {
		serial_num = "(no-serial-num)";
	}
	return serial_num;
}


const char *sfptpd_interface_get_model(struct sfptpd_interface *interface)
{
	const char *model;

	if (interface_get_canonical_with_lock(&interface)) {
		assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
		model = interface->model;
		interface_unlock();
	} else {
		model = "(no-model)";
	}
	return model;
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


bool sfptpd_interface_is_siena(struct sfptpd_interface *interface)
{
	bool is_siena;
	if (!interface_get_canonical_with_lock(&interface))
		return false;
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	/* Is this the PTP port of a SFN5322F or SFN6322F adapter? */
	is_siena = (interface->class == SFPTPD_INTERFACE_SFC &&
		    (interface->pci_device_id == SFPTPD_SIENA_DEVID));
	interface_unlock();
	return is_siena;
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
	struct ifreq ifr;
	int rc = 0;

	assert(link_detected != NULL);
	*link_detected = false;

	if (!interface_get_canonical_with_lock(&interface))
		return rc;
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	/* Set up the ifrequest structure with the interface name */
	memset(&ifr, 0, sizeof(ifr));
	sfptpd_strncpy(ifr.ifr_name, interface->name, sizeof(interface->name));

	if (ioctl(sfptpd_interface_socket, SIOCGIFFLAGS, &ifr) >= 0) {
		*link_detected = !!(ifr.ifr_flags & IFF_UP);
	} else {
		ERROR("interface %s: SIOCGIFFLAGS error %s\n",
		      interface->name, strerror(errno));
		rc = errno;
	}

	interface_unlock();
	return rc;
}


/****************************************************************************/

struct sfptpd_db_query_result sfptpd_interface_get_all_snapshot(void)
{
	return sfptpd_db_table_query(sfptpd_interface_table,
				     SFPTPD_DB_SEL_ORDER_BY,
				     INTF_KEY_NIC,
				     INTF_KEY_MAC);
}


struct sfptpd_db_query_result sfptpd_interface_get_active_ptp_snapshot(void)
{
	return sfptpd_db_table_query(sfptpd_interface_table,
				     INTF_KEY_DELETED, &(int){ false },
				     INTF_KEY_PTP, &(int){ true },
				     SFPTPD_DB_SEL_ORDER_BY,
				     INTF_KEY_NIC,
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

	if (0 == strcmp(ifr.ifr_name, interface->name)) {
		return 0;
	}

	INFO("interface %s: hotplug changed name during ioctl -> %s (%d)\n",
	     interface->name, ifr.ifr_name, interface->if_index);

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
	sfptpd_strncpy(ifr.ifr_name, interface->name, sizeof(interface->name));
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

			/* If we get EBUSY, retry a few times to avoid hitting bug58245.
			 * On SFC NICs, enabling hw timestamping requires a clock sync op,
			 * which in turn relies on the system not being overloaded. This
			 * can be especially problematic at system startup, i.e. now. */
			if (rc == EBUSY) {
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
		ERROR("interface: can't disable timestamping on missing interface\n");
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


int sfptpd_interface_ptp_set_vlan_filter(struct sfptpd_interface *interface,
					 unsigned int num_vlan_tags,
					 uint16_t vlan_tags[])
{
	struct efx_sock_ioctl req;
	int i, rc;

	if (!interface_get_canonical_with_lock(&interface)) {
		ERROR("interface: can't set vlan filter on missing interface\n");
		return ENOENT;
	}

	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	assert((num_vlan_tags == 0) || (vlan_tags != NULL));
	assert(num_vlan_tags < SFPTPD_MAX_VLAN_TAGS);

	/* We should not be calling this function on interfaces that support
	 * hardware receive packet timestamping */
	assert((sfptpd_interface_rx_ts_caps(interface) & SFPTPD_INTERFACE_TS_CAPS_HW) == 0);

	memset(&req, 0, sizeof(req));
	req.cmd = EFX_TS_SET_VLAN_FILTER;
	req.u.ts_vlan_filter.num_vlan_tags = num_vlan_tags;
	/* The order of the VLAN tags needs to be reversed to correctly
	 * match the ethernet packet structure - for a VLAN interface
	 * of the form etha.b.c.d, the tags appear in the packet in the
	 * order a, b, c, d. */
	for (i = 0; i < num_vlan_tags; i++) {
		req.u.ts_vlan_filter.vlan_tags[i] =
			vlan_tags[num_vlan_tags - 1 - i];
	}

	rc = sfptpd_interface_ioctl(interface, SIOCEFX, &req);
	if (rc == 0) {
		TRACE_L2("interface %s: set VLAN filter for %d tags\n",
			 interface->name, num_vlan_tags);
	} else {
		ERROR("interface %s: failed to set PTP VLAN filter, %s\n",
		      interface->name, strerror(rc));
	}

	interface_unlock();
	return rc;
}


int sfptpd_interface_ptp_set_uuid_filter(struct sfptpd_interface *interface,
					 bool enable, uint8_t uuid[])
{
	struct efx_sock_ioctl req;
	int rc;
	
	if (!interface_get_canonical_with_lock(&interface)) {
		ERROR("interface: can't set uuid filter on missing interface\n");
		return ENOENT;
	}

	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);
	assert(!enable || (uuid != NULL));

	/* We should not be calling this function on interfaces that support
	 * hardware receive packet timestamping */
	assert((sfptpd_interface_rx_ts_caps(interface) & SFPTPD_INTERFACE_TS_CAPS_HW) == 0);

	memset(&req, 0, sizeof(req));
	req.cmd = EFX_TS_SET_UUID_FILTER;
	req.u.ts_uuid_filter.enable = enable;
	if (enable) {
		memcpy(req.u.ts_uuid_filter.uuid, uuid,
		       sizeof(req.u.ts_uuid_filter.uuid));
	}

	rc = sfptpd_interface_ioctl(interface, SIOCEFX, &req);
	if (rc == 0) {
		TRACE_L2("interface %s: %s UUID filter\n",
			 interface->name, enable? "enabled": "disabled");
	} else {
		ERROR("interface %s: failed to set PTP UUID filter, %s\n",
		      interface->name, strerror(rc));
	}

	interface_unlock();
	return rc;
}


int sfptpd_interface_ptp_set_domain_filter(struct sfptpd_interface *interface,
					   bool enable, uint8_t domain)
{
	struct efx_sock_ioctl req;
	int rc;

	if (!interface_get_canonical_with_lock(&interface)) {
		ERROR("interface: can't set domain filter on missing interface\n");
		return ENOENT;
	}

	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	/* We should not be calling this function on interfaces that support
	 * hardware receive packet timestamping */
	assert((sfptpd_interface_rx_ts_caps(interface) & SFPTPD_INTERFACE_TS_CAPS_HW) == 0);

	memset(&req, 0, sizeof(req));
	req.cmd = EFX_TS_SET_DOMAIN_FILTER;
	req.u.ts_domain_filter.enable = enable;
	req.u.ts_domain_filter.domain = domain;

	rc = sfptpd_interface_ioctl(interface, SIOCEFX, &req);
	if (rc == 0) {
		TRACE_L2("interface %s: %s domain filter\n",
			 interface->name, enable? "enabled": "disabled");
	} else {
		ERROR("interface %s: failed to set PTP Domain filter, %s\n",
		      interface->name, strerror(rc));
	}

	interface_unlock();
	return rc;
}

bool sfptpd_interface_get_sysfs_max_freq_adj(struct sfptpd_interface *interface,
					     int *max_freq_adj)
{
	bool rc;

	if (!interface_get_canonical_with_lock(&interface)) {
		ERROR("interface: can't read sysfs maximum frequency adjustment "
		      "on missing interface\n");
		return false;
	}

	assert(interface != NULL);
	assert(interface->magic == SFPTPD_INTERFACE_MAGIC);

	/* We should only be calling this function on SFC interfaces where we can
	 * guarantee the sysfs file content will be formatted as expected */
	assert(interface->class == SFPTPD_INTERFACE_SFC);

	rc =  sysfs_read_int(SFPTPD_SYSFS_NET_PATH, interface->name,
			     "device/max_adjfreq", max_freq_adj);
	interface_unlock();
	return rc;
}


/* fin */
