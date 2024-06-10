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
#include "sfptpd_thread.h"
#include "ptpd.h"

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
	transport->bondSocksMcastResolutionMask = 0ull;
	transport->multicastBondSocksLen = 0;
}

void probeBondSocks(struct ptpd_transport *transport)
{
	struct sockaddr addr;
	struct msghdr msg = {
		.msg_name = (struct sockaddr *) &addr,
		.msg_namelen = 0,
		.msg_iov = NULL,
		.msg_iovlen = 0,
		.msg_control = NULL,
		.msg_controllen = 0
	};

	transport->bondSocksMcastResolutionMask = 0ull;
	transport->multicastBondSocksLen = 0;

	/* Short verion of `copyAddress` and `copyPort`, not worth
	 * forcing these to be in a wider scope than necessary. */
	memcpy(&addr, &transport->multicastAddr,
	       transport->multicastAddrLen);
	msg.msg_namelen = transport->multicastAddrLen;
	if (transport->eventAddr.ss_family == AF_INET)
		((struct sockaddr_in *)&addr)->sin_port =
			((struct sockaddr_in *)&transport->eventAddr)->sin_port;
	else
		((struct sockaddr_in6 *)&addr)->sin6_port =
			((struct sockaddr_in6 *)&transport->eventAddr)->sin6_port;

	/* We send an empty UDP packet as this should be trivially
	 * discarded by the other end, if this succeds then we make a
	 * note that we expect to see something on the error queue. */
	FOR_EACH_MASK_IDX(transport->bondSocksValidMask, idx)
		if (sendmsg(transport->bondSocks[idx], &msg, 0) == 0)
			transport->bondSocksMcastResolutionMask |= (1 << idx);
}

void bondSocksHandleMcastResolution(PtpInterface *ptpInterface)
{
	struct ptpd_transport *transport = &ptpInterface->transport;

	/* TODO: the current approach is somewhat out of place with the rest of
	 * the surrounding code. A better solution here would be to add these
	 * sockets to the epoll set and handle retrieval of the ifindex from
	 * the error queue alongside other data. This is perhaps already done,
	 * and would just require properly filling out the "iptype_pktinfo"
	 * section defined in `netProcessError`. */
	FOR_EACH_MASK_IDX(transport->bondSocksMcastResolutionMask, idx) {
		int i, ifindex;

		ifindex = netTryGetSockIfindex(ptpInterface, transport->bondSocks[idx]);
		/* Skip this socket if we don't know its ifindex yet. */
		if (ifindex <= 0)
			continue;

		/* Check that we haven't already found this ifindex. */
		for (i = 0; i < transport->multicastBondSocksLen; i++)
			if (transport->multicastBondSocks[i].ifindex == ifindex)
				break;

		/* If we have already found this ifindex, or we don't have
		 * enough space to store it, then remove this socket from the
		 * waiting pool. */
		const int maxLen = sizeof(transport->multicastBondSocks) /
				   sizeof(transport->multicastBondSocks[0]);
		if (i < transport->multicastBondSocksLen || i >= maxLen) {
			/* Don't clean up the socket as it's still valid! */
			transport->bondSocksMcastResolutionMask &= ~(1 << idx);
			continue;
		}

		/* Record the newly found (socket, ifindex) pair. */
		assert(i >= transport->multicastBondSocksLen);
		assert(i < maxLen);
		transport->multicastBondSocks[i].sockfd = transport->bondSocks[idx];
		transport->multicastBondSocks[i].ifindex = ifindex;
		transport->multicastBondSocksLen++;
		transport->bondSocksMcastResolutionMask &= ~(1 << idx);
		sfptpd_thread_user_fd_add(transport->bondSocks[idx], true, false);

		/* If our storage is full, then lets make sure we skip this
		 * discovery code in the future. */
		if (transport->multicastBondSocksLen >= maxLen)
			transport->bondSocksMcastResolutionMask = 0ull;
	}
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
