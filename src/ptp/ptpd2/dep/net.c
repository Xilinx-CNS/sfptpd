/*-
 * Copyright (c) 2023-2024 Advanced Micro Devices, Inc.
 * Copyright (c) 2019      Xilinx, Inc.
 * Copyright (c) 2014-2018 Solarflare Communications Inc.
 * Copyright (c) 2013      Harlan Stenn,
 *                         George N. Neville-Neil,
 *                         Wojciech Owczarek
 *                         Solarflare Communications Inc.
 * Copyright (c) 2011-2012 George V. Neville-Neil,
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Wojciech Owczarek,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen,
 *                         Inaqui Delgado,
 *                         Rick Ratzel,
 *                         National Instruments.
 *                         Solarflare Communications Inc.
 * Copyright (c) 2009-2010 George V. Neville-Neil, 
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen
 *
 * Copyright (c) 2005-2008 Kendall Correll, Aidan Williams
 *
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
* @file   net.c
* @date   Tue Jul 20 16:17:49 2010
*
* @brief  Functions to interact with the network sockets and NIC driver.
*
*
*/

#ifdef linux
#include <asm/types.h>
#include <sys/socket.h>
#include <time.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>

#ifdef HAVE_ONLOAD_EXT
#include <onload/extensions.h>
#endif

/* SO_EE_ORIGIN_TIMESTAMPING is defined in linux/errqueue.h in recent kernels.
 * Define it for compilation with older kernels.
 */
#ifndef SO_EE_ORIGIN_TIMESTAMPING
#define SO_EE_ORIGIN_TIMESTAMPING 4
#endif

/* SO_TIMESTAMP is defined in asm/socket.h in recent kernels.
 * Define it here for compilation with older kernels.
 */
#ifndef SO_TIMESTAMPNS
#define SO_TIMESTAMPNS 35
#ifndef SCM_TIMESTAMPNS
#define SCM_TIMESTAMPNS SO_TIMESTAMPNS
#endif
#endif

/* SO_TIMESTAMPING is defined in asm/socket.h in recent kernels.
 * Define it for compilation with older kernels.
 */
#ifndef SO_TIMESTAMPING
#define SO_TIMESTAMPING  37
#endif

/* SCM_TIMESTAMPING_PKTINFO was introduced in Linux 4.13.
 * Define it for compatibility with older kernels.
 */
#ifndef SCM_TIMESTAMPING_PKTINFO
#define SCM_TIMESTAMPING_PKTINFO     58
enum {
	SOF_TIMESTAMPING_OPT_PKTINFO = (1<<13),
};
struct scm_ts_pktinfo {
	__u32 if_index;
	__u32 pkt_length;
	__u32 reserved[2];
};
#endif

#endif /*linux*/

#include "../ptpd.h"
#include "efx_ioctl.h"

#if defined PTPD_SNMP
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
#endif


/****************************************************************************
 * Inline helper functions to iterate over pending timestamp requests
 ****************************************************************************/

typedef unsigned int ts_cache_iterator_t;

/* Create iterator over pending timestamping requests, or free slots in the
 * cache if free_slots true */
static inline ts_cache_iterator_t ts_cache_iterator(struct sfptpd_ts_cache *cache,
						    bool free_slots)
{
	return free_slots ? cache->free_bitmap : ~cache->free_bitmap;
}

/* Check for iteration complete */
static inline bool ts_cache_iterator_done(const ts_cache_iterator_t iterator)
{
	return iterator == 0;
}

/* Get the slot index for the next pending request slot */
static inline int ts_cache_iterator_slot(const ts_cache_iterator_t iterator)
{
	return __builtin_clz(iterator);
}

/* Get the bit number of the given cache slot */
static inline int ts_cache_bit(int slot)
{
	return TS_CACHE_SIZE - slot - 1;
}

/* Move the iterator to the next pending slot */
static inline void ts_cache_iterator_next(ts_cache_iterator_t *iterator)
{
	*iterator &= ~(1 << ts_cache_bit(ts_cache_iterator_slot(*iterator)));
}

/* Convenience macro for iterating over the cache. 'slot' is an lvalue. */
#define TS_CACHE_FOREACH(cache, slot) \
	for (ts_cache_iterator_t iterator = ts_cache_iterator((cache), false), slot = ts_cache_iterator_slot(iterator); \
	     !ts_cache_iterator_done(iterator); \
	     ts_cache_iterator_next(&iterator), slot = ts_cache_iterator_slot(iterator))


/****************************************************************************
 * Constants
 ****************************************************************************/

const char *ts_quantile_units[] = { "1us", "10us", "100us", "1ms", "10ms", "100ms", "1s", "10s", "100s" };
#define TS_QUANTILE_MIN_UNIT_E10 -6


/****************************************************************************
 * Global ptpd net module functions
 ****************************************************************************/

void formatTsPkt(struct sfptpd_ts_user *pkt, char desc[48])
{
	const char *type = "Unknown";

	switch(pkt->type) {
	case TS_SYNC:
		type = "Sync";
		break;
	case TS_DELAY_REQ:
		type = "Delay_Req";
		break;
	case TS_PDELAY_REQ:
		type = "PDelay_Req";
		break;
	case TS_PDELAY_RESP:
		type = "PDelay_Resp";
		break;
	case TS_MONITORING_SYNC:
		type = "Monitoring Sync";
		break;
	}

	snprintf(desc, 48, "%s, seq %" PRIu16, type, pkt->seq_id);
}

void copyAddress(struct sockaddr_storage *destAddr,
			socklen_t *destLen,
			const struct sockaddr_storage *srcAddr,
			socklen_t srcLen)
{
	assert(destAddr != NULL);
	assert(destLen != NULL);

	if (srcLen > sizeof *destAddr) {
		CRITICAL("attempt to copy address that is too big: %d > %d\n",
			 srcLen, sizeof *destAddr);
		*destLen = 0;
		assert(NULL == "Address too big");
	} else {
		*destLen = srcLen;
		if (srcLen > 0) {
			assert(srcAddr != NULL);
			memcpy(destAddr, srcAddr, srcLen);
		}
	}
}

void copyPort(struct sockaddr_storage *dest,
	      struct sockaddr_storage *src) {

	assert(dest->ss_family == src->ss_family);

	switch (dest->ss_family) {
	case AF_INET:
		((struct sockaddr_in *) dest)->sin_port = ((struct sockaddr_in *) src)->sin_port;
		break;
	case AF_INET6:
		((struct sockaddr_in6 *) dest)->sin6_port = ((struct sockaddr_in6 *) src)->sin6_port;
		break;
	default:
		CRITICAL("unexpected addressing family %d copying port\n", dest->ss_family);
		assert(false);
	}
}

Boolean isPortSet(struct sockaddr_storage *addr) {

	switch (addr->ss_family) {
	case AF_INET:
		return (((struct sockaddr_in *) addr)->sin_port) != 0;
	case AF_INET6:
		return (((struct sockaddr_in6 *) addr)->sin6_port) != 0;
	default:
		return TRUE;
	}
}


void setLoopback(struct sockaddr_storage *dest,
		 socklen_t destLen) {

	assert(destLen != 0);

	switch (dest->ss_family) {
	case AF_INET:
		assert(destLen >= sizeof(struct sockaddr_in));
		((struct sockaddr_in *) dest)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		break;
	case AF_INET6:
		assert(destLen >= sizeof(struct sockaddr_in6));
		((struct sockaddr_in6 *) dest)->sin6_addr = in6addr_loopback;
		break;
	default:
		CRITICAL("unexpected addressing family %s setting host\n", dest->ss_family);
		assert(false);
	}
}

Boolean hostAddressesEqual(const struct sockaddr_storage *addressA,
			   socklen_t lengthA,
			   const struct sockaddr_storage *addressB,
			   socklen_t lengthB)
{
	if (lengthA != 0 && lengthB != 0 &&
	    addressA->ss_family == addressB->ss_family) {
		switch(addressA->ss_family) {
		case AF_INET:
			/* If we know the addressing family, only compare the host portion */
			return (0 == memcmp(&((struct sockaddr_in *) addressA)->sin_addr,
					    &((struct sockaddr_in *) addressB)->sin_addr,
					    sizeof ((struct sockaddr_in *) addressA)->sin_addr));
		case AF_INET6:
			/* If we know the addressing family, only compare the host portion */
			return (0 == memcmp(&((struct sockaddr_in6 *) addressA)->sin6_addr,
					    &((struct sockaddr_in6 *) addressB)->sin6_addr,
					    sizeof ((struct sockaddr_in6 *) addressA)->sin6_addr));
		}
	}

	return (lengthA == lengthB) ?
		(0 == memcmp(addressA, addressB, lengthA)) :
		FALSE;
}

void writeProtocolAddress(PortAddress *protocolAddress,
			  const struct sockaddr_storage *address,
			  socklen_t length)
{
	switch (address->ss_family) {
	case AF_INET:
		protocolAddress->networkProtocol = PTPD_NETWORK_PROTOCOL_UDP_IPV4;
		protocolAddress->addressLength = 4;
		XMALLOC(protocolAddress->addressField,
			protocolAddress->addressLength);
		memcpy(protocolAddress->addressField,
		       &((struct sockaddr_in *) address)->sin_addr,
		       protocolAddress->addressLength);
		break;
	case AF_INET6:
		protocolAddress->networkProtocol = PTPD_NETWORK_PROTOCOL_UDP_IPV6;
		protocolAddress->addressLength = 16;
		XMALLOC(protocolAddress->addressField,
			protocolAddress->addressLength);
		memcpy(protocolAddress->addressField,
		       &((struct sockaddr_in6 *) address)->sin6_addr,
		       protocolAddress->addressLength);
		break;
	default:
		CRITICAL("addressing family %d not supported or expected\n",
			 address->ss_family);
	}
}


static int sendMessage(int sockfd, const void *buf, size_t length,
                       const struct sockaddr *addr, socklen_t addrLen,
                       const char *messageType, void *control,
                       socklen_t controllen);


/**
 * shutdown IPv4 multicast for specific address
 *
 * @param transport
 * @param multicastAddr
 *
 * @return TRUE if successful
 */
static Boolean
netClearIPv4MulticastOptions(struct ptpd_transport *transport, struct sockaddr_storage *multicastAddr)
{
	struct ip_mreqn imr;

	assert(transport->interfaceAddr.ss_family == AF_INET);

	/* Close General Multicast */
	imr.imr_multiaddr = ((struct sockaddr_in *) multicastAddr)->sin_addr;
	imr.imr_address.s_addr = ((struct sockaddr_in *) &transport->interfaceAddr)->sin_addr.s_addr;
	imr.imr_ifindex = transport->interfaceInfo.ifindex;

	setsockopt(transport->eventSock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
		   &imr, sizeof imr);
	setsockopt(transport->generalSock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
		   &imr, sizeof imr);

	return TRUE;
}

/**
 * shutdown IPv6 multicast for specific address
 *
 * @param transport
 * @param multicastAddr
 *
 * @return TRUE if successful
 */
static Boolean
netClearIPv6MulticastOptions(struct ptpd_transport *transport, struct sockaddr_storage *multicastAddr)
{

	struct ipv6_mreq imr;

	assert(transport->interfaceAddr.ss_family == AF_INET6);

	imr.ipv6mr_multiaddr = ((struct sockaddr_in6 *) multicastAddr)->sin6_addr;
	imr.ipv6mr_interface = transport->interfaceInfo.ifIndex;

	setsockopt(transport->eventSock, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP,
		   &imr, sizeof imr);
	setsockopt(transport->generalSock, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP,
		   &imr, sizeof imr);

	return TRUE;
}


/**
 * shutdown multicast for specific address
 *
 * @param transport
 * @param multicastAddr
 *
 * @return TRUE if successful
 */
static Boolean
netClearMulticastOptions(struct ptpd_transport *transport, struct sockaddr_storage *multicastAddr)
{
	if (transport->interfaceAddrLen == 0)
		return TRUE;

	if (transport->interfaceAddr.ss_family == AF_INET) {
		return netClearIPv4MulticastOptions(transport, multicastAddr);
	} else if (transport->interfaceAddr.ss_family == AF_INET6) {
		return netClearIPv6MulticastOptions(transport, multicastAddr);
	} else {
		return FALSE;
	}
}


/**
 * Check if we have a physical interface (we may just be squatting on an
 * aggregate interface temporarily lacking a slave.
 *
 * @param transport the transport object
 *
 * @return TRUE if present, otherwise FALSE
 */
static Boolean
netHavePhysicalInterface(struct ptpd_transport *transport)
{
	return (transport &&
		transport->interfaceInfo.ifIndex >=1 );
}


/**
 * shutdown the multicast (both General and Peer)
 *
 * @param transport
 * 
 * @return TRUE if successful
 */
static Boolean
netShutdownMulticast(struct ptpd_transport *transport)
{
	/* Close General Multicast */
	netClearMulticastOptions(transport, &transport->multicastAddr);
	transport->multicastAddrLen = 0;

	/* Close Peer Multicast */
	netClearMulticastOptions(transport, &transport->peerMulticastAddr);
	transport->peerMulticastAddrLen = 0;

	return TRUE;
}


/* shut down the UDP stuff */
Boolean 
netShutdown(struct ptpd_transport *transport)
{
	netShutdownMulticast(transport);

	/* TODO: Put this into a port-specific shutdown
	   ... but probably not necessary */
	//transport->unicastAddr = 0;

	/* Close sockets */
	if (transport->eventSock >= 0)
		close(transport->eventSock);
	transport->eventSock = -1;

	if (transport->generalSock >= 0)
		close(transport->generalSock);
	transport->generalSock = -1;

	if (transport->monitoringSock >= 0)
		close(transport->monitoringSock);
	transport->monitoringSock = -1;

	freeIpv4AccessList(&transport->timingAcl);
	freeIpv4AccessList(&transport->managementAcl);
	freeIpv4AccessList(&transport->monitoringAcl);

	return TRUE;
}


static int
getInterfaceFlags(char *ifaceName, unsigned int *flags)
{
	int ret;
	struct ifaddrs *ifaddr, *ifa;

	if (!strlen(ifaceName)) {
		DBG("interfaceExists called for an empty interface!");
		return 0;
	}

	if (getifaddrs(&ifaddr) == -1) {
		PERROR("Could not get interface list");
		ret = -1;
		goto end;
	}

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (!strcmp(ifaceName, ifa->ifa_name)) {
			*flags = ifa->ifa_flags;
			ret = 1;
			goto end;
		}
	}

	ret = 0;
	DBG("Interface not found: %s\n", ifaceName);

end:
	freeifaddrs(ifaddr);
	return ret;
}


/* Try getting addr address of family family from interface ifaceName.
   Return 1 on success, 0 when no suitable address available, -1 on failure.
 */
static int
getInterfaceAddress(char* ifaceName, int family, struct sockaddr_storage* addr, socklen_t *len)
{
	int ret = -1;
	struct ifaddrs *ifaddr, *ifa;
	socklen_t size;

	if (getifaddrs(&ifaddr) == -1) {
		PERROR("Could not get interface list");
		goto end;
	}

	if (family == AF_INET)
		size = sizeof(struct sockaddr_in);
	else if (family == AF_INET6)
		size = sizeof(struct sockaddr_in6);
	else
		goto end;

	ret = 0;
	*len = 0;
	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (!strcmp(ifaceName, ifa->ifa_name) && ifa->ifa_addr->sa_family == family) {
			memcpy(addr, ifa->ifa_addr, size);
			*len = size;
			ret = 1;
		}
	}

	if (ret == 0) {
		DBG("Interface not found: %s\n", ifaceName);
	} else {
		DBGV("Interface found: %s\n", ifaceName);
		address_display("interface address", addr, size, TRUE);
	}

end:
	freeifaddrs(ifaddr);
	return ret;
}


/* Try getting hwAddrSize bytes of ifaceName hardware address,
   and place them in hwAddr. Return 1 on success, 0 when no suitable
   hw address available, -1 on failure.
 */
static int
getHwAddress(char *ifaceName, unsigned char *hwAddr, int hwAddrSize)
{
	int ret;
	if(!strlen(ifaceName))
		return 0;

	int sockfd;
	struct ifreq ifr;

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);

	if(sockfd < 0) {
		PERROR("Could not open test socket");
		return -1;
	}

	memset(&ifr, 0, sizeof(struct ifreq));

	sfptpd_strncpy(ifr.ifr_name, ifaceName, sizeof(ifr.ifr_name));

	if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) < 0) {
		DBGV("failed to request hardware address for %s", ifaceName);
		ret = -1;
		goto end;
	}


	int af = ifr.ifr_hwaddr.sa_family;

	if (af == ARPHRD_ETHER || af == ARPHRD_IEEE802) {
		memcpy(hwAddr, ifr.ifr_hwaddr.sa_data, hwAddrSize);
		ret = 1;
	} else {
		DBGV("Unsupported hardware address family on %s\n", ifaceName);
		ret = 0;
	}

end:
	close(sockfd);
	return ret;
}


static Boolean getInterfaceInfo(char *ifaceName, InterfaceInfo *ifaceInfo)
{
	unsigned int ifIndex;
	int res = getInterfaceAddress(ifaceName, ifaceInfo->addressFamily,
				      &ifaceInfo->afAddress, &ifaceInfo->afAddressLen);
	if (res == -1)
		return FALSE;

	if (res == 1)
		ifaceInfo->hasAfAddress = true;

	ifIndex = if_nametoindex(ifaceName);
	if (ifIndex == 0)
		return FALSE;
	ifaceInfo->ifIndex = ifIndex;

	res = getHwAddress(ifaceName, (unsigned char*)ifaceInfo->hwAddress, ETH_ALEN);
	if (res == -1)
		return FALSE;
	ifaceInfo->hasHwAddress = true;

	res = getInterfaceFlags(ifaceName, &ifaceInfo->flags);
	if (res == -1)
		return FALSE;

	if ( (ifaceInfo->ifindex = if_nametoindex(ifaceName)) == 0 )
		return FALSE;

	return TRUE;
}


Boolean
netInitInterfaceInfo(struct ptpd_transport *transport, InterfaceOpts *ifOpts)
{
	InterfaceInfo *info;

	assert(transport != NULL);
	assert(ifOpts != NULL);

	info = &transport->interfaceInfo;
	info->addressFamily = ifOpts->transportAF;

	if (!getInterfaceInfo(ifOpts->ifaceName, info))
		return FALSE;

	if (!info->hasAfAddress) {
		ERROR("interface %s has no address set for the required addressing family\n", ifOpts->ifaceName);
		return FALSE;
	}

	if (!(info->flags & IFF_UP) || !(info->flags & IFF_RUNNING))
		WARNING("interface %s seems to be down. PTP will not operate correctly until it's up.\n",
			ifOpts->ifaceName);

	if (info->flags & IFF_LOOPBACK)
		WARNING("interface %s is a loopback interface.\n", ifOpts->ifaceName);

	if (!(info->flags & IFF_MULTICAST))
		WARNING("interface %s is not multicast capable.\n", ifOpts->ifaceName);

	return TRUE;
}


/**
 * Set the IPv4 multicast options for specific address
 *
 * @param transport
 * @param multicastAddr
 *
 * @return TRUE if successful
 */
static Boolean
netSetIPv4MulticastOptions(struct ptpd_transport * transport, struct sockaddr_storage *multicastAddr)
{
	struct ip_mreqn imr;

	assert(multicastAddr->ss_family == AF_INET);

	/* multicast send only on specified interface */
	imr.imr_multiaddr = ((struct sockaddr_in *) multicastAddr)->sin_addr;
	imr.imr_address.s_addr = ((struct sockaddr_in *) &transport->interfaceAddr)->sin_addr.s_addr;
	imr.imr_ifindex = transport->interfaceInfo.ifindex;
	if (setsockopt(transport->eventSock, IPPROTO_IP, IP_MULTICAST_IF,
		       &imr, sizeof(imr)) < 0 ||
	    setsockopt(transport->generalSock, IPPROTO_IP, IP_MULTICAST_IF,
		       &imr, sizeof(imr)) < 0) {
		PERROR("failed to enable multi-cast on the interface ifindex=%d",
		       imr.imr_ifindex);
		return FALSE;
	}
	/* join multicast group (for receiving) on specified interface */
	if (setsockopt(transport->eventSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, 
		       &imr, sizeof(imr)) < 0 ||
	    setsockopt(transport->generalSock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		       &imr, sizeof(imr)) < 0) {
		PERROR("failed to join the multi-cast group on the interface ifindex=%d",
		       imr.imr_ifindex);
		return FALSE;
	}
	return TRUE;
}


/**
 * Set the IPv6 multicast options for specific address
 *
 * @param transport
 * @param multicastAddr
 *
 * @return TRUE if successful
 */
static Boolean
netSetIPv6MulticastOptions(struct ptpd_transport * transport, struct sockaddr_storage *multicastAddr)
{
	struct ipv6_mreq imr;
	struct sockaddr_in6 *multicastAddr6 = (struct sockaddr_in6 *) multicastAddr;

	assert(multicastAddr->ss_family == AF_INET6);

	/* set scope id in the address for this interface */
	multicastAddr6->sin6_scope_id = transport->interfaceInfo.ifIndex;

	/* multicast send only on specified interface */
	imr.ipv6mr_multiaddr = multicastAddr6->sin6_addr;
	imr.ipv6mr_interface = transport->interfaceInfo.ifIndex;
	if (setsockopt(transport->eventSock, IPPROTO_IPV6, IPV6_MULTICAST_IF,
		       &imr.ipv6mr_interface, sizeof imr.ipv6mr_interface) < 0 ||
	    setsockopt(transport->generalSock, IPPROTO_IPV6, IPV6_MULTICAST_IF,
		       &imr.ipv6mr_interface, sizeof imr.ipv6mr_interface) < 0) {
		PERROR("failed to enable multi-cast on the interface");
		return FALSE;
	}
	/* join multicast group (for receiving) on specified interface */
	if (setsockopt(transport->eventSock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
		       &imr, sizeof imr) < 0 ||
	    setsockopt(transport->generalSock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
		       &imr, sizeof imr) < 0) {
		PERROR("failed to join the multi-cast group");
		return FALSE;
	}
	return TRUE;
}


/**
 * Set the multicast options for specific address
 *
 * @param transport
 * @param multicastAddr
 *
 * @return TRUE if successful
 */
static Boolean
netSetMulticastOptions(struct ptpd_transport * transport, struct sockaddr_storage *multicastAddr)
{
	if (multicastAddr->ss_family == AF_INET) {
		return netSetIPv4MulticastOptions(transport, multicastAddr);
	} else if (multicastAddr->ss_family == AF_INET6) {
		return netSetIPv6MulticastOptions(transport, multicastAddr);
	} else {
		return FALSE;
	}
}


/**
 * Init the multcast (both General and Peer)
 * 
 * @param transport
 * @param rtOpts
 * 
 * @return TRUE if successful
 */
static Boolean
netInitMulticast(struct ptpd_transport * transport,  InterfaceOpts * ifOpts)
{
	int rc;
	int addr_family = ifOpts->transportAF;
	const struct addrinfo hints = {
		.ai_family = addr_family,
		.ai_flags = AI_ADDRCONFIG,
	};
	struct addrinfo *result;
	const char *addrStr;

	/* Init General multicast IP address */

	if (!netHavePhysicalInterface(transport)) {
		INFO("%s: no physical interface for multicast\n",
		     ifOpts->ifaceName);
		return FALSE;
	}

	if (addr_family == AF_INET6) {
		addrStr = ifOpts->linkLocalScope ?
			DEFAULT_PTP_PRIMARY_ADDRESS_IPV6_LL :
			DEFAULT_PTP_PRIMARY_ADDRESS_IPV6_G;
	} else {
		addrStr = DEFAULT_PTP_PRIMARY_ADDRESS_IPV4;
	}
	rc = getaddrinfo(addrStr, NULL, &hints, &result);
	if (rc != 0) {
		ERROR("failed to lookup multi-cast address %s: %s\n",
		      addrStr, gai_strerror(rc));
		return FALSE;
	}
	assert(result != NULL);
	copyAddress(&transport->multicastAddr, &transport->multicastAddrLen,
		    (struct sockaddr_storage *) result->ai_addr, result->ai_addrlen);
	freeaddrinfo(result);
	if (!netSetMulticastOptions(transport, &transport->multicastAddr)) {
		return FALSE;
	}
	address_display("general/non-peer event multicast address",
			&transport->multicastAddr, transport->multicastAddrLen, TRUE);
	/* End of General multicast Ip address init */

	/* Init Peer multicast IP address */
	if (addr_family == AF_INET6) {
		addrStr = DEFAULT_PTP_PDELAY_ADDRESS_IPV6;
	} else {
		addrStr = DEFAULT_PTP_PDELAY_ADDRESS_IPV4;
	}
	rc = getaddrinfo(addrStr, NULL, &hints, &result);
	if (rc != 0) {
		ERROR("failed to lookup multi-cast address %s: %s\n",
		      addrStr, gai_strerror(rc));
		return FALSE;
	}
	assert(result != NULL);
	copyAddress(&transport->peerMulticastAddr, &transport->peerMulticastAddrLen,
		    (struct sockaddr_storage *) result->ai_addr, result->ai_addrlen);
	freeaddrinfo(result);
	if (!netSetMulticastOptions(transport, &transport->peerMulticastAddr)) {
		return FALSE;
	}
	address_display("peer event multicast address",
			&transport->peerMulticastAddr, transport->peerMulticastAddrLen, TRUE);
	/* End of Peer multicast Ip address init */

	return TRUE;
}

Boolean
netInitPort(PtpClock *ptpClock, RunTimeOpts *rtOpts)
{
	int rc;
	const struct addrinfo hints = {
		.ai_family = rtOpts->ifOpts->transportAF,
	};
	struct addrinfo *result;

	ptpClock->unicastAddrLen = 0;
	if (rtOpts->unicastAddress[0] != '\0') {
		rc = getaddrinfo(rtOpts->unicastAddress,
				 NULL, &hints, &result);
		if (rc == 0 && result != NULL) {
			copyAddress(&ptpClock->unicastAddr,
				    &ptpClock->unicastAddrLen,
				    (struct sockaddr_storage *) result->ai_addr, result->ai_addrlen);
			freeaddrinfo(result);
		} else {
			ERROR("could not resolve unicast host %s to address: %s\n",
			      rtOpts->unicastAddress,
			      gai_strerror(rc));
		}
	}

	return TRUE;
}

static Boolean
netSetMulticastTTL(int sockfd, int ttl)
{
	int temp = ttl;

	if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL,
		       &temp, sizeof(temp)) < 0) {
	    PERROR("Failed to set socket multicast time-to-live");
	    return FALSE;
	}
	return TRUE;
}


static Boolean
netSetMulticastLoopback(struct ptpd_transport *transport, Boolean value, int addr_family) {
	int temp = value ? 1 : 0;
	int rc = 0;

	assert(transport != NULL);

	DBG("Going to set multicast loopback with %d \n", temp);

	if (addr_family == AF_INET) {
		rc = setsockopt(transport->eventSock, IPPROTO_IP, IP_MULTICAST_LOOP,
				&temp, sizeof temp);
	} else if (addr_family == AF_INET6) {
		rc = setsockopt(transport->eventSock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
				&temp, sizeof temp);
	}

	if (rc < 0) {
		PERROR("Failed to set multicast loopback");
		return FALSE;
	}

	return TRUE;
}

static void reset_timestamp(struct sfptpd_ts_info *info)
{
	assert(info);
	memset(info, '\0', sizeof *info);
}

static int parse_timestamp(uint8_t *pdu, size_t pdu_len,
			   enum ptpd_ts_fmt ts_fmt,
			   struct sfptpd_ts_info *info,
			   bool tx)
{
	const char *type = ts_fmt == PTPD_TS_ONLOAD_EXT ? "onload extension timestamp" : "so_timestamping";
	union {
		void *ptr;
		struct timespec *lnx;
#ifdef HAVE_ONLOAD_EXT
		struct onload_timestamp *onload;
#endif
	} ts;

	assert(info);

	if (
#ifdef HAVE_ONLOAD_EXT
	    (ts_fmt == PTPD_TS_ONLOAD_EXT && pdu_len < CMSG_LEN(sizeof(*ts.onload) * 1)) ||
#endif
	    (ts_fmt == PTPD_TS_LINUX && pdu_len < CMSG_LEN(sizeof(*ts.lnx) * 3))) {
		ERROR("received short %s (%zu)\n", type, pdu_len);
		memset(info, '\0', sizeof *info);
		return ENOTIMESTAMP;
	}

	ts.ptr = pdu;

	if (ts_fmt == PTPD_TS_ONLOAD_EXT) {
#ifdef HAVE_ONLOAD_EXT
		if (ts.onload->sec) {
			info->hw.sec = ts.onload->sec;
			info->hw.nsec = ts.onload->nsec;
			info->hw.nsec_frac = (uint32_t) ts.onload->nsec_frac << 8;
			info->have_hw = true;
		} else {
			sfptpd_time_zero(&info->hw);
			info->have_hw = false;
		}
		sfptpd_time_zero(&info->sw);
		info->have_sw = false;
#endif
	} else {
		sfptpd_time_from_std_floor(&info->sw, &ts.lnx[0]);
		sfptpd_time_from_std_floor(&info->hw, &ts.lnx[2]);
		info->have_sw = info->sw.sec != 0;
		info->have_hw = info->hw.sec != 0;
	}

	if (info->have_sw) {
		DBG("%s sw timestamp: " SFPTPD_FMT_SFTIMESPEC " (%s)\n",
		    tx ? "Tx" : "Rx",
		    SFPTPD_ARGS_SFTIMESPEC(info->sw), type);
	}
	if (info->have_hw) {
		DBG("%s hw timestamp: " SFPTPD_FMT_SFTIMESPEC " (%s)\n",
		    tx ? "Tx" : "Rx",
		    SFPTPD_ARGS_SFTIMESPEC(info->hw), type);
	}

	return info->have_sw || info->have_hw ? 0 : ENOTIMESTAMP;
}

struct sfptpd_ts_ticket netMatchPacketToTsCache(struct sfptpd_ts_cache *ts_cache,
						struct sfptpd_ts_user *user,
						const char *data, size_t length)
{
	struct sfptpd_timespec now, elapsed;
	struct sfptpd_ts_pkt *pkt;
	int quantile;

	assert(ts_cache);
	assert(data);

	sfclock_gettime(CLOCK_MONOTONIC, &now);

	TS_CACHE_FOREACH(ts_cache, slot) {
		pkt = &ts_cache->packet[slot];

		/* Skip where packet too short to match */
		if (length < pkt->match.pdu.len + pkt->match.pdu.trailer)
			continue;

		DBGV("Checking PDU slot %d\n", slot);
		DUMP("PDU to match",
		     pkt->match.pdu.data, pkt->match.pdu.len);

		if (memcmp(pkt->match.pdu.data,
			   data +
			   length -
			   pkt->match.pdu.len -
			   pkt->match.pdu.trailer,
			   pkt->match.pdu.len) == 0) {

			/* Record the match details for user */
			if (user)
				*user = pkt->user;

			/* Remove from cache */
			ts_cache->free_bitmap |= (1 << ts_cache_bit(slot));

			/* Record time take to get the result */
			sfptpd_time_subtract(&elapsed, &now, &pkt->sent_monotime);
			for (quantile = 0; quantile < TS_QUANTILES; quantile++) {
				if (!sfptpd_time_is_greater_or_equal(&elapsed, &ts_cache->stats_periodic.quantile_bounds[quantile])) {
					ts_cache->stats_periodic.resolved_quantile[quantile]++;
					ts_cache->stats_adhoc.resolved_quantile[quantile]++;
					break;
				}
			}
			DBGV("Tx timestamp took " SFPTPD_FMT_SFTIMESPEC "s to acquire\n",
			     SFPTPD_ARGS_SFTIMESPEC(elapsed));

			/* Return ticket for match to user */
			return (struct sfptpd_ts_ticket) { .slot = slot, .seq = pkt->seq };
		}
	}

	return TS_NULL_TICKET;
}

bool netProcessError(PtpInterface *ptpInterface,
		     size_t length,
		     struct sfptpd_ts_user *user,
		     struct sfptpd_ts_ticket *ticket,
		     struct sfptpd_ts_info *info)
{
	int ipproto = ptpInterface->ifOpts.transportAF == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
	int iptype = ipproto == IPPROTO_IPV6 ? IPV6_RECVERR : IP_RECVERR;
	Boolean haveTs = FALSE, havePkt = FALSE;
	struct msghdr *msg = &ptpInterface->msgEbuf;
	struct cmsghdr *cmsg;
	struct sock_extended_err *err;

	assert(user);
	assert(ticket);
	assert(info);

	reset_timestamp(info);

	if (length != 0) {
		DUMP("cmsg (pdu on error queue)", msg->msg_iov[0].iov_base, length);
	}

	if (length == 0) {
		DBG("ignoring socket error queue message with no payload\n");
		goto finish;
	}

	for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL;
	     cmsg = CMSG_NXTHDR(msg, cmsg)) {
		size_t len;
		int level, type;

		len = cmsg->cmsg_len;
		level = cmsg->cmsg_level;
		type  = cmsg->cmsg_type;

		DBGV("control message len=%zu level=%d type=%d\n",
		     len, level, type);
		DUMP("cmsg (header)", cmsg, len);

		if ((SOL_SOCKET == level) &&
		    (SO_TIMESTAMPING == type)) {
			haveTs = parse_timestamp(CMSG_DATA(cmsg), len,
						 ptpInterface->ts_fmt,
						 info, true) == 0;
		} else if ((ipproto == level) &&
			   (iptype == type)) {
			err = (struct sock_extended_err *)CMSG_DATA(cmsg);
			if (err->ee_origin != SO_EE_ORIGIN_TIMESTAMPING ||
			    err->ee_errno != ENOMSG) {
		                WARNING("unexpected socket error "
					"queue msg: origin %d, errno %d\n",
				err->ee_origin, err->ee_errno);
			} else {
				havePkt = TRUE;
			}
		} else {
			WARNING("unexpected socket error queue "
				"msg: level %d, type %d\n",
				level, type);
		}
	}

	if (haveTs && !havePkt)
		WARNING("retrieved transmit timestamp "
			"but no packet\n");
	if (!haveTs && havePkt)
		WARNING("retrieved packet but no "
			"transmit timestamp\n");
	if (!haveTs && !havePkt)
		WARNING("retrieved neither packet nor "
			"transmit timestamp\n");
	if (!havePkt)
		goto finish;

	*ticket = netMatchPacketToTsCache(&ptpInterface->ts_cache,
					  user,
					  msg->msg_iov[0].iov_base,
					  length);

	if (sfptpd_ts_is_ticket_valid(*ticket)) {
		if (!haveTs) {
			WARNING("received looped back transmit "
				"packet but no timestamp\n");
		}
	} else {
		if (havePkt) {
			WARNING("unexpected pkt received on "
				"socket error queue. Expected one of:\n");
			TS_CACHE_FOREACH(&ptpInterface->ts_cache, slot) {
				struct sfptpd_ts_pkt *pkt = &ptpInterface->ts_cache.packet[slot];
				dump("expected", pkt->match.pdu.data, pkt->match.pdu.len);
			}
			dump("got (with headers)", msg->msg_iov[0].iov_base, length);
		}
	}

finish:
	return haveTs;
}

void netCheckTimestampStats(struct sfptpd_ts_cache *cache,
			    struct sfptpd_ts_stats *stats, int severity)
{
	struct sfptpd_timespec now;
	struct sfptpd_timespec elapsed;
	char buf[120];
	int quantile;
	int ptr;
	int ret;
	int q;
	struct sfptpd_timespec period;
	sfptpd_time_t period_f;

	sfclock_gettime(CLOCK_MONOTONIC, &now);
	sfptpd_time_subtract(&period, &now, &stats->start);
	period_f = sfptpd_time_timespec_to_float_s(&period);

	for (quantile = 0; quantile < TS_QUANTILES; quantile++)
		stats->pending_quantile[quantile] = 0;

	TS_CACHE_FOREACH(cache, slot) {
		int quantile;
		sfptpd_time_subtract(&elapsed, &now, &cache->packet[slot].sent_monotime);
		for (quantile = 0; quantile < TS_QUANTILES; quantile++) {
			if (!sfptpd_time_is_greater_or_equal(&elapsed, &stats->quantile_bounds[quantile])) {
				stats->pending_quantile[quantile]++;
				break;
			}
		}
	}

	ptr = 0;
	for (q = 0; q < TS_QUANTILES; q++) {
		int q_e10 = q + TS_QUANTILE_E10_MIN;

		assert(q_e10 >= TS_QUANTILE_MIN_UNIT_E10);
		assert(q < (sizeof ts_quantile_units) / (sizeof *ts_quantile_units));

		ret = snprintf(buf + ptr, sizeof buf - ptr, " <%5s |",
			       q == TS_QUANTILES - 1 ? "T_MAX" :
			       ts_quantile_units[q_e10 - TS_QUANTILE_MIN_UNIT_E10]);
		if (ret >= sizeof buf)
			goto truncated;
		else
			ptr += ret;
	}
	DBGX(severity, " over %-02.1Lfs period |%s\n", period_f, buf);

	ptr = 0;
	for (q = 0; q < TS_QUANTILES; q++) {
		ret = snprintf(buf + ptr, sizeof buf - ptr, " %6d |",
			       stats->resolved_quantile[q]);
		if (ret >= sizeof buf)
			goto truncated;
		else
			ptr += ret;
	}
	DBGX(severity, "num resolved tx ts |%s\n", buf);

	ptr = 0;
	for (q = 0; q < TS_QUANTILES; q++) {
		ret = snprintf(buf + ptr, sizeof buf - ptr, " %6d |",
			       stats->pending_quantile[q]);
		if (ret >= sizeof buf)
			goto truncated;
		else
			ptr += ret;
	}
	DBGX(severity, "num pending tx ts  |%s (%d evicted)\n", buf, stats->evicted);

truncated:
	/* Failure of the diagnostic output is (a) unlikely, (b) not serious. */
	stats->evicted = 0;
	stats->total = 0;
	stats->start = now;

	for (quantile = 0; quantile < TS_QUANTILES; quantile++)
		stats->resolved_quantile[quantile] = 0;
}

enum sfptpd_tristate netCheckTimestampAlarms(PtpClock *ptpClock)
{
	struct sfptpd_ts_cache *cache = &ptpClock->interface->ts_cache;
	struct sfptpd_timespec elapsed;
	struct sfptpd_timespec period;
	struct sfptpd_timespec now;
	const int alarm_quantile = TS_TIME_TO_ALARM_E10 - TS_QUANTILE_E10_MIN;
	const int evict_quantile = TS_TIME_TO_EVICT_E10 - TS_QUANTILE_E10_MIN;
	enum sfptpd_tristate alarm = TRISTATE_OFF; /* Default to clearing alarm */
	char buf[48];

	/* Do not set alarm because of evicted timestamping requests because
	 * we no longer know which port they were associated with and the
	 * given ports should already have alarmed based on the timestamps they
	 * wanted being late.
	 */

	assert(alarm_quantile < TS_QUANTILES);
	sfclock_gettime(CLOCK_MONOTONIC, &now);
	sfptpd_time_subtract(&period, &now, &cache->stats_periodic.start);

	TS_CACHE_FOREACH(cache, slot) {
		if (cache->packet[slot].user.port == ptpClock) {
			sfptpd_time_subtract(&elapsed, &now, &cache->packet[slot].sent_monotime);

			/* Raise an alarm if there are timestamp requests waiting longer then the alarm threshold */
			if (sfptpd_time_is_greater_or_equal(&elapsed, &cache->stats_periodic.quantile_bounds[alarm_quantile])
			    && alarm != 1) {
				if (cache->packet[slot].has_caused_alarm) {
					alarm = TRISTATE_Z; /* hysteresis */
				} else {
					alarm = TRISTATE_ON;
					cache->packet[slot].has_caused_alarm = true;
				}
			}

			/* Evict timestamp requests taking longer than the eviction threshold.
			 * These will not show up as alarmed next time round. */
			if (sfptpd_time_is_greater_or_equal(&elapsed, &cache->stats_periodic.quantile_bounds[evict_quantile])) {
				formatTsPkt(&cache->packet[slot].user, buf);
				DBGV("ptpd: timestamp taking longer than %s; evicting %s\n",
				     ts_quantile_units[evict_quantile], buf);
				cache->free_bitmap |= ~((unsigned int) INT_MAX) >> slot;
				cache->stats_periodic.evicted++;
				cache->stats_adhoc.evicted++;
			}
		}
	}

	return alarm;
}


/* Used to get receive timestamps  */
static Boolean getRxTimestamp(PtpInterface *ptpInterface,
			      char *pdu, int pduLength,
			      struct msghdr *msg,
			      struct sfptpd_ts_info *info)
{
	struct cmsghdr *cmsg;

	assert(info);
	reset_timestamp(info);

	if (msg->msg_controllen <= 0) {
		DBG2("received PTP event packet with no timestamp (%zu)\n",
		     msg->msg_controllen);
		return FALSE;
	}

	for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		struct timeval *tv;
		struct timespec *ts;
		struct scm_ts_pktinfo *ts_pktinfo;

		DUMP("CM", cmsg, cmsg->cmsg_len);

		if (cmsg->cmsg_level != SOL_SOCKET)
			continue;

		switch (cmsg->cmsg_type) {
		case SCM_TIMESTAMP:
			tv = (struct timeval *)CMSG_DATA(cmsg);

			if(cmsg->cmsg_len < CMSG_LEN(sizeof(*tv))) {
				ERROR("received short SCM_TIMESTAMP (%zu)\n",
				      cmsg->cmsg_len);
				return FALSE;
			}
			sfptpd_time_init(&info->sw, tv->tv_sec, tv->tv_usec * 1000, 0);
			info->have_sw = true;
			break;

		case SCM_TIMESTAMPNS:
			ts = (struct timespec *)CMSG_DATA(cmsg);

			if(cmsg->cmsg_len < CMSG_LEN(sizeof(*ts))) {
				ERROR("received short SCM_TIMESTAMPNS (%zu)\n",
				      cmsg->cmsg_len);
				return FALSE;
			}
			sfptpd_time_init(&info->sw, ts->tv_sec, ts->tv_nsec, 0);
			info->have_sw = true;
			break;

		case SO_TIMESTAMPING:
			parse_timestamp(CMSG_DATA(cmsg), cmsg->cmsg_len,
					ptpInterface->ts_fmt,
					info, FALSE);
			break;

		case SCM_TIMESTAMPING_PKTINFO:
			ts_pktinfo = (struct scm_ts_pktinfo*)CMSG_DATA(cmsg);

			if(cmsg->cmsg_len < CMSG_LEN(sizeof(*ts_pktinfo))) {
				ERROR("received short SCM_TIMESTAMPING_PKTINFO (%zu)\n",
				      cmsg->cmsg_len);
				return FALSE;
			}
			info->if_index = ts_pktinfo->if_index;
			break;
		}
	}

	if(!info->have_sw && !info->have_hw)
		DBG("failed to retrieve rx time stamp\n");

	return info->have_sw || info->have_hw;
}


/* Attempt to use SO_TIMESTAMPING with the provided flags, and try to include
 * SOF_TIMESTAMPING_OPT_PKTINFO if possible. */
static Boolean netTryEnableTimestampingPktinfo(PtpInterface *ptpInterface, int flags)
{
	int has_pktinfo_flag;

	/* The option SOF_TIMESTAMPING_OPT_PKTINFO was only introduced in the
	 * linux kernel version 4.13, and is not necessary for sfptpd. Older
	 * kernels will return `-EINVAL` on setsockopt, so we try both with
	 * and without this option and let other code deal with the presence/
	 * absence of pktinfo for timestamping. */
	flags |= SOF_TIMESTAMPING_OPT_PKTINFO;
	for (has_pktinfo_flag = 1; has_pktinfo_flag >= 0; has_pktinfo_flag--) {
		if (setsockopt(ptpInterface->transport.eventSock, SOL_SOCKET,
			       SO_TIMESTAMPING, &flags, sizeof(flags)) == 0) {
			return TRUE;
		}
		flags &= ~SOF_TIMESTAMPING_OPT_PKTINFO;
	}

	return FALSE;
}


/**
 * Initialize timestamping of packets
 *
 * @param transport
 *
 * @return TRUE if successful
 */
Boolean
netInitTimestamping(PtpInterface *ptpInterface, InterfaceOpts *ifOpts)
{
	sfptpd_interface_ts_caps_t ts_caps;
	int flags;

	/* We want hardware timestamping. We need an interface that supports
	 * hardware timestamping and a hardware clock */
	ptpInterface->interface = ifOpts->physIface;
	if (ptpInterface->interface == NULL) {
		ERROR("error no interface object supplied\n");
		return false;
	}
	ts_caps = sfptpd_interface_ptp_caps(ptpInterface->interface);

	if (ifOpts->timestampType == PTPD_TIMESTAMP_TYPE_SW &&
	    ts_caps & SFPTPD_INTERFACE_TS_CAPS_SW) {

		/* SWPTP-145: on supporting kernels, SO_TIMESTAMPING enablement
		 * for software transmit timestamping succeeds regardless of
		 * whether the relevant net driver suports it. (Receive
		 * timestamping is performed in the stack.) This may not
		 * be a practical issue in 2024 but we nevertheless gate use
		 * of this method on the timestamping capabilities retrieved
		 * via ethtool or netlink and fall back to SO_TIMESTAMPNS
		 * if not available. */

		/* Try SO_TIMESTAMPING software timestamps */
		DBG("trying SO_TIMESTAMPING software timestamping...\n");

		/* Enable software transmit and receive timestamping */
		flags = SOF_TIMESTAMPING_TX_SOFTWARE
		      | SOF_TIMESTAMPING_RX_SOFTWARE
		      | SOF_TIMESTAMPING_SOFTWARE;

		if (netTryEnableTimestampingPktinfo(ptpInterface, flags)) {
			ptpInterface->tsMethod = TS_METHOD_SO_TIMESTAMPING;
			INFO("using SO_TIMESTAMPING software timestamps\n");
			return TRUE;
		}
	}

	if (ifOpts->timestampType == PTPD_TIMESTAMP_TYPE_SW) {

		/* Try SO_TIMESTAMPNS software timestamps */
		DBG("trying SO_TIMESTAMPNS software timestamping...\n");

		/* Enable software timestamps */
		flags = 1;

		if (setsockopt(ptpInterface->transport.eventSock, SOL_SOCKET,
			       SO_TIMESTAMPNS, &flags, sizeof(flags)) == 0) {
			ptpInterface->tsMethod = TS_METHOD_SYSTEM;
			INFO("using SO_TIMESTAMPNS software timestamps\n");
			return TRUE;
		}

		/* Try SO_TIMESTAMP software timestamps */
		DBG("trying SO_TIMESTAMP software timestamping...\n");

		/* Enable software timestamps */
		flags = 1;

		if (setsockopt(ptpInterface->transport.eventSock, SOL_SOCKET,
			       SO_TIMESTAMP, &flags, sizeof(flags)) == 0) {
			ptpInterface->tsMethod = TS_METHOD_SYSTEM;
			INFO("using SO_TIMESTAMP software timestamps\n");
			return TRUE;
		}

		ERROR("failed to configure software timestamping, %s\n",
		      strerror(errno));
		return FALSE;
	}

	/* Try Onload extensions API if available */
#ifdef HAVE_ONLOAD_EXT
	if (onload_is_present() && ptpInterface->ifOpts.use_onload_ext) {
		int flags = ONLOAD_TIMESTAMPING_FLAG_TX_NIC |
			    ONLOAD_TIMESTAMPING_FLAG_RX_NIC |
			    SOF_TIMESTAMPING_OPT_PKTINFO;
		int rc = onload_timestamping_request(ptpInterface->transport.eventSock,
						     flags);

		if (rc != 0) {
			flags &= ~SOF_TIMESTAMPING_OPT_PKTINFO;
			rc = onload_timestamping_request(ptpInterface->transport.eventSock,
							 flags);
		}

		if (rc == 0) {
			INFO("using Onload Extensions API NIC timestamps\n");
			ptpInterface->tsMethod = TS_METHOD_SO_TIMESTAMPING;
			ptpInterface->ts_fmt = PTPD_TS_ONLOAD_EXT;
			return TRUE;
		} else if (rc == ENOTTY) {
			INFO("using Onload but PTP event socket not accelerated\n");
		} else {
			ERROR("error trying to enable Onload timestamps, %s\n",
			      strerror(rc));
		}
	}
#endif

	/* Configure hardware timestamping */
	DBG("trying SO_TIMESTAMPING hardware timestamping...\n");

	/* Enable software transmit and receive timestamping */
	flags = SOF_TIMESTAMPING_TX_HARDWARE
	      | SOF_TIMESTAMPING_RX_HARDWARE
	      | SOF_TIMESTAMPING_RAW_HARDWARE;

	if (netTryEnableTimestampingPktinfo(ptpInterface, flags)) {
		ptpInterface->tsMethod = TS_METHOD_SO_TIMESTAMPING;
		INFO("using SO_TIMESTAMPING hardware timestamps\n");
		return TRUE;
	}

	ERROR("failed to configure hardware timestamping, %s\n",
	      strerror(errno));
	return FALSE;
}

/**
 * Create and bind a socket for listening
 *
 * @param purpose purpose for the socket
 * @param af addressing family
 * @param service the service name
 * @param saveAddr where to save a copy of the address
 * @param saveAddrLen where to save the address length
 *
 * @return the socket or -1 on failure
 */
int
netCreateBindSocket(const char *purpose, int af, const char *service,
		    struct sockaddr_storage *saveAddr, socklen_t *saveAddrLen) {
	int rc;
	int fd = -1;
	const int one = 1;
	struct addrinfo hints = {
		.ai_family = af,
		.ai_flags = AI_PASSIVE,
		.ai_socktype = SOCK_DGRAM,
		.ai_protocol = IPPROTO_UDP
	};
	struct addrinfo *result;

	rc = getaddrinfo(NULL, service, &hints, &result);
	if (rc == 0) {
		assert(result != NULL);

		/* create socket */
		fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
		if (fd != -1) {

			/* allow address reuse */
			if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
				       &one, sizeof one) < 0)
				DBG("failed to set socket reuse\n");

			/* bind to listening address */
			rc = bind(fd, result->ai_addr, result->ai_addrlen);
			if (rc == 0) {
				copyAddress(saveAddr, saveAddrLen,
					    (struct sockaddr_storage *) result->ai_addr,
					    result->ai_addrlen);
			} else {
				close(fd);
				fd = -1;
			}
		}
		if (fd == -1) {
			PERROR("failed to initialise %s socket", purpose);
		}
		freeaddrinfo(result);
	} else {
		ERROR("failed to get address for %s socket: %s\n",
		      purpose, gai_strerror(rc));
	}
	if (fd == -1) {
		*saveAddrLen = 0;
	}
	return fd;
}

static void sfptpd_ts_stats_init(struct sfptpd_ts_stats *stats)
{
	int quantile;

	/* Initialise stats */
	memset(stats, '\0', sizeof *stats);
	for (quantile = 0; quantile < TS_QUANTILES - 1; quantile++) {
		sfptpd_time_t bound = powl(10.0L, TS_QUANTILE_E10_MIN + quantile);
		sfptpd_time_float_s_to_timespec(bound, &stats->quantile_bounds[quantile]);
	}
	stats->quantile_bounds[TS_QUANTILES - 1] = sfptpd_time_max();
	sfclock_gettime(CLOCK_MONOTONIC, &stats->start);
};

static void sfptpd_ts_cache_init(struct sfptpd_ts_cache *cache)
{
	/* Initialise cache proper */
	cache->free_bitmap = ~0;
	cache->seq = 0;

	/* Initialise short term stats */
	sfptpd_ts_stats_init(&cache->stats_periodic);
	sfptpd_ts_stats_init(&cache->stats_adhoc);
};

/**
 * Init all network transports
 *
 * @param transport
 * @param rtOpts
 * @param ptpClock
 * 
 * @return TRUE if successful
 */
Boolean 
netInit(struct ptpd_transport * transport, InterfaceOpts * ifOpts, PtpInterface * ptpInterface)
{
	Boolean loopback_multicast;

	DBG("netInit\n");

	sfptpd_ts_cache_init(&ptpInterface->ts_cache);

	transport->generalSock = -1;
	transport->eventSock = -1;
	transport->monitoringSock = -1;

	/* Bug78221. We track the TTL value that we believe is configured on
	 * the socket. Initialise this to 1 (default multicast TTL) to ensure
	 * that we have the correct value in the case where we do not get as
	 * far as setting the configured TTL value. */
	transport->ttlEvent = 1;
	transport->ttlGeneral = 1;

	if (!netInitInterfaceInfo(transport, ifOpts)) {
		ERROR("failed to get interface info\n");
		return FALSE;
	}

	// TODO this code doesn't tidy up on failure!

	/* No HW address, we'll use the protocol address to form interfaceID -> clockID */
	if(!transport->interfaceInfo.hasHwAddress && transport->interfaceInfo.hasAfAddress) {
		switch(ifOpts->transportAF) {
		case AF_INET:
			{
				uint32_t addr = ((struct sockaddr_in*)&(transport->interfaceInfo.afAddress))->sin_addr.s_addr;
				memcpy(transport->interfaceID, (uint8_t *) &addr, 2);
				memcpy(transport->interfaceID + 4, ((uint8_t *) &addr) + 2, 2);
			}
			break;
		case AF_INET6:
			{
				uint8_t *addr = (uint8_t *) &((struct sockaddr_in6*)&(transport->interfaceInfo.afAddress))->sin6_addr;

				/* Use the host portion of the IPv6 address as the interface ID
				 * per 7.5.2.2.2 */
				memcpy(transport->interfaceID, addr + 8, 8);
			}
		}
	/* Initialise interfaceID with hardware address */
	} else {
		memcpy(&transport->interfaceID, &transport->interfaceInfo.hwAddress,
		       sizeof(transport->interfaceID) <= sizeof(transport->interfaceInfo.hwAddress) ?
		       sizeof(transport->interfaceID) : sizeof(transport->interfaceInfo.hwAddress)
		       );
	}

	/* save interface address for IGMP refresh */
	{
		copyAddress(&transport->interfaceAddr,
			    &transport->interfaceAddrLen,
			    &transport->interfaceInfo.afAddress,
			    transport->interfaceInfo.afAddressLen);
	}


	/* create and bind event socket */
	transport->eventSock = netCreateBindSocket("event",
						   ifOpts->transportAF,
						   DEFAULT_PTP_EVENT_PORT,
						   &transport->eventAddr,
						   &transport->eventAddrLen);
	if (transport->eventSock == -1)
		return FALSE;

	/* create and bind general socket */
	transport->generalSock = netCreateBindSocket("general",
						   ifOpts->transportAF,
						   DEFAULT_PTP_GENERAL_PORT,
						   &transport->generalAddr,
						   &transport->generalAddrLen);
	if (transport->generalSock == -1)
		return FALSE;

	/* create unbound socket for monitoring output */
	if ((transport->monitoringSock = socket(ifOpts->transportAF, SOCK_DGRAM, 0)) < 0) {
		PERROR("failed to initialise monitoring socket");
		return FALSE;
	}

	/* TODO: The information printed below is misleading for IPv6
	   because we actually use link local addressing not the adapter's
	   global unicast address. */
	address_display("Listening on IP", &transport->interfaceInfo.afAddress, transport->interfaceInfo.afAddressLen, FALSE);
	address_display("Local IP address used", &transport->interfaceAddr, transport->interfaceAddrLen, FALSE);

	/* Set socket dscp */
	if (ifOpts->dscpValue) {
		if (setsockopt(transport->eventSock, IPPROTO_IP, IP_TOS,
			       &ifOpts->dscpValue, sizeof(int)) < 0
		    || setsockopt(transport->generalSock, IPPROTO_IP, IP_TOS,
				  &ifOpts->dscpValue, sizeof(int)) < 0) {
			PERROR("failed to set socket DSCP bits");
			return FALSE;
		}
	}

	/* make timestamps available through recvmsg() */
	if (!netInitTimestamping(ptpInterface, ifOpts)) {
		ERROR("failed to enable packet time stamping\n");
		return FALSE;
	}

	if (ptpInterface->tsMethod != TS_METHOD_SYSTEM) {
		// TODO @bind do we still need to -not- bind to interface for
		// unicast loopback - test!
		/* The following code makes sure that the data is only received
		 * on the specified interface. Without this option, it's
		 * possible that PTP packets from another interface could be
		 * received and confuse the protocol. Note that we only do this
		 * for hardware timestamping because software timestamping
		 * needs to receive looped back packets from the transmit
		 * data path. */
		if (setsockopt(transport->eventSock, SOL_SOCKET, SO_BINDTODEVICE,
			       ifOpts->ifaceName, strlen(ifOpts->ifaceName)) < 0
		    || setsockopt(transport->generalSock, SOL_SOCKET, SO_BINDTODEVICE,
				  ifOpts->ifaceName, strlen(ifOpts->ifaceName)) < 0){
			PERROR("failed to call SO_BINDTODEVICE on the interface");
			return FALSE;
		}
	}

	if (ifOpts->multicast_needed) {
		/* init UDP Multicast on both Default and Peer addresses */
		if (!netInitMulticast(transport, ifOpts))
			return FALSE;

		/* set socket time-to-live  */
		if(!netSetMulticastTTL(transport->eventSock, ifOpts->ttl) ||
		   !netSetMulticastTTL(transport->generalSock, ifOpts->ttl))
			return FALSE;

		/* start tracking TTL */
		transport->ttlEvent = ifOpts->ttl;
		transport->ttlGeneral = ifOpts->ttl;
	}

	loopback_multicast = (ptpInterface->tsMethod == TS_METHOD_SYSTEM);
	if (!netSetMulticastLoopback(transport, loopback_multicast, ifOpts->transportAF))
		return FALSE;

	/* Compile ACLs */
	if (ifOpts->timingAclEnabled &&
	    ifOpts->transportAF == AF_INET) {
		freeIpv4AccessList(&transport->timingAcl);
		transport->timingAcl =
			createIpv4AccessList(ifOpts->timingAclAllowText,
					     ifOpts->timingAclDenyText,
					     ifOpts->timingAclOrder);
	}

	if (ifOpts->managementAclEnabled &&
	    ifOpts->transportAF == AF_INET) {
		freeIpv4AccessList(&transport->managementAcl);
		transport->managementAcl =
			createIpv4AccessList(ifOpts->managementAclAllowText,
					     ifOpts->managementAclDenyText,
					     ifOpts->managementAclOrder);
	}

	if (ifOpts->monitoringAclEnabled) {
		freeIpv4AccessList(&transport->monitoringAcl);
		transport->monitoringAcl =
			createIpv4AccessList(ifOpts->monitoringAclAllowText,
					     ifOpts->monitoringAclDenyText,
					     ifOpts->monitoringAclOrder);
	}
	return TRUE;
}

/**
 * store received data from network to "buf" , get and store the
 * SO_TIMESTAMP value in "time" for an event message
 *
 * @param buf
 * @param time
 * @param transport
 *
 * @return
 */
ssize_t
netRecvEvent(Octet *buf,
	     PtpInterface *ptpInterface,
	     struct sfptpd_ts_info *info)
{
	ssize_t ret;
	struct msghdr msg;
	struct iovec vec[1];
	struct ptpd_transport *transport = &ptpInterface->transport;

	union {
		struct cmsghdr cm;
		char control[512];
	} cmsg_un;

	reset_timestamp(info);

	vec[0].iov_base = buf;
	vec[0].iov_len = PACKET_SIZE;

	memset(&msg, 0, sizeof(msg));
	memset(buf, 0, PACKET_SIZE);
	memset(&cmsg_un, 0, sizeof(cmsg_un));

	msg.msg_name = (caddr_t) &transport->lastRecvAddr;
	msg.msg_namelen = sizeof transport->lastRecvAddr;
	msg.msg_iov = vec;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_un.control;
	msg.msg_controllen = sizeof(cmsg_un.control);
	msg.msg_flags = 0;

	ret = recvmsg(transport->eventSock, &msg, MSG_DONTWAIT | MSG_TRUNC);
	if (ret <= 0) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		return ret;
	}

	if (msg.msg_flags & MSG_TRUNC) {
		WARNING("dropped truncated incoming message (%zu -> %d)\n", ret, PACKET_SIZE);
		return 0;
	}

	/* get time stamp of packet */
	if (!info) {
		ERROR("null receive time stamp argument\n");
		return 0;
	}

	if (msg.msg_flags & MSG_CTRUNC) {
		ERROR("received truncated ancillary data\n");
		return 0;
	}

	/* Store the length of the address of sender */
	transport->lastRecvAddrLen = msg.msg_namelen;
	transport->receivedPackets++;

	getRxTimestamp(ptpInterface, buf, ret, &msg, info);
	return ret;
}


/**
 *
 * store received data from network to "buf" get and store the
 * SO_TIMESTAMP value in "time" for a general message
 *
 * @param buf
 * @param time
 * @param transport
 *
 * @return
 */
ssize_t
netRecvGeneral(Octet *buf, struct ptpd_transport *transport)
{
	ssize_t ret;
	struct msghdr msg;
	struct iovec vec[1];

	vec[0].iov_base = buf;
	vec[0].iov_len = PACKET_SIZE;

	memset(&msg, 0, sizeof(msg));
	memset(buf, 0, PACKET_SIZE);

	msg.msg_name = (caddr_t) &transport->lastRecvAddr;
	msg.msg_namelen = sizeof transport->lastRecvAddr;
	msg.msg_iov = vec;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	/* Receive datagram and store the sender's address -
	   used for Hybrid mode and Unix Domain Sockets */
	ret = recvmsg(transport->generalSock, &msg, MSG_DONTWAIT | MSG_TRUNC);
	if (ret <= 0) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		return ret;
	}

	/* Store the length of the address of sender */
	transport->lastRecvAddrLen = msg.msg_namelen;

	if (msg.msg_flags & MSG_TRUNC) {
		WARNING("dropped truncated incoming message (%zu -> %d)\n", ret, PACKET_SIZE);
		return 0;
	}

	DBGV("netRecvGeneral: rxed %zu bytes\n", ret);
	return ret;
}

/**
 * Receive error queue message
 */
ssize_t
netRecvError(PtpInterface *ptpInterface)
{
	struct msghdr *msg = &ptpInterface->msgEbuf;
	ssize_t len;

	msg->msg_controllen = CONTROL_MSG_SIZE;

	len = recvmsg(ptpInterface->transport.eventSock, msg, MSG_ERRQUEUE | MSG_DONTWAIT);

	if (len == -1)
		return -errno;

	/* Report if we get unexpected control message flags
	 * but still try parsing the ancillary data */
	if (msg->msg_flags != MSG_ERRQUEUE) {
		WARNING("Received %s ancillary data 0x%x\n",
		        msg->msg_flags | MSG_CTRUNC ? "truncated" : "invalid",
			msg->msg_flags);
	}

	DBGV("netRecvError: rxed %zd bytes\n", len);

	return len;
}


static int generateSendMessageControl(void **control, socklen_t *controllen,
				      int sockfd,
				      int request_tx_ifindex,
				      bool use_onload_ext)
{
	if (!controllen)
		return -EINVAL;

	*controllen = 0;

	if (request_tx_ifindex > 0) {
#ifdef HAVE_ONLOAD_EXT
		/* Onload includes functionality to try and send down a given
		 * ifindex, much like IP_PKTINFO, but allowing for physical
		 * interfaces rather than only logical. */
#define ONLOAD_FD_FEAT_TX_SCM_TS_PKTINFO 2
		int feat = ONLOAD_FD_FEAT_TX_SCM_TS_PKTINFO;
		if( onload_fd_check_feature(sockfd, feat) == 1 &&
		    use_onload_ext ) {
			struct scm_ts_pktinfo send_info = { 0 };
			struct cmsghdr *cmsg;

			if (!control || !*control)
				return -EINVAL;

			cmsg = (struct cmsghdr*)*control;
			send_info.if_index = request_tx_ifindex;
			cmsg->cmsg_level = SOL_SOCKET;
			cmsg->cmsg_type = SCM_TIMESTAMPING_PKTINFO;
			*(struct scm_ts_pktinfo*)(CMSG_DATA(cmsg)) = send_info;
			cmsg->cmsg_len = CMSG_LEN(sizeof(send_info));

			*controllen = cmsg->cmsg_len;
		}
#endif
	}

	/* We don't really need a NULL control, but it might help, particularly
	 * if running with onload, to skip some unnecessary logic. */
	if (*controllen == 0)
		*control = NULL;

	return 0;
}

/* Function that wraps up call to send message */
static int sendMessage(int sockfd, const void *buf, size_t length,
                       const struct sockaddr *addr, socklen_t addrLen,
                       const char *messageType, void *control,
                       socklen_t controllen)
{
	int rc;
	const static uint16_t zero = 0;
	struct iovec iov[2] = {
		{
			.iov_base = (void *) buf,
			.iov_len = length
		},
		{
			.iov_base = (void *) &zero,
			.iov_len = sizeof zero
		}
	};
	struct msghdr msg = {
		.msg_name = (struct sockaddr *) addr,
		.msg_namelen = addrLen,
		.msg_iov = iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = controllen
	};

	/* According to IEEE1588 Annex E.1, add two extra bytes
	   to the payload to help UDP checksum tweaking by
	   transparent clocks. */
	if (addr->sa_family == AF_INET6) {
		msg.msg_iovlen = 2;
		length += 2;
	}
	rc = sendmsg(sockfd, &msg, 0);

	if (rc < 0) {
		DBG("error sending %s message, %s\n", messageType, strerror(errno));
		return errno;
	}

	if (rc != length) {
		DBG("error sending %s message, sent %d bytes, expected %zu\n",
		    messageType, rc, length);
		return EIO;
	}

	return 0;
}

struct sfptpd_ts_ticket netExpectTimestamp(struct sfptpd_ts_cache *cache,
					   struct sfptpd_ts_user *user,
					   Octet *pkt_data,
					   size_t pkt_len,
					   size_t trailer)
{
	struct sfptpd_ts_pkt *pkt;
	char buf[48];
	int slot;
	int bit;

	if (cache->free_bitmap == 0) {

		/* Cache is full - evict oldest packet */
		uint64_t oldest = UINT64_MAX;
		int oldest_slot = 0;
		for (slot = 0; slot < TS_CACHE_SIZE; slot++) {
			if (cache->packet->seq < oldest) {
				oldest = cache->packet->seq;
				oldest_slot = slot;
			}
		}
		formatTsPkt(&cache->packet[slot].user, buf);
		DBGV("ptpd: timestamp cache full; evicting %s\n", buf);
		cache->free_bitmap |= ~((unsigned int) INT_MAX) >> oldest_slot;
		cache->stats_periodic.evicted++;
		cache->stats_adhoc.evicted++;
	}

	/* Find first free slot */
	slot = __builtin_clz(cache->free_bitmap);
	bit = TS_CACHE_SIZE - slot - 1;
	pkt = cache->packet + slot;

	/* Save match information */
	if (pkt_len > TS_MAX_PDU) {
		pkt->match.pdu.len = TS_MAX_PDU;
	} else {
		pkt->match.pdu.len = pkt_len;
	}
	memcpy(pkt->match.pdu.data, pkt_data,
	       pkt->match.pdu.len);
	pkt->match.pdu.trailer = trailer;

	/* Store packet information in cache */
	pkt->user = *user;
	pkt->seq = cache->seq++;
	pkt->has_caused_alarm = false;
	sfclock_gettime(CLOCK_MONOTONIC, &pkt->sent_monotime);
	cache->free_bitmap &= ~(1 << bit);
	cache->stats_periodic.total++;
	cache->stats_adhoc.total++;

	DBGV("ptpd: timestamp %" PRIu64 " request in slot %d\n", pkt->seq, slot);

	return (struct sfptpd_ts_ticket) {.slot = slot, .seq = pkt->seq};
}

// The function `netSendEvent` may use the SCM_TIMESTAMPING_PKTINFO control
// message, so we assert that we actually have enough space for this.
STATIC_ASSERT(sizeof(((PtpInterface*)NULL)->msgCbuf) >= CMSG_SPACE(sizeof(struct scm_ts_pktinfo)));

//
// alt_dst: alternative destination.
//   if filled, send to this unicast dest;
//   if zero, do the normal operation (send to unicast with -u, or send to the multicast group)
//
int
netSendEvent(Octet *buf, UInteger16 length, PtpClock *ptpClock,
	     RunTimeOpts *rtOpts, const struct sockaddr_storage *altDst,
	     socklen_t altDstLen, Integer32 request_tx_ifindex)
{
	int ret;
	struct sockaddr_storage addr;
	socklen_t addrLen;
	struct ptpd_transport *transport = &ptpClock->interface->transport;
	bool use_onload_ext = rtOpts->ifOpts->use_onload_ext;
	// This must be big enough to contain the SCM_TIMESTAMPING_PKTINFO cmsg
	// which we assert above this function.
	void *control = (void*)&ptpClock->interface->msgCbuf[0];
	socklen_t controllen = 0;

	if (ptpClock->unicastAddrLen != 0 || altDstLen != 0) {
		if (ptpClock->unicastAddrLen != 0) {
			copyAddress(&addr, &addrLen, &ptpClock->unicastAddr, ptpClock->unicastAddrLen);
		} else {
			copyAddress(&addr, &addrLen, altDst, altDstLen);
		}
		copyPort(&addr, &transport->eventAddr);

		/* If we're sending to a unicast address, set the UNICAST flag */
		*(char *)(buf + 6) |= PTPD_FLAG_UNICAST;

		ret = generateSendMessageControl(&control, &controllen,
						 transport->eventSock,
						 request_tx_ifindex,
						 use_onload_ext);
		if (ret != 0)
			return ret;

		ret = sendMessage(transport->eventSock, buf, length,
				  (struct sockaddr *)&addr, addrLen,
				  "unicast event", control, controllen);
		if (ret != 0)
			return ret;

		/* If doing timestamping in the IP stack, loop back packet */
		if (ptpClock->interface->tsMethod == TS_METHOD_SYSTEM) {
			/* Need to forcibly loop back the packet since
			 * we are not using multicast. */
			setLoopback(&addr, addrLen);
			ret = sendMessage(transport->eventSock, buf, length,
					  (struct sockaddr *)&addr, addrLen,
					  "loopback unicast event", NULL, 0);
		}
	} else if (transport->multicastAddrLen != 0) {
		copyAddress(&addr, &addrLen, &transport->multicastAddr, transport->multicastAddrLen);
		copyPort(&addr, &transport->eventAddr);

		/* If the socket has been used to send a peer-to-peer message,
		 * restore the multicast TTL to the default */
		if (transport->ttlEvent != rtOpts->ifOpts->ttl) {
			/* Try restoring TTL */
			if (netSetMulticastTTL(transport->eventSock,rtOpts->ifOpts->ttl)) {
				transport->ttlEvent = rtOpts->ifOpts->ttl;
			}
		}

		ret = generateSendMessageControl(&control, &controllen,
						 transport->eventSock,
						 request_tx_ifindex,
						 use_onload_ext);
		if (ret != 0)
			return ret;

		ret = sendMessage(transport->eventSock, buf, length,
				  (struct sockaddr *)&addr, addrLen,
				  "multicast event", control, controllen);
	} else {
		ret = EDESTADDRREQ;
	}

	if (ret == 0)
		transport->sentPackets++;

	return ret;
}


int
netSendGeneralImpl(Octet *buf, UInteger16 length, PtpClock *ptpClock,
		   RunTimeOpts *rtOpts, const struct sockaddr_storage *altDst,
		   socklen_t altDstLen, bool unbound, bool withPort)
{
	int ret;
	struct sockaddr_storage addr;
	socklen_t addrLen;
	struct ptpd_transport *transport = &ptpClock->interface->transport;

	if (ptpClock->unicastAddrLen != 0 || altDstLen != 0) {
		if (ptpClock->unicastAddrLen != 0) {
			copyAddress(&addr, &addrLen, &ptpClock->unicastAddr, ptpClock->unicastAddrLen);
		} else {
			assert(altDst != NULL);
			copyAddress(&addr, &addrLen, altDst, altDstLen);
		}
		if (!withPort || !isPortSet(&addr))
			copyPort(&addr, &transport->generalAddr);

		/* If we're sending to a unicast address, set the UNICAST flag */
		*(char *)(buf + 6) |= PTPD_FLAG_UNICAST;

		ret = sendMessage(unbound ? transport->monitoringSock : transport->generalSock,
				  buf, length,
				  (struct sockaddr *)&addr, addrLen,
				  "unicast general", NULL, 0);
	} else if (transport->multicastAddrLen != 0) {
		copyAddress(&addr, &addrLen, &transport->multicastAddr, transport->multicastAddrLen);
		copyPort(&addr, &transport->generalAddr);

		/* If the socket has been used to send a peer-to-peer message,
		 * restore the multicast TTL to the default */
		if (transport->ttlGeneral != rtOpts->ifOpts->ttl) {
			/* Try restoring TTL */
			if (netSetMulticastTTL(transport->generalSock,rtOpts->ifOpts->ttl)) {
				transport->ttlGeneral = rtOpts->ifOpts->ttl;
			}
		}

		ret = sendMessage(transport->generalSock, buf, length,
				  (struct sockaddr *)&addr, addrLen,
				  "multicast general", NULL, 0);
	} else {
		ret = EDESTADDRREQ;
	}

	if (ret == 0)
		transport->sentPackets++;
	return ret;
}


int
netSendGeneral(Octet *buf, UInteger16 length, PtpClock *ptpClock,
	       RunTimeOpts *rtOpts, const struct sockaddr_storage *altDst,
	       socklen_t altDstLen)
{
	return netSendGeneralImpl(buf, length, ptpClock, rtOpts, altDst, altDstLen, false, false);
}


int
netSendMonitoring(Octet *buf, UInteger16 length, PtpClock *ptpClock,
		  RunTimeOpts *rtOpts, const struct sockaddr_storage *altDst,
		  socklen_t altDstLen)
{
	return netSendGeneralImpl(buf, length, ptpClock, rtOpts, altDst, altDstLen, true, true);
}


int
netSendPeerGeneral(Octet *buf, UInteger16 length, PtpClock *ptpClock)
{
	int ret;
	struct sockaddr_storage addr;
	socklen_t addrLen;
	struct ptpd_transport *transport = &ptpClock->interface->transport;

	if (ptpClock->unicastAddrLen != 0) {
		copyAddress(&addr, &addrLen, &ptpClock->unicastAddr, ptpClock->unicastAddrLen);
		copyPort(&addr, &transport->generalAddr);

		/* If we're sending to a unicast address, set the UNICAST flag */
		*(char *)(buf + 6) |= PTPD_FLAG_UNICAST;

		ret = sendMessage(transport->generalSock, buf, length,
				  (struct sockaddr *)&addr, addrLen,
				  "unicast general", NULL, 0);
	} else if (transport->multicastAddrLen != 0) {
		copyAddress(&addr, &addrLen, &transport->multicastAddr, transport->multicastAddrLen);
		copyPort(&addr, &transport->generalAddr);

		/* Make sure the TTL is set to 1 for peer-to-peer multicast messages */
		if (transport->ttlGeneral != 1) {
			/* Try setting TTL to 1 */
			if (netSetMulticastTTL(transport->generalSock,1)) {
				transport->ttlGeneral = 1;
			}
		}

		ret = sendMessage(transport->generalSock, buf, length,
				  (struct sockaddr *)&addr, addrLen,
				  "multicast general", NULL, 0);
	} else {
		ret = EDESTADDRREQ;
	}

	if (ret == 0)
		transport->sentPackets++;
	return ret;
}


int
netSendPeerEvent(Octet *buf, UInteger16 length, PtpClock *ptpClock,
		 RunTimeOpts *rtOpts)
{
	int ret;
	struct sockaddr_storage addr;
	socklen_t addrLen;
	struct ptpd_transport *transport = &ptpClock->interface->transport;

	if (ptpClock->unicastAddrLen != 0) {
		copyAddress(&addr, &addrLen, &ptpClock->unicastAddr, ptpClock->unicastAddrLen);
		copyPort(&addr, &transport->eventAddr);

		/* If we're sending to a unicast address, set the UNICAST flag */
		*(char *)(buf + 6) |= PTPD_FLAG_UNICAST;

		ret = sendMessage(transport->eventSock, buf, length,
				  (struct sockaddr *)&addr, addrLen,
				  "unicast event", NULL, 0);
		if (ret != 0)
			return ret;

		/* If doing timestamping in the IP stack, loop back packet */
		if (ptpClock->interface->tsMethod == TS_METHOD_SYSTEM) {
			/* Need to forcibly loop back the packet since
			 * we are not using multicast. */
			setLoopback(&addr, addrLen);
			ret = sendMessage(transport->eventSock, buf, length,
					  (struct sockaddr *)&addr, addrLen,
					  "loopback unicast event", NULL, 0);
		}
	} else if (transport->peerMulticastAddrLen != 0) {
		copyAddress(&addr, &addrLen, &transport->peerMulticastAddr, transport->peerMulticastAddrLen);
		copyPort(&addr, &transport->eventAddr);

		/* Make sure the TTL is set to 1 for peer-to-peer multicast messages */
		if (transport->ttlEvent != 1) {
			/* Try setting TTL to 1 */
			if (netSetMulticastTTL(transport->eventSock,1)) {
				transport->ttlEvent = 1;
			}
		}

		ret = sendMessage(transport->eventSock, buf, length,
				  (struct sockaddr *)&addr, addrLen,
				  "multicast event", NULL, 0);
	} else {
		ret = EDESTADDRREQ;
	}

	if (ret == 0)
		transport->sentPackets++;

	return ret;
}
