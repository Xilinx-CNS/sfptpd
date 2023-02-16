/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2005-2019 Xilinx, Inc. */

#ifndef EFX_IOCTL_H
#define EFX_IOCTL_H

#if defined(__KERNEL__)
#include <linux/if.h>
#else
#include <net/if.h>
#ifndef _LINUX_IF_H
#define _LINUX_IF_H /* prevent <linux/if.h> from conflicting with <net/if.h> */
#endif
#endif
#include <linux/if_ether.h>
#include <linux/sockios.h>
#include <linux/types.h>

/* Old versions of <linux/ethtool.h> use the kernel type names (no
 * '__' prefix).  To avoid conflicting with other workarounds using
 * typedefs, temporarily define macros for them. */
#define u8 __u8
#define u16 __u16
#define u32 __u32
#define u64 __u64
#include <linux/ethtool.h>
#undef u8
#undef u16
#undef u32
#undef u64

/**
 * DOC: sfc driver private ioctl
 *
 * Various driver features can be controlled through a private ioctl,
 * which has multiple sub-commands.
 *
 * Most of these features are also available through the ethtool API
 * or other standard kernel API on a sufficiently recent kernel
 * version.  Userland tools should generally use the standard API
 * first and fall back to the private ioctl in case of an error code
 * indicating the standard API is not implemented (e.g. %EOPNOTSUPP,
 * %ENOSYS, or %ENOTTY).
 *
 * A few features are intended for driver debugging and are not
 * included in the production driver.
 *
 * The private ioctl is numbered %SIOCEFX and is implemented on
 * both sockets and a char device (/dev/sfc_control).  Sockets are
 * more reliable as they do not depend on a device node being
 * created on disk.  However, on VMware ESX only char ioctls will
 * work.
 */

/* Efx private ioctl number */
/* We do not use the first 3 private ioctls because some utilities expect
 * them to be the old MDIO ioctls. */
#define SIOCEFX (SIOCDEVPRIVATE + 3)

/*
 * Efx private ioctls
 */

#ifdef EFX_NOT_EXPORTED

#include "enum.h"

#endif /* EFX_NOT_EXPORTED */

/* PTP support for NIC time disciplining ************************************/

struct efx_timespec {
	__s64	tv_sec;
	__s32	tv_nsec;
};

#if !defined(__KERNEL__) || defined(__VMKLNX__)

enum {
	SOF_TIMESTAMPING_TX_HARDWARE = (1<<0),
	SOF_TIMESTAMPING_TX_SOFTWARE = (1<<1),
	SOF_TIMESTAMPING_RX_HARDWARE = (1<<2),
	SOF_TIMESTAMPING_RX_SOFTWARE = (1<<3),
	SOF_TIMESTAMPING_SOFTWARE = (1<<4),
	SOF_TIMESTAMPING_SYS_HARDWARE = (1<<5),
	SOF_TIMESTAMPING_RAW_HARDWARE = (1<<6),
	SOF_TIMESTAMPING_MASK =
	(SOF_TIMESTAMPING_RAW_HARDWARE - 1) |
	SOF_TIMESTAMPING_RAW_HARDWARE
};

enum hwtstamp_tx_types {
	HWTSTAMP_TX_OFF,
	HWTSTAMP_TX_ON,
	HWTSTAMP_TX_ONESTEP_SYNC,
};

enum hwtstamp_rx_filters {
	HWTSTAMP_FILTER_NONE,
	HWTSTAMP_FILTER_ALL,
	HWTSTAMP_FILTER_SOME,
	HWTSTAMP_FILTER_PTP_V1_L4_EVENT,
	HWTSTAMP_FILTER_PTP_V1_L4_SYNC,
	HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ,
	HWTSTAMP_FILTER_PTP_V2_L4_EVENT,
	HWTSTAMP_FILTER_PTP_V2_L4_SYNC,
	HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ,
	HWTSTAMP_FILTER_PTP_V2_L2_EVENT,
	HWTSTAMP_FILTER_PTP_V2_L2_SYNC,
	HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ,
	HWTSTAMP_FILTER_PTP_V2_EVENT,
	HWTSTAMP_FILTER_PTP_V2_SYNC,
	HWTSTAMP_FILTER_PTP_V2_DELAY_REQ,
};

struct hwtstamp_config {
	int flags;
	int tx_type;
	int rx_filter;
};

#endif /* !__KERNEL__ || __VMKLNX__ */

#if !defined(EFX_HAVE_NET_TSTAMP)

/* Read any transmit or receive timestamps since the last call **************/
#define EFX_TS_READ 0xef13

struct efx_ts_read {
	__u32 tx_valid;
	struct efx_timespec tx_ts;
	struct efx_timespec tx_ts_hw;
	__u32 rx_valid;
	struct efx_timespec rx_ts;
	struct efx_timespec rx_ts_hw;
	__u8 uuid[6];
	__u8 seqid[2];
};
#endif

/* Set the NIC time clock offset ********************************************/
#define EFX_TS_SETTIME 0xef14
struct efx_ts_settime {
	struct efx_timespec ts;	/* In and out */
	__u32 iswrite;		/* 1 == write, 0 == read (only) */
};

/* Adjust the NIC time frequency ********************************************/
#define EFX_TS_ADJTIME 0xef15
struct efx_ts_adjtime {
	__s64 adjustment;	/* Parts per billion, In and out */
	__u32 iswrite;		/* 1 == write, 0 == read (only) */
};

/* Get the NIC-system time skew *********************************************/
#define EFX_TS_SYNC 0xef16
struct efx_ts_sync {
	struct efx_timespec ts;
};

/* Set the NIC-system synchronization status ********************************/
#define EFX_TS_SET_SYNC_STATUS 0xef27
struct efx_ts_set_sync_status {
	__u32 in_sync;		/* 0 == not in sync, 1 == in sync */
	__u32 timeout;		/* Seconds until no longer in sync */
};

/* Get the clock/timestamp capabilities, like ETHTOOL_GET_TS_INFO ***********/
#define EFX_GET_TS_INFO 0xef24
#ifndef ETHTOOL_GET_TS_INFO
	struct ethtool_ts_info {
		__u32	cmd;
		__u32	so_timestamping;
		__s32	phc_index;
		__u32	tx_types;
		__u32	tx_reserved[3];
		__u32	rx_filters;
		__u32	rx_reserved[3];
	};
	#define ETHTOOL_GET_TS_INFO	0x00000041 /* Get time stamping and PHC info */
#endif

/* Return a PPS timestamp ***************************************************/
#define EFX_TS_GET_PPS 0xef1c
struct efx_ts_get_pps {
	__u32 sequence;				/* seq. num. of assert event */
	__u32 timeout;
	struct efx_timespec sys_assert;		/* time of assert in system time */
	struct efx_timespec nic_assert;		/* time of assert in nic time */
	struct efx_timespec delta;		/* delta between NIC and system time */
};

#define EFX_TS_ENABLE_HW_PPS 0xef1d
struct efx_ts_hw_pps {
	__u32 enable;
};


/* Efx private ioctl command structures *************************************/

union efx_ioctl_data {
	struct hwtstamp_config ts_init;
#if !defined(EFX_HAVE_NET_TSTAMP)
	struct efx_ts_read ts_read;
#endif
	struct efx_ts_settime ts_settime;
	struct efx_ts_adjtime ts_adjtime;
	struct efx_ts_sync ts_sync;
	struct efx_ts_set_sync_status ts_set_sync_status;
	struct ethtool_ts_info ts_info;
	struct efx_ts_get_pps pps_event;
	struct efx_ts_hw_pps pps_enable;
};

/**
 * struct efx_ioctl - Parameters for sfc private ioctl on char device
 * @if_name: Name of the net device to control
 * @cmd: Command number
 * @u: Command-specific parameters
 *
 * Usage:
 *     struct efx_ioctl efx;
 *
 *     fd = open("/dev/sfc_control", %O_RDWR);
 *
 *     strncpy(efx.if_name, if_name, %IFNAMSIZ);
 *
 *     efx.cmd = %EFX_FROBNOSTICATE;
 *
 *     efx.u.frobnosticate.magic = 42;
 *
 *     ret = ioctl(fd, %SIOCEFX, & efx);
 */
struct efx_ioctl {
	char if_name[IFNAMSIZ];
	/* Command to run */
	__u16 cmd;
	/* Parameters */
	union efx_ioctl_data u;
} __attribute__ ((packed));

/**
 * struct efx_sock_ioctl - Parameters for sfc private ioctl on socket
 * @cmd: Command number
 * @u: Command-specific parameters
 *
 * Usage:
 *     struct ifreq ifr;
 *
 *     struct efx_sock_ioctl efx;
 *
 *     fd = socket(%AF_INET, %SOCK_STREAM, 0);
 *
 *     strncpy(ifr.ifr_name, if_name, %IFNAMSIZ);
 *
 *     ifr.ifr_data = (caddr_t) & efx;
 *
 *     efx.cmd = %EFX_FROBNOSTICATE;
 *
 *     efx.u.frobnosticate.magic = 42;
 *
 *     ret = ioctl(fd, %SIOCEFX, & ifr);
 */
struct efx_sock_ioctl {
	/* Command to run */
	__u16 cmd;
	__u16 reserved;
	/* Parameters */
	union efx_ioctl_data u;
} __attribute__ ((packed));

#ifdef __KERNEL__
int efx_private_ioctl(struct efx_nic *efx, u16 cmd,
		      union efx_ioctl_data __user *data);
int efx_control_init(void);
void efx_control_fini(void);
#endif

#endif /* EFX_IOCTL_H */
