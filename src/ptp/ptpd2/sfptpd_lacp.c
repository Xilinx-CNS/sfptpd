/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

#ifdef __linux__
#include <asm/types.h>
#include <sys/socket.h>
#include <time.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#ifdef HAVE_ONLOAD_EXT
#include <onload/extensions.h>
#endif
#endif

#include "sfptpd_lacp.h"
#include "sfptpd_ptp_module.h"

static inline void invalidateBondBypassSocket(struct ptpd_transport *transport, int sockIdx)
{
	transport->bondSocksValidMask &= ~(1 << sockIdx);
	close(transport->bondSocks[sockIdx]);
	transport->bondSocks[sockIdx] = 0;
}

/* This macro is intended as a relatively general purpose iterator over a
 * bitmask, such that `copyMask |= (1 << idx)` will recreate `mask`. */
#define FOR_EACH_MASK_IDX(mask, idx) \
	for (uint64_t msk = (mask), idx = __builtin_ffsll(msk) - 1; \
	     msk != 0; \
	     msk &= ~(1 << idx), idx = __builtin_ffsll(msk) - 1)

void createBondSocks(struct ptpd_transport *transport, int transportAF)
{
	int i;
	int sockCount = transport->bond_info->num_physical_ifs *
			SFPTP_BOND_BYPASS_PER_INTF_SOCK_COUNT;
	struct sockaddr_in localAddr;
	int one = 1;
	int level = (transportAF == AF_INET) ? IPPROTO_IP : IPPROTO_IPV6;
	int opt = (transportAF == AF_INET) ? IP_PKTINFO : IPV6_RECVPKTINFO;

	/* If the current setup is not appropriate, then don't create the bond
	 * bypass sockets. Notably, we must be in an LACP bond with multiple
	 * slave interfaces using layer3+4 hashing. */
	/* TODO: also check the bond is using layer3+4 hashing, this will
	 * require work wiring `IFLA_BOND_XMIT_HASH_POLICY` through into
	 * `bond_info` from the netlink code. */
	if (!transport->bond_info ||
	    transport->bond_info->bond_mode != SFPTPD_BOND_MODE_LACP ||
	    transport->bond_info->num_physical_ifs <= 1)
		return;

	memcpy((struct sockaddr_storage*)&localAddr, &transport->interfaceAddr,
	       transport->interfaceAddrLen);
	localAddr.sin_port = 0;

	assert(transport->bondSocksValidMask == 0);
	/* Because we use a mask for valid fds, we must have at most 64 fds. */
	static_assert(SFPTP_BOND_BYPASS_SOCK_COUNT <= 64,
		      "socket count fits in bitfield");
	/* This doesn't really need to be an assertion, if we want to increase
	 * the number of sockets per interface then we could just take the
	 * minimum of sockCount and SFPTP_BOND_BYPASS_SOCK_COUNT. Although the
	 * performance for many interfaces would degrade after the maximum. */
	assert(sockCount <= SFPTP_BOND_BYPASS_SOCK_COUNT);
	for (i = 0; i < sockCount; i++) {
		int sockfd, rc;

		assert(transport->bondSocks[i] == 0);

		sockfd = socket(transportAF, SOCK_DGRAM, 0);
		if (sockfd < 0)
			continue;

		rc = bind(sockfd, &localAddr, sizeof(localAddr));
		if (rc < 0) {
			close(sockfd);
			continue;
		}

		rc = setsockopt(sockfd, level, opt, &one, sizeof(one));
		if (rc < 0) {
			close(sockfd);
			continue;
		}

		transport->bondSocks[i] = sockfd;
		transport->bondSocksValidMask |= (1 << i);
	}
}

void destroyBondSocks(struct ptpd_transport *transport)
{
	FOR_EACH_MASK_IDX(transport->bondSocksValidMask, idx)
		invalidateBondBypassSocket(transport, idx);

	/* Should already be true but doesn't hurt to do it here too */
	transport->bondSocksValidMask = 0ull;
}

void setBondSockopt(struct ptpd_transport *transport, int level, int optname,
		    const void *optval, socklen_t optlen)
{
	FOR_EACH_MASK_IDX(transport->bondSocksValidMask, idx)
		if (setsockopt(transport->bondSocks[idx], level, optname,
			       optval, optlen) < 0)
			invalidateBondBypassSocket(transport, idx);
}

void copyMulticastTTLToBondSocks(struct ptpd_transport *transport)
{
	int temp = transport->ttlEvent;
	setBondSockopt(transport, IPPROTO_IP, IP_MULTICAST_TTL,
		       &temp, sizeof(temp));
}

void copyTimestampingToBondSocks(struct ptpd_transport *transport,
				 TsSetupMethod *tsSetupMethod)
{
	int flags = tsSetupMethod->flags | SOF_TIMESTAMPING_OPT_CMSG;
#ifdef HAVE_ONLOAD_EXT
	if (tsSetupMethod->is_onload) {
		FOR_EACH_MASK_IDX(transport->bondSocksValidMask, i)
			if (onload_timestamping_request(transport->bondSocks[i],
							flags) != 0)
				invalidateBondBypassSocket(transport, i);
	}
	else
#endif
	{
		setBondSockopt(transport, SOL_SOCKET, tsSetupMethod->sockopt,
			       &flags, sizeof(flags));
	}
}
