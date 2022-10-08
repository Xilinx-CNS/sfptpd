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
	uint8_t addr[ETH_ALEN];
} sfptpd_mac_addr_t;


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
 * @return 0 for success or an errno on failure.
 * @param hardware_state_lock Pointer to shared hardware mutex
 */
int sfptpd_interface_initialise(struct sfptpd_config *config, pthread_mutex_t *hardware_state_lock);

/** Release resources associated with the interface subcomponent
 * @param config Pointer to configuration structure
 */
void sfptpd_interface_shutdown(struct sfptpd_config *config);

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

/** Get the product name of the NIC providing an interface
 * @param interface  Pointer to interface instance
 * @return The product name of the NIC or an empty string if not known.
 */
const char *sfptpd_interface_get_product_name(struct sfptpd_interface *interface);

/** Get the serial number of the NIC providing an interface
 * @param interface  Pointer to interface instance
 * @return The serial number of the NIC or an empty string if not known.
 */
const char *sfptpd_interface_get_serial_no(struct sfptpd_interface *interface);

/** Get the model number of the NIC providing an interface
 * @param interface  Pointer to interface instance
 * @return The model number of the NIC or an empty string if not known.
 */
const char *sfptpd_interface_get_model(struct sfptpd_interface *interface);

/** Get the firmware version of the NIC providing an interface
 * @param interface  Pointer to interface instance
 * @return The firmware version of the NIC or NULL in the case of an error.
 */
const char *sfptpd_interface_get_fw_version(struct sfptpd_interface *interface);

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

/** Is this Solarflare adapter using a Siena chipset
 * @param interface Pointer to instance instance
 * @return True if Solarflare Siena chipset
 */
bool sfptpd_interface_is_siena(struct sfptpd_interface *interface);

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

/** Set the VLAN tags for filtering of received timestamped PTP packets
 * @param interface Pointer to the interface instance
 * @param num_vlan_tags Number of VLAN tags
 * @param vlan_tags Array of VLAN tags
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_interface_ptp_set_vlan_filter(struct sfptpd_interface *interface,
					 unsigned int num_vlan_tags,
					 uint16_t vlan_tags[]);

/** Set the PTP UUID for filtering of received timestamped PTP packets
 * @param interface Pointer to the interface instance
 * @param uuid_enable Boolean indicating whether to enable the filter
 * @param uuid UUID to filter against
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_interface_ptp_set_uuid_filter(struct sfptpd_interface *interface,
					 bool enable, uint8_t uuid[]);

/** Set the PTP Domain number for filtering of received timestamped PTP packets
 * @param interface Pointer to the interface instance
 * @param enable Boolean indicating whether to enable the filter
 * @param domain Domain number to filter against
 * @return 0 for success otherwise an errno status code.
 */
int sfptpd_interface_ptp_set_domain_filter(struct sfptpd_interface *interface,
					   bool enable, uint8_t domain);

/** Triggers handling by the interface module of a netlink
 * interface hotplug event designating the insertion or modification
 * of a logical interface.
 * @param if_index the OS index for this interface
 * @param if_name the newest name for this interface
 * @return 0 on success else an error from errno.h
 */
int sfptpd_interface_hotplug_insert(int if_index, const char *if_name);

/** Triggers handling by the interface module of a netlink
 * interface hotplug event designating the removal
 * of a logical interface.
 * @param if_index the OS index for this interface
 * @param if_name the last name for this interface
 * @return 0 on success else an error from errno.h
 */
int sfptpd_interface_hotplug_remove(int if_index, const char *if_name);

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


#endif /* _SFPTPD_INTERFACE_H */
