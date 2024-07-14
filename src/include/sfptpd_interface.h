/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2021 Xilinx, Inc. */

#ifndef _SFPTPD_INTERFACE_H
#define _SFPTPD_INTERFACE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <net/ethernet.h>
#include <linux/if_ether.h>

#include "sfptpd_constants.h"
#include "sfptpd_statistics.h"
#include "sfptpd_link.h"
#include "sfptpd_db.h"


/** Forward declaration of structures */
struct sfptpd_clock;
struct sfptpd_interface;
struct sfptpd_config;


/****************************************************************************
 * Structures, Types, Defines
 ****************************************************************************/

typedef enum {
	SFPTPD_INTERFACE_SFC,
	SFPTPD_INTERFACE_XNET,
	SFPTPD_INTERFACE_OTHER,
} sfptpd_interface_class_t;


/** Structure to hold interface hardware address */
typedef struct {
	size_t len;
	uint8_t addr[SFPTPD_L2ADDR_MAX];
} __attribute__ ((packed)) sfptpd_mac_addr_t;


/** Bitmask of interface timestamping capabilities */
typedef uint32_t sfptpd_interface_ts_caps_t;
#define SFPTPD_INTERFACE_TS_CAPS_SW (1<<0)
#define SFPTPD_INTERFACE_TS_CAPS_HW (1<<1)
#define SFPTPD_INTERFACE_TS_CAPS_ALL (3)


/****************************************************************************
 * Function Prototypes
****************************************************************************/

/** Initialise interface subcomponent.
 * @param config Pointer to configuration structure
 * @param hardware_state_lock Pointer to shared hardware mutex
 * @param link_table Pointer to link table from netlink
 * @return 0 for success or an errno on failure.
 */
int sfptpd_interface_initialise(struct sfptpd_config *config,
				pthread_mutex_t *hardware_state_lock,
				const struct sfptpd_link_table *link_table);

/** Release resources associated with the interface subcomponent
 * @param config Pointer to configuration structure
 */
void sfptpd_interface_shutdown(struct sfptpd_config *config);

/** Find an interface by if_index
 * @param if_index  The if_index for an interface
 * @return A pointer to the interface or NULL if not found.
 */
struct sfptpd_interface *sfptpd_interface_find_by_if_index(int if_index);

/** Find an interface by name 
 * @param name  Textual name of interface
 * @return A pointer to the interface or NULL if not found.
 */
struct sfptpd_interface *sfptpd_interface_find_by_name(const char *name);

/** Find an interface by MAC address
 * @param mac Pointer to MAC address
 * @return A pointer to the interface instance or NULL if not found.
 */
struct sfptpd_interface *sfptpd_interface_find_by_mac_addr(sfptpd_mac_addr_t *mac);

/** Get the name of an interface
 * @param interface  Pointer to interface instance
 * @return The name of the interface or NULL in the case of an error.
 */
const char *sfptpd_interface_get_name(struct sfptpd_interface *interface);

/** Get the permanent MAC address of the interface. This is guaranteed to
 * remain constant and unique for each interface.
 * @param interface Pointer to interface instance
 * @return The MAC address of the interface.
 */
void sfptpd_interface_get_mac_addr(struct sfptpd_interface *interface,
				   sfptpd_mac_addr_t *mac);

/** Get the firmware version of the NIC providing an interface
 * @param interface  Pointer to interface instance
 * @return The firmware version of the NIC or NULL in the case of an error.
 */
const char *sfptpd_interface_get_fw_version(struct sfptpd_interface *interface);

/** Get the ifindex of a given interface
 * @param interface Pointer to interface instance
 * @return The ifindex of the provided interface, or 0 on error.
 */
int sfptpd_interface_get_ifindex(struct sfptpd_interface *interface);

/** Set the clock associated with an interface
 * @param interface Pointer to interface instance
 * @param clock Clock to be associated with this interface
 */
void sfptpd_interface_set_clock(struct sfptpd_interface *interface,
				struct sfptpd_clock *clock);

/** Get the clock associated with an interface
 * @param interface Pointer to interface instance
 * @return A pointer to the clock instance or NULL in the case of an error.
 */
struct sfptpd_clock *sfptpd_interface_get_clock(struct sfptpd_interface *interface);

/** Get the clock device index for the clock associated with this interface
 * @param interface Pointer to interface instance
 * @param supports_phc Returned value - supports PHC
 * @param device_idx Returned unique clock device index
 * @param supports_efx Returned value - supports EFX ioctl
 */
void sfptpd_interface_get_clock_device_idx(const struct sfptpd_interface *interface,
					   bool *supports_phc, int *device_idx,
					   bool *supports_efx);

/** Notify that PHC operations do not succeed so should be avoided. The
 * returned device index is used as a pseudo clock identifier enabling
 * operation with only the private EFX ioctl functionality.
 * @param interface Pointer to interface instatnce
 * @return a substitute device index for this clock
 */
int sfptpd_interface_phc_unavailable(struct sfptpd_interface *interface);

/** Get the class of the interface, whether it is SFC, Xilinx (X3NET+) or
    a third party NIC.
 * @param interface Pointer to interface instance
 * @return class of NIC
 */
sfptpd_interface_class_t sfptpd_interface_get_class(struct sfptpd_interface *interface);

/** Checks what PTP capabilities the interface has
 * @param interface Pointer to interface instance
 * @return Bitmask of PTP timestamping capabilities
 */
sfptpd_interface_ts_caps_t sfptpd_interface_ptp_caps(struct sfptpd_interface *interface);

/** Gets ethtool ts_info structure for interface
 * @param interface Pointer to interface instance
 * @return Timestamp information structure
 */
void sfptpd_interface_get_ts_info(const struct sfptpd_interface *interface,
				  struct ethtool_ts_info *ts_info);

/** Checks if the interface has hardware PTP support
 * @param interface Pointer to interface instance
 * @return Boolean indicating whether PTP is supported
 */
bool sfptpd_interface_supports_ptp(struct sfptpd_interface *interface);

/** Checks what receive timestamping capabilities (as opposed to PTP packets
 * only) the interface has
 * @param interface Pointer to interface instance
 * @return Boolean indicating whether general receive timestamping is supported
 */
sfptpd_interface_ts_caps_t sfptpd_interface_rx_ts_caps(struct sfptpd_interface *interface);

/** Checks if the interface supports PPS
 * @param interface Pointer to interface instance
 * @return Boolean indicating whether PPS is supported
 */
bool sfptpd_interface_supports_pps(struct sfptpd_interface *interface);

/** Test link status of interface
 * @param interface Pointer to interface instance
 * @param link_detected Returned boolean indicating whether interface has link
 * up
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_interface_is_link_detected(struct sfptpd_interface *interface,
				      bool *link_detected);

/** Iterate through list of interfaces. Get first interface.
 * @return A pointer to the first interface in the list or NULL if list empty.
 */
struct sfptpd_interface *sfptpd_interface_first(void);

/** Iterate through list of interfaces. Get next interface.
 * @param interface Pointer to current list member
 * @return A pointer to the next interface in the list or NULL if end reached.
 */
struct sfptpd_interface *sfptpd_interface_next(struct sfptpd_interface *interface);


/** Carry out an ioctl operation on an interface
 * @param interface Pointer to the interface instance
 * @param request Request code for the ioctl
 * @param data Data associated with the ioctl operation
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_interface_ioctl(struct sfptpd_interface *interface,
			   int request, void *data);

/** Enable hardware packet timestamping on an interface
 * @param interface Pointer to the interface instance
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_interface_hw_timestamping_enable(struct sfptpd_interface *interface);

/** Disable hardware packet timestamping on an interface
 * @param interface Pointer to the interface instancestruct sfptpd_clock *clock,
 */
void sfptpd_interface_hw_timestamping_disable(struct sfptpd_interface *interface);

/** Triggers handling by the interface module of a netlink
 * interface hotplug event designating the insertion or modification
 * of a logical interface.
 * @param link link object for the new or modified interface
 * @return 0 on success else an error from errno.h
 */
int sfptpd_interface_hotplug_insert(const struct sfptpd_link *link);

/** Triggers handling by the interface module of a netlink
 * interface hotplug event designating the removal
 * of a logical interface.
 * @param link link object for the removed interface
 * @return 0 on success else an error from errno.h
 */
int sfptpd_interface_hotplug_remove(const struct sfptpd_link *link);

/** Take a snapshot of an index over the interfaces list.
 * The copied index is an array of pointers to interface objects.
 * The user must call the corresponding free function when the snapshot is finished with.
 * @param type the index type
 * @param dest a structure that will be updated with a pointer to the newly-allocated
 * copy of the index and the size of the index.
 * @return 0 for success otherwise an errno status code.
 */
struct sfptpd_db_query_result sfptpd_interface_get_all_snapshot(void);
struct sfptpd_db_query_result sfptpd_interface_get_active_ptp_snapshot(void);


/** Get whether the interface object passed in has been 'deleted', e.g.
 * it has been removed from the system and therefore not available for
 * use by the daemon.
 * @param dest pointer to the interface object
 * @return true if the interface is deleted, otherwise false
 */
bool sfptpd_interface_is_deleted(struct sfptpd_interface *interface);

/** Get the permanently-unique identifier for the nic on which the supplied
 * interface resides.
 * @para interface pointer to interface object to check.
 * @return The nic id
 */
int sfptpd_interface_get_nic_id(struct sfptpd_interface *interface);

/** Get the permanent MAC address of the interface. This is guaranteed to
 * remain constant and unique for each interface.
 * @param interface Pointer to interface instance
 * @return The MAC address of the interface.
 */
const char *sfptpd_interface_get_mac_string(struct sfptpd_interface *interface);

/** Get the stratum of the interface's clock.
 * @param interface Pointer to interface instance
 * @return The clock stratum for the interface.
 */
enum sfptpd_clock_stratum sfptpd_interface_get_clock_stratum(struct sfptpd_interface *interface);

/** Find the first interface with the given nic id, i.e. the one
 * with the lowest MAC address.
 * @param nic_id the nic id for which to look
 * @return The interface found else NULL.
 */
struct sfptpd_interface *sfptpd_interface_find_first_by_nic(int nic_id);

/** Check whether any of the names of the interfaces associated with a clock
 * match a supplied string.
 * @param phc_index the phc index for which to look
 * @param cfg_name the supplied string
 * @return Whether the supplied string matched any of the clock's interfaces
 */
bool sfptpd_check_clock_interfaces(const int phc_index, const char* cfg_name);

/** Read the maximum frequency adjust value advertised by an interface for
 * its clock(s).
 * @param interface Pointer to interface instance
 * @param max_freq_adj variable containing value of max_freq_adj from sysfs file
 * @return	true	- successfully updated max_freq_adj with value in file
 * 		false	- failure
 */
bool sfptpd_interface_get_sysfs_max_freq_adj(struct sfptpd_interface *interface,
					     int *max_freq_adj);

/** Read driver stats for an interface.
 * If an error occurs trying to read any of the statistics, other statistics
 * may be updated, resulting in a partial update of the given array.
 * @param interface Pointer to interface instance
 * @param stats array to store driver stats in
 * @return 0 on success, otherwise errno.
 */
int sfptpd_interface_driver_stats_read(struct sfptpd_interface *interface,
				       uint64_t stats[SFPTPD_DRVSTAT_MAX]);

/** Reset driver stats for an interface.
 * @param interface Pointer to interface instance
 * @return 0 on success, otherwise errno.
 */
int sfptpd_interface_driver_stats_reset(struct sfptpd_interface *interface);

/** Dump list of interfaces to logs
 * @param trace_level the level of trace
 */
void sfptpd_interface_diagnostics(int trace_level);

/** Reassign interface to another one
 * @param from_phc phc index of interface to change
 * @param to_phc phc index to use
 * @return 0 on success, else errno
 */
int sfptpd_interface_reassign_to_nic(int from_phc, int to_phc);

#endif /* _SFPTPD_INTERFACE_H */
