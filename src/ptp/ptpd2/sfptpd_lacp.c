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
		union bond_sock_invalid_description *invalid_desc =
			&transport->bondSocksInvalidDescriptions[i];

		assert(transport->bondSocks[i] == 0);

		sockfd = socket(transportAF, SOCK_DGRAM, 0);
		if (sockfd < 0) {
			invalid_desc->socket.reason =
				BOND_SOCK_INVALID_REASON_SOCKET;
			invalid_desc->socket.rc = sockfd;
			continue;
		}

		rc = bind(sockfd, &localAddr, sizeof(localAddr));
		if (rc < 0) {
			invalid_desc->bind.reason =
				BOND_SOCK_INVALID_REASON_BIND;
			invalid_desc->bind.rc = rc;
			close(sockfd);
			continue;
		}

		rc = setsockopt(sockfd, level, opt, &one, sizeof(one));
		if (rc < 0) {
			invalid_desc->setsockopt.reason =
				BOND_SOCK_INVALID_REASON_SETSOCKOPT;
			invalid_desc->setsockopt.rc = rc;
			invalid_desc->setsockopt.level = level;
			invalid_desc->setsockopt.sockopt = opt;
			close(sockfd);
			continue;
		}

		rc = sfptpd_thread_user_fd_add(sockfd, true, false);
		if (rc != 0) {
			invalid_desc->add_to_epoll_set.reason =
				BOND_SOCK_INVALID_REASON_ADD_TO_EPOLL_SET;
			invalid_desc->add_to_epoll_set.rc = rc;
			close(sockfd);
			continue;
		}

		transport->bondSocks[i] = sockfd;
		transport->bondSocksValidMask |= (1 << i);
	}

	/* We want to include even those that might have failed for the sake of
	 * being able to log which sockets have been invalidated. */
	transport->bondSocksCreatedCount = sockCount;
}

void destroyBondSocks(struct ptpd_transport *transport)
{
	FOR_EACH_MASK_IDX(transport->bondSocksValidMask, idx) {
		union bond_sock_invalid_description *invalid_desc =
			&transport->bondSocksInvalidDescriptions[idx];

		invalidateBondBypassSocket(transport, idx);

		invalid_desc->shutdown.reason =
			BOND_SOCK_INVALID_REASON_SHUTDOWN;
	}

	/* Should already be true but doesn't hurt to do it here too */
	transport->bondSocksValidMask = 0ull;
	transport->multicastBondSocksLen = 0;
}

void probeBondSocks(struct ptpd_transport *transport)
{
	struct sockaddr_storage addr;
	struct msghdr msg = {
		.msg_name = (struct sockaddr *) &addr,
		.msg_namelen = 0,
		.msg_iov = NULL,
		.msg_iovlen = 0,
		.msg_control = NULL,
		.msg_controllen = 0
	};

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
		sendmsg(transport->bondSocks[idx], &msg, 0);
}

void bondSocksOnTxIfindex(struct ptpd_transport *transport, int sockfd, int ifindex)
{
	struct socket_ifindex *mapElement;
	int i;

	/* First check if the socket is one we already are sending over, and
	 * verify that we are getting the correct tx ifindex. */
	mapElement = bondSockFindMulticastMappingByFd(transport, sockfd);
	if (mapElement != NULL && mapElement->ifindex != ifindex)
	{
		transport->multicastBondSocksLen = 0;
		probeBondSocks(transport);
	}

	/* If this sockfd isn't in our mapping already, then lets update our
	 * mapping to include it and the ifindex. */
	for (i = 0; i < transport->multicastBondSocksLen; i++)
		if (transport->multicastBondSocks[i].ifindex == ifindex)
			break;

	/* If we have already found this ifindex, or we don't have
	 * enough space to store it, then remove this socket from the
	 * waiting pool. */
	const int maxLen = sizeof(transport->multicastBondSocks) /
			   sizeof(transport->multicastBondSocks[0]);
	if (i < transport->multicastBondSocksLen || i >= maxLen) {
		return;
	}

	/* Record the newly found (socket, ifindex) pair. */
	assert(i >= transport->multicastBondSocksLen);
	assert(i < maxLen);
	transport->multicastBondSocks[i].sockfd = sockfd;
	transport->multicastBondSocks[i].ifindex = ifindex;
	transport->multicastBondSocksLen++;
}

struct socket_ifindex*
bondSockFindMulticastMappingByFd(struct ptpd_transport *transport, int sockfd)
{
	int i;

	for (i = 0; i < transport->multicastBondSocksLen; i++)
		if (transport->multicastBondSocks[i].sockfd == sockfd)
			return &transport->multicastBondSocks[i];

	return NULL;
}

int bondSockFdIndexInSockPool(struct ptpd_transport *transport, int sockfd)
{
	FOR_EACH_MASK_IDX(transport->bondSocksValidMask, idx)
		if (transport->bondSocks[idx] == sockfd)
			return idx;

	return -1;
}

void setBondSockopt(struct ptpd_transport *transport, int level, int optname,
		    const void *optval, socklen_t optlen)
{
	int rc;
	FOR_EACH_MASK_IDX(transport->bondSocksValidMask, idx) {
		if ((rc = setsockopt(transport->bondSocks[idx], level, optname,
			       optval, optlen)) < 0) {
			union bond_sock_invalid_description *invalid_desc =
				&transport->bondSocksInvalidDescriptions[idx];

			invalidateBondBypassSocket(transport, idx);

			invalid_desc->setsockopt.reason =
				BOND_SOCK_INVALID_REASON_SETSOCKOPT;
			invalid_desc->setsockopt.rc = rc;
			invalid_desc->setsockopt.level = level;
			invalid_desc->setsockopt.sockopt = optname;
		}

	}
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
	setBondSockopt(transport, SOL_SOCKET, tsSetupMethod->sockopt,
		       &flags, sizeof(flags));
}

#define BOND_SOCK_INVALID_REASON_STRLEN 39
void getBondSockInvalidReason(struct ptpd_transport *transport, int idx,
			      char *reason)
{
	union bond_sock_invalid_description *invalid_desc =
		&transport->bondSocksInvalidDescriptions[idx];

	switch (invalid_desc->anonymous.reason) {
	case BOND_SOCK_INVALID_REASON_SOCKET:
		snprintf(reason, BOND_SOCK_INVALID_REASON_STRLEN,
			 "INVALID: socket = %d",
			 invalid_desc->socket.rc);
		break;
	case BOND_SOCK_INVALID_REASON_BIND:
		snprintf(reason, BOND_SOCK_INVALID_REASON_STRLEN,
			 "INVALID: bind = %d",
			 invalid_desc->bind.rc);
		break;
	case BOND_SOCK_INVALID_REASON_SETSOCKOPT:
		snprintf(reason, BOND_SOCK_INVALID_REASON_STRLEN,
			 "INVALID: setsockopt(%d, %d) = %d",
			 invalid_desc->setsockopt.level,
			 invalid_desc->setsockopt.sockopt,
			 invalid_desc->setsockopt.rc);
		break;
	case BOND_SOCK_INVALID_REASON_ADD_TO_EPOLL_SET:
		snprintf(reason, BOND_SOCK_INVALID_REASON_STRLEN,
			 "INVALID: add_to_epoll = %d",
			 invalid_desc->add_to_epoll_set.rc);
		break;
	case BOND_SOCK_INVALID_REASON_SHUTDOWN:
		snprintf(reason, BOND_SOCK_INVALID_REASON_STRLEN,
			 "INVALID: shutdown");
		break;
	default:
		snprintf(reason, BOND_SOCK_INVALID_REASON_STRLEN,
			 "INVALID: unknown reason");
		break;
	}
}

void bondSocksDumpState(struct ptpd_intf_context *intf, int sev)
{
	const char *header[] = { "#", "fd", "port", "mcast ifindex" };
	const char *formatHeader = "| %-2s | %-10s | %-10s | %-13s |\n";
	const char *formatRecordValid = "| %-2d | %-10d | %-10d | %-13d |\n";
	const char *formatRecordInvalid =
		"| %-2d | %-" STRINGIFY(BOND_SOCK_INVALID_REASON_STRLEN) "s |\n";
	const char *separator =
		"|----+------------+------------+---------------|\n";
	char invalidReason[BOND_SOCK_INVALID_REASON_STRLEN];
	struct ptpd_transport *transport = &intf->transport;
	int i;

	if (transport->bondSocksCreatedCount <= 0)
		return;

	TRACE_LX(sev, "LACP bypass sockets state for interface %s:\n",
		 transport->bond_info->bond_if);
	TRACE_LX(sev, formatHeader, header[0], header[1], header[2], header[3]);
	TRACE_LX(sev, separator);
	for (i = 0; i < transport->bondSocksCreatedCount; i++) {
		if (transport->bondSocksValidMask & (1 << i)) {
			int fd = transport->bondSocks[i];
			struct sockaddr_in addr;
			socklen_t addrlen = sizeof(addr);
			int getsockname_rc;
			struct socket_ifindex *mapping;

			getsockname_rc = getsockname(fd, &addr, &addrlen);
			mapping = bondSockFindMulticastMappingByFd(transport, fd);

			TRACE_LX(sev, formatRecordValid, i, fd,
				 (getsockname_rc == 0) ? ntohs(addr.sin_port) : 0,
				 (mapping) ? mapping->ifindex : 0);
		} else {
			getBondSockInvalidReason(transport, i, invalidReason);
			TRACE_LX(sev, formatRecordInvalid, i, invalidReason);
		}
	}
}
