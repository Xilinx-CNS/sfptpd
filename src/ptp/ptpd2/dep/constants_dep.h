/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */
/* (c) Copyright prior contributors */

#ifndef CONSTANTS_DEP_H
#define CONSTANTS_DEP_H

/**
*\file
* \brief Platform-dependent constants definition
*
* This header defines all includes and constants which are platform-dependent
*
* ptpdv2 is only implemented for linux, NetBSD and FreeBSD
 */

/* platform dependent */

#if !defined(linux) && !defined(__NetBSD__) && !defined(__FreeBSD__) && \
  !defined(__APPLE__)
#error Not ported to this architecture, please update.
#endif

#ifdef linux
#include <netinet/in.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <ifaddrs.h>
#define IFACE_NAME_LENGTH         IF_NAMESIZE
#define NET_ADDRESS_LENGTH        INET_ADDRSTRLEN

#define IFCONF_LENGTH 10

#define octet ether_addr_octet
#include <endian.h>
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define PTPD_LSBF
#elif __BYTE_ORDER == __BIG_ENDIAN
#define PTPD_MSBF
#endif
#endif /* linux */

#ifndef htobe64
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htobe64(x) __bswap_64(x)
#elif __BYTE_ORDER == __BIG_ENDIAN
#define htobe64(x) (x)
#endif
#endif

#ifndef betoh64
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define betoh64(x) __bswap_64(x)
#elif __BYTE_ORDER == __BIG_ENDIAN
#define betoh64(x) (x)
#endif
#endif

#if defined(__NetBSD__) || defined(__FreeBSD__) || defined(__APPLE__)
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <net/if.h>
# include <net/if_dl.h>
# include <net/if_types.h>
# if defined(__FreeBSD__) || defined(__APPLE__)
#  include <net/ethernet.h>
#  include <sys/uio.h>
# else
#  include <net/if_ether.h>
# endif
# include <ifaddrs.h>
# define IFACE_NAME_LENGTH         IF_NAMESIZE
# define NET_ADDRESS_LENGTH        INET_ADDRSTRLEN

# define IFCONF_LENGTH 10

# define adjtimex ntp_adjtime

# include <machine/endian.h>
# if BYTE_ORDER == LITTLE_ENDIAN
#   define PTPD_LSBF
# elif BYTE_ORDER == BIG_ENDIAN
#   define PTPD_MSBF
# endif
#endif

#define CLOCK_IDENTITY_LENGTH 8

#define SUBDOMAIN_ADDRESS_LENGTH  4
#define PORT_ADDRESS_LENGTH       2
#define PTP_UUID_LENGTH		  6
#define CLOCK_IDENTITY_LENGTH	  8
#define FLAG_FIELD_LENGTH		  2

#define PACKET_SIZE  500 // allow space for optional TLVs
#define PACKET_BEGIN_UDP (ETHER_HDR_LEN + sizeof(struct ip) + \
	    sizeof(struct udphdr))
#define PACKET_BEGIN_ETHER (ETHER_HDR_LEN)

#define DEFAULT_PTP_EVENT_PORT    "319"
#define DEFAULT_PTP_GENERAL_PORT  "320"

#define DEFAULT_PTP_PRIMARY_ADDRESS_IPV4  "224.0.1.129"
#define DEFAULT_PTP_PDELAY_ADDRESS_IPV4   "224.0.0.107"

#define DEFAULT_PTP_PRIMARY_ADDRESS_IPV6_LL "ff02::181"
#define DEFAULT_PTP_PRIMARY_ADDRESS_IPV6_G  "ff0e::181"
#define DEFAULT_PTP_PDELAY_ADDRESS_IPV6     "ff02::6b"


/* 802.3 Support */

#define PTP_ETHER_DST "01:1b:19:00:00:00"
#define PTP_ETHER_TYPE 0x88f7
#define PTP_ETHER_PEER "01:80:c2:00:00:0E"

#define MM_STARTING_BOUNDARY_HOPS  0x7fff

/* others */

/* bigger screen size constants */
#define SCREEN_BUFSZ  228
#define SCREEN_MAXSZ  180

/* default size for string buffers */
#define BUF_SIZE  1000

/* limit operator messages to once every X seconds */
#define OPERATOR_MESSAGES_INTERVAL 300.0


#define MAXTIMESTR 32
#endif /*CONSTANTS_DEP_H_*/
