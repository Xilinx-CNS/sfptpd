/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_LACP_H
#define _SFPTPD_LACP_H

/* This file, and its corresponding implementation file, contain an approach to
 * bypass an LACP bond. We may want to do this, for example, if our bond covers
 * two NICs, each of which are connected to separate boundary clocks.
 *
 * TODO: ASCII diagram for context?
 *
 * In such a situation, we may experience the following failure mode:
 * 1. We select GM_B as our time source
 * 2. A Sync message is issued from GM_B (through BC_B)
 * 3. We send a Delay_Req over the bond, which is routed to BC_A (not BC_B!)
 * 4. BC_A responds with a Delay_Resp, which we ignore as the clock identifier
 *    for GM_A is different to the one we expect from GM_B
 *
 * The main problem is step (3), which this bypass attempts to address by
 * enabling us to specify the ifindex of the interface we wish to send over.
 * In this case, we specifically use the ifindex of the interface that last
 * received a Sync message.
 *
 * The implementation of this bypass takes advantage of a few things working
 * together, but the main design idea is that we allocate a pool of sockets,
 * each with a different port (probably assigned ephemerally). Then if a bond
 * uses layer3+4 hashing, we would expect each of these sockets to use a fixed,
 * different interface for physical transport. We can then probe these sockets
 * to identify the exact ifindex it uses for a given destination (IP, port)
 * pair. Then the next time we want to send to that desination, we can simply
 * lookup which socket to use to send over a specific ifindex.
 *
 * Limitations:
 * - The bond must be an LACP bond using layer3+4 hashing, no other scheme will
 *   work (as conveniently) unless we add many IPs to the bond for a layer3
 *   solution, or many MAC addresses for the layer2 solution.
 * - It is possible, although unlikely, that we are unable to create (enough)
 *   sockets for the bypass solution to work. The current implementation uses
 *   4x the number of physical interfaces, meaning the probability of one
 *   physical interface not being covered is = (n-1/n)^(4n) where n is the
 *   number of physical interfaces in the bond. This function increases with n,
 *   so the worst-case scenario is a 1.6% chance of failure with n=16.
 * - We end up consuming quite a few socket fds and ports as these are only
 *   freed if they are invalid, or when we are shutting down. This is probably
 *   fine in most cases, but there are surely some cases where these are
 *   limited resources.
 * - It's possible that the probing step may not complete before we want to use
 *   the sockets to send over a specific ifindex. In these cases, we still are
 *   not able to guarantee the Delay_Req is sent to the intended receiver.
 * - The current implementation can only handle multicast addresses as these
 *   are known to be fixed for PTP, unlike sfptpd's implementation of hybrid
 *   mode which can have unicast addresses change at any point without needing
 *   to netShutdown and netInit again (reasonably so).
 * - This solution may be incompatible with other implementations of PTP that
 *   expect the event source port to be that which is defined in the standard
 *   (319 as of writing). While there is no formal specification for the source
 *   port of a message, it isn't unreasonable that some solutions might expect
 *   the source port to match the destination port.
 */

struct ptpd_transport;
typedef struct TsSetupMethod_s TsSetupMethod;
typedef struct ptpd_intf_context PtpInterface;

#define SFPTPD_BOND_BYPASS_USE_SOCKPOOL (1 << 0)
#define SFPTPD_BOND_BYPASS_USE_CMSG (1 << 1)
#define SFPTPD_BOND_BYPASS_USE_BOTH (SFPTPD_BOND_BYPASS_USE_SOCKPOOL | \
				     SFPTPD_BOND_BYPASS_USE_CMSG)

void createBondSocks(struct ptpd_transport *transport, int transportAF);
void destroyBondSocks(struct ptpd_transport *transport);

void probeBondSocks(struct ptpd_transport *transport);
void bondSocksHandleMcastResolution(PtpInterface *ptpInterface);

void setBondSockopt(struct ptpd_transport *transport, int level, int optname,
		    const void *optval, socklen_t optlen);
void copyMulticastTTLToBondSocks(struct ptpd_transport *transport);
void copyTimestampingToBondSocks(struct ptpd_transport *transport,
				 TsSetupMethod *tsMethodVerbose);

#endif /* _SFPTPD_LACP_H */
