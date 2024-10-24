/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_ntpd_mode7_client.c
 * @brief  Interface to NTP daemon
 */

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_time.h"
#include "sfptpd_misc.h"

#include "ntpengine/ntp_isc_md5.h"

#include "sfptpd_ntpd_client.h"
#include "sfptpd_ntpd_client_impl.h"


/****************************************************************************
 * Macros
 ****************************************************************************/

/* NTP component specific trace */
#define DBG_L1(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 1, x, ##__VA_ARGS__)
#define DBG_L2(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 2, x, ##__VA_ARGS__)
#define DBG_L3(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 3, x, ##__VA_ARGS__)
#define DBG_L4(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 4, x, ##__VA_ARGS__)
#define DBG_L5(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 5, x, ##__VA_ARGS__)
#define DBG_L6(x, ...)  TRACE(SFPTPD_COMPONENT_ID_NTP, 6, x, ##__VA_ARGS__)


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/****************************************************************************
 * NTP Definitions
 * 
 * The reference for the following definitions is the ntp source code
 * ntp-4.2.6p5 in the following files:
 *   include/ntp_request.h
 *   include/ntp_types.h
 *   include/ntp_fp.h
 *   include/ntp.h
 * 
 ****************************************************************************/

/* Cryptographic key ID */
typedef int32_t keyid_t;

/* Maximum length of MAC */
#define MAX_MAC_LEN     (6 * sizeof(uint32_t))   /* SHA */

/*
 * Values for peer mode and packet mode. Only the modes through
 * MODE_BROADCAST and MODE_BCLIENT appear in the transition
 * function. MODE_CONTROL and MODE_PRIVATE can appear in packets,
 * but those never survive to the transition function.
 * is a
 */
#define	MODE_UNSPEC	0	/* unspecified (old version) */
#define	MODE_ACTIVE	1	/* symmetric active mode */
#define	MODE_PASSIVE	2	/* symmetric passive mode */
#define	MODE_CLIENT	3	/* client mode */
#define	MODE_SERVER	4	/* server mode */
#define	MODE_BROADCAST	5	/* broadcast mode */

/*
 * NTP uses two fixed point formats.  The first (l_fp) is the "long"
 * format and is 64 bits long with the decimal between bits 31 and 32.
 * This is used for time stamps in the NTP packet header (in network
 * byte order) and for internal computations of offsets (in local host
 * byte order). We use the same structure for both signed and unsigned
 * values, which is a big hack but saves rewriting all the operators
 * twice. Just to confuse this, we also sometimes just carry the
 * fractional part in calculations, in both signed and unsigned forms.
 * Anyway, an l_fp looks like:
 *
 *    0			  1		      2			  3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |			       Integral Part			     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |			       Fractional Part			     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
typedef struct {
	union {
		uint32_t Xl_ui;
		int32_t Xl_i;
	} Ul_i;
	union {
		uint32_t Xl_uf;
		int32_t Xl_f;
	} Ul_f;
} l_fp;

#define l_ui    Ul_i.Xl_ui              /* unsigned integral part */
#define l_i     Ul_i.Xl_i               /* signed integral part */
#define l_uf    Ul_f.Xl_uf              /* unsigned fractional part */
#define l_f     Ul_f.Xl_f               /* signed fractional part */

#define M_ADD(r_i, r_f, a_i, a_f)       /* r += a */ \
	do { \
		register uint32_t lo_tmp; \
		register uint32_t hi_tmp; \
		\
		lo_tmp = ((r_f) & 0xffff) + ((a_f) & 0xffff); \
		hi_tmp = (((r_f) >> 16) & 0xffff) + (((a_f) >> 16) & 0xffff); \
		if (lo_tmp & 0x10000) \
			hi_tmp++; \
		(r_f) = ((hi_tmp & 0xffff) << 16) | (lo_tmp & 0xffff); \
		\
		(r_i) += (a_i); \
		if (hi_tmp & 0x10000) \
		(r_i)++; \
	} while (0)


#define L_ADD(r, a)     M_ADD((r)->l_ui, (r)->l_uf, (a)->l_ui, (a)->l_uf)

#define HTONL_FP(h, n) do { (n)->l_ui = htonl((h)->l_ui); \
		    (n)->l_uf = htonl((h)->l_uf); } while (0)

/*
 * The second fixed point format is 32 bits, with the decimal between
 * bits 15 and 16.  There is a signed version (s_fp) and an unsigned
 * version (u_fp).  This is used to represent synchronizing distance
 * and synchronizing dispersion in the NTP packet header (again, in
 * network byte order) and internally to hold both distance and
 * dispersion values (in local byte order).  In network byte order
 * it looks like:
 *
 *    0			  1		      2			  3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |		  Integer Part	     |	   Fraction Part	     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
typedef int32_t s_fp;
typedef uint32_t u_fp;

/*
 * A unit second in fp format.	Actually 2**(half_the_bits_in_a_long)
 */
#define	FP_SECOND	(0x10000)

/*
 * A mode 7 packet is used exchanging data between an NTP server
 * and a client for purposes other than time synchronization, e.g.
 * monitoring, statistics gathering and configuration.  A mode 7
 * packet has the following format:
 *
 *    0			  1		      2			  3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |R|M| VN  | Mode|A|  Sequence   | Implementation|   Req Code    |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |  Err  | Number of data items  |  MBZ  |   Size of data item   |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |								     |
 *   |            Data (Minimum 0 octets, maximum 500 octets)        |
 *   |								     |
 *                            [...]
 *   |								     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |               Encryption Keyid (when A bit set)               |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |								     |
 *   |          Message Authentication Code (when A bit set)         |
 *   |								     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * where the fields are (note that the client sends requests, the server
 * responses):
 *
 * Response Bit:  This packet is a response (if clear, packet is a request).
 *
 * More Bit:	Set for all packets but the last in a response which
 *		requires more than one packet.
 *
 * Version Number: 2 for current version
 *
 * Mode:	Always 7
 *
 * Authenticated bit: If set, this packet is authenticated.
 *
 * Sequence number: For a multipacket response, contains the sequence
 *		number of this packet.  0 is the first in the sequence,
 *		127 (or less) is the last.  The More Bit must be set in
 *		all packets but the last.
 *
 * Implementation number: The number of the implementation this request code
 *		is defined by.  An implementation number of zero is used
 *		for requst codes/data formats which all implementations
 *		agree on.  Implementation number 255 is reserved (for
 *		extensions, in case we run out).
 *
 * Request code: An implementation-specific code which specifies the
 *		operation to be (which has been) performed and/or the
 *		format and semantics of the data included in the packet.
 *
 * Err:		Must be 0 for a request.  For a response, holds an error
 *		code relating to the request.  If nonzero, the operation
 *		requested wasn't performed.
 *
 *		0 - no error
 *		1 - incompatable implementation number
 *		2 - unimplemented request code
 *		3 - format error (wrong data items, data size, packet size etc.)
 *		4 - no data available (e.g. request for details on unknown peer)
 *		5-6 I don't know
 *		7 - authentication failure (i.e. permission denied)
 *
 * Number of data items: number of data items in packet.  0 to 500
 *
 * MBZ:		A reserved data field, must be zero in requests and responses.
 *
 * Size of data item: size of each data item in packet.  0 to 500
 *
 * Data:	Variable sized area containing request/response data.  For
 *		requests and responses the size in octets must be greater
 *		than or equal to the product of the number of data items
 *		and the size of a data item.  For requests the data area
 *		must be exactly 40 octets in length.  For responses the
 *		data area may be any length between 0 and 500 octets
 *		inclusive.
 *
 * Message Authentication Code: Same as NTP spec, in definition and function.
 *		May optionally be included in requests which require
 *		authentication, is never included in responses.
 *
 * The version number, mode and keyid have the same function and are
 * in the same location as a standard NTP packet.  The request packet
 * is the same size as a standard NTP packet to ease receive buffer
 * management, and to allow the same encryption procedure to be used
 * both on mode 7 and standard NTP packets.  The mac is included when
 * it is required that a request be authenticated, the keyid should be
 * zero in requests in which the mac is not included.
 *
 * The data format depends on the implementation number/request code pair
 * and whether the packet is a request or a response.  The only requirement
 * is that data items start in the octet immediately following the size
 * word and that data items be concatenated without padding between (i.e.
 * if the data area is larger than data_items*size, all padding is at
 * the end).  Padding is ignored, other than for encryption purposes.
 * Implementations using encryption might want to include a time stamp
 * or other data in the request packet padding.  The key used for requests
 * is implementation defined, but key 15 is suggested as a default.
 */

/* NTP request packet. These are almost a fixed length.
 * @rm_vn_mode: response, more, version, mode
 * @auth_seq: key, sequence number
 * @implementation: implementation number
 * @request: request number
 * @err_nitems: error code/number of data items
 * @mbz_itemsize: item size
 * @data: data area [32 prev](176 byte max)
 * @tstamp: timestamp for authentication
 * @keyid: (optional) encryption key
 * @mac: (optional) auth code
 */
struct ntp_request_pkt {
	uint8_t rm_vn_mode;
	uint8_t auth_seq;
	uint8_t implementation;
	uint8_t request;
	uint16_t err_nitems;
	uint16_t mbz_itemsize;
	uint8_t data[128 + 48];
	uint32_t tstamp;
	keyid_t keyid;
	char mac[MAX_MAC_LEN - sizeof(keyid_t)];
};

/* MODE_PRIVATE request packet header length before optional items. */
#define REQ_LEN_HDR	(offsetof(struct ntp_request_pkt, data))
/* MODE_PRIVATE request packet fixed length without MAC. */
#define REQ_LEN_NOMAC	(offsetof(struct ntp_request_pkt, keyid))
/* MODE_PRIVATE req_pkt_tail minimum size (16 octet digest) */

#define RESP_HEADER_SIZE	(offsetof(struct ntp_response_pkt, data))
#define RESP_DATA_SIZE		(500)

/* A MODE_PRIVATE response packet. The length here is variable, this is the
 * maximum size. Note that this implementation doesn't authenticate responses.
 * @rm_vn_mode: response, more, version, mode
 * @auth_seq: key, sequence number
 * @implementation: implementation number
 * @request: request number
 * @err_nitems: error code/number of data items
 * @mbz_itemsize: item size
 * @data: data area
 */
struct ntp_response_pkt {
	uint8_t rm_vn_mode;
	uint8_t auth_seq;
	uint8_t implementation;
	uint8_t request;
	uint16_t err_nitems;
	uint16_t mbz_itemsize;
	uint8_t data[RESP_DATA_SIZE];
};


/* Information error codes */
#define INFO_OKAY	0
#define INFO_ERR_IMPL	1	/* incompatable implementation */
#define INFO_ERR_REQ	2	/* unknown request code */
#define INFO_ERR_FMT	3	/* format error */
#define INFO_ERR_NODATA	4	/* no data for this request */
#define INFO_ERR_AUTH	7	/* authentication failure */
#define INFO_ERR_MAX	8

/* Maximum sequence number */
#define MAXSEQ	127

/* Bit setting macros for multifield items */
#define RESP_BIT	0x80
#define MORE_BIT	0x40

#define ISRESPONSE(rm_vn_mode)	(((rm_vn_mode)&RESP_BIT)!=0)
#define ISMORE(rm_vn_mode)	(((rm_vn_mode)&MORE_BIT)!=0)
#define INFO_VERSION(rm_vn_mode) ((uint8_t)(((rm_vn_mode)>>3)&0x7))
#define INFO_MODE(rm_vn_mode)	((rm_vn_mode)&0x7)

#define RM_VN_MODE(resp, more, version)		\
				((uint8_t)(((resp)?RESP_BIT:0)\
				|((more)?MORE_BIT:0)\
				|((version?version:(NTP_OLDVERSION+1))<<3)\
				|(MODE_PRIVATE)))

#define INFO_IS_AUTH(auth_seq)	(((auth_seq) & 0x80) != 0)
#define INFO_SEQ(auth_seq)	((auth_seq)&0x7f)
#define AUTH_SEQ(auth, seq)	((uint8_t)((((auth)!=0)?0x80:0)|((seq)&0x7f)))

#define INFO_ERR(err_nitems)	((uint16_t)((ntohs(err_nitems)>>12)&0xf))
#define INFO_NITEMS(err_nitems)	((uint16_t)(ntohs(err_nitems)&0xfff))
#define ERR_NITEMS(err, nitems)	(htons((uint16_t)((((u_short)(err)<<12)&0xf000)\
				|((uint16_t)(nitems)&0xfff))))

#define INFO_MBZ(mbz_itemsize)		((ntohs(mbz_itemsize)>>12)&0xf)
#define INFO_ITEMSIZE(mbz_itemsize)	((uint16_t)(ntohs(mbz_itemsize)&0xfff))
#define MBZ_ITEMSIZE(itemsize)		(htons((uint16_t)(itemsize)))

/* Implementation numbers. One for universal use and one for ntpd */
#define IMPL_UNIV	0
#define IMPL_XNTPD_OLD	2	/* Used by pre ipv6 ntpclient */
#define IMPL_XNTPD	3	/* Used by post ipv6 ntpclient */

/* Some limits related to authentication.  Frames which are
 * authenticated must include a time stamp which differs from
 * the receive time stamp by no more than 10 seconds. */
#define INFO_TS_MAXSKEW	10.

#define NTP_VERSION     ((u_char)4)
#define NTP_OLDVERSION  ((u_char)1)
#define MODE_PRIVATE    7

#define	NTP_SHIFT	8	/* clock filter stages */

/* NTPD request codes */
#define REQ_PEER_LIST_SUM	1	/* return summary info for all peers */
#define REQ_PEER_LIST		0	/* return list of peers */
#define REQ_PEER_INFO		2	/* get standard information on peer */
#define REQ_PEER_STATS		3	/* get statistics for peer */
#define REQ_SYS_INFO		4	/* get system information */
#define REQ_SYS_STATS		5	/* get system stats */
#define REQ_IO_STATS		6	/* get I/O stats */
#define REQ_MEM_STATS		7	/* stats related to peer list maint */
#define REQ_LOOP_INFO		8	/* info from the loop filter */
#define REQ_TIMER_STATS		9	/* get timer stats */
#define REQ_CONFIG		10	/* configure a new peer */
#define REQ_UNCONFIG		11	/* unconfigure an existing peer */
#define REQ_SET_SYS_FLAG	12	/* set system flags */
#define REQ_CLR_SYS_FLAG	13	/* clear system flags */
#define REQ_MONITOR		14	/* (not used) */
#define REQ_NOMONITOR		15	/* (not used) */
#define REQ_GET_RESTRICT	16	/* return restrict list */
#define REQ_RESADDFLAGS		17	/* add flags to restrict list */
#define REQ_RESSUBFLAGS		18	/* remove flags from restrict list */
#define REQ_UNRESTRICT		19	/* remove entry from restrict list */
#define REQ_MON_GETLIST		20	/* return data collected by monitor */
#define REQ_RESET_STATS		21	/* reset stat counters */
#define REQ_RESET_PEER		22	/* reset peer stat counters */
#define REQ_REREAD_KEYS		23	/* reread the encryption key file */
#define REQ_DO_DIRTY_HACK	24	/* (not used) */
#define REQ_DONT_DIRTY_HACK	25	/* (not used) */
#define REQ_TRUSTKEY		26	/* add a trusted key */
#define REQ_UNTRUSTKEY		27	/* remove a trusted key */
#define REQ_AUTHINFO		28	/* return authentication info */
#define REQ_TRAPS		29	/* return currently set traps */
#define REQ_ADD_TRAP		30	/* add a trap */
#define REQ_CLR_TRAP		31	/* clear a trap */
#define REQ_REQUEST_KEY		32	/* define a new request keyid */
#define REQ_CONTROL_KEY		33	/* define a new control keyid */
#define REQ_GET_CTLSTATS	34	/* get stats from the control module */
#define REQ_GET_LEAPINFO	35	/* (not used) */
#define REQ_GET_CLOCKINFO	36	/* get clock information */
#define REQ_SET_CLKFUDGE	37	/* set clock fudge factors */
#define REQ_GET_KERNEL		38	/* get kernel pll/pps information */
#define REQ_GET_CLKBUGINFO	39	/* get clock debugging info */
#define REQ_SET_PRECISION	41	/* (not used) */
#define REQ_MON_GETLIST_1	42	/* return collected v1 monitor data */
#define REQ_HOSTNAME_ASSOCID	43	/* Here is a hostname + assoc_id */
#define REQ_IF_STATS		44	/* get interface statistics */
#define REQ_IF_RELOAD		45	/* reload interface list */

/* Determine size of pre-v6 version of structures */
#define v4sizeof(type)		offsetof(type, v6_flag)

/* Flags in the peer information returns */
#define INFO_FLAG_CONFIG	0x1
#define INFO_FLAG_SYSPEER	0x2
#define INFO_FLAG_BURST		0x4
#define INFO_FLAG_REFCLOCK	0x8
#define INFO_FLAG_PREFER	0x10
#define INFO_FLAG_AUTHENABLE	0x20
#define INFO_FLAG_SEL_CANDIDATE	0x40
#define INFO_FLAG_SHORTLIST	0x80
#define INFO_FLAG_IBURST	0x100

/* Flags in the system information returns */
#define INFO_FLAG_BCLIENT	0x1
#define INFO_FLAG_AUTHENTICATE	0x2
#define INFO_FLAG_NTP		0x4
#define INFO_FLAG_KERNEL	0x8
#define INFO_FLAG_MONITOR	0x40
#define INFO_FLAG_FILEGEN	0x80
#define INFO_FLAG_CAL		0x10
#define INFO_FLAG_PPS_SYNC	0x20

/*
 * Peer list structure. Used to return raw lists of peers and also
 * to specify peers in a request e.g. REQ_PEER_STATS
 * @addr: address of peer
 * @port: port number of peer
 * @hmode: mode for this peer
 * @flags: flags (from above)
 * @v6_flag: is this v6 or not
 * @addr6: v6 address of peer
 */
struct ntp_info_peer_list {
	uint32_t addr;
	uint16_t port;
	uint8_t hmode;
	uint8_t flags;
	int32_t v6_flag;
	int32_t unused1;
	struct in6_addr addr6;
};

/* Peer summary structure. Response to REQ_PEER_LIST_SUM
 * @dstadr: local address (zero for undetermined)
 * @srcadr: source address
 * @srcport: source port
 * @stratum: stratum of peer
 * @hpoll: host polling interval
 * @ppoll: peer polling interval
 * @reach: reachability register
 * @flags: flags, from above
 * @hmode: peer mode
 * @delay: peer.estdelay
 * @offset: peer.estoffset
 * @dispersion: peer.estdisp
 * @v6_flag: is this v6 or not
 * @dstadr6: local address (v6)
 * @srcadr6: source address (v6)
 */
struct ntp_info_peer_summary {
	uint32_t dstadr;
	uint32_t srcadr;
	uint16_t srcport;
	uint8_t stratum;
	int8_t hpoll;
	int8_t ppoll;
	uint8_t reach;
	uint8_t flags;
	uint8_t hmode;
	s_fp delay;
	l_fp offset;
	u_fp dispersion;
	int32_t v6_flag;
	int32_t unused1;
	struct in6_addr dstadr6;
	struct in6_addr srcadr6;
};

/*
 * Peer information structure.
 * @dstadr: local address
 * @srcadr: source address
 * @srcport: remote port
 * @flags: peer flags
 * @leap: peer.leap
 * @hmode: peer.hmode
 * @pmode: peer.pmode
 * @stratum: peer.stratum
 * @ppoll: peer.ppoll
 * @hpoll: peer.hpoll
 * @precision: peer.precision
 * @version: peer.version
 * @reach: peer.reach
 * @unreach: peer.unreach
 * @flash: old peer.flash
 * @ttl: peer.ttl
 * @flash2: new peer.flash 
 * @associd: association ID
 * @keyid: peer.keyid
 * @pkeyid: unused
 * @refid: peer.refid
 * @timer: peer.timer
 * @rootdelay: peer.delay
 * @rootdispersion: peer.dispersion
 * @reftime: peer.reftime
 * @org: peer.org
 * @rec: peer.rec
 * @xmt: peer.xmt
 * @filtdelay: delay shift register
 * @filtoffset: offset shift register
 * @order: order of peers from last filter
 * @delay: peer.estdelay
 * @dispersion: peer.estdisp
 * @offset: peer.estoffset
 * @selectdisp: peer select dispersion
 * @estbdelay: broadcast offset
 * @v6_flag: is this v6 or not
 * @dstadr6: local address (v6-like)
 * @srcadr6: sources address (v6-like)
 */
struct ntp_info_peer {
	uint32_t dstadr;
	uint32_t srcadr;
	uint16_t srcport;
	uint8_t flags;
	uint8_t leap;
	uint8_t hmode;
	uint8_t pmode;
	uint8_t stratum;
	uint8_t ppoll;
	uint8_t hpoll;
	int8_t precision;
	uint8_t version;
	uint8_t unused8;
	uint8_t reach;
	uint8_t unreach;
	uint8_t flash;
	uint8_t ttl;
	uint16_t flash2;
	uint16_t associd;
	uint32_t keyid;
	uint32_t pkeyid;
	uint32_t refid;
	uint32_t timer;
	s_fp rootdelay;
	u_fp rootdispersion;
	l_fp reftime;
	l_fp org;
	l_fp rec;
	l_fp xmt;
	s_fp filtdelay[NTP_SHIFT];
	l_fp filtoffset[NTP_SHIFT];
	uint8_t order[NTP_SHIFT];
	s_fp delay;
	u_fp dispersion;
	l_fp offset;
	u_fp selectdisp;
	int32_t unused1;
	int32_t unused2;
	int32_t unused3;
	int32_t unused4;
	int32_t unused5;
	int32_t unused6;
	int32_t unused7;
	s_fp estbdelay;
	u_int v6_flag;
	u_int unused9;
	struct in6_addr dstadr6;
	struct in6_addr srcadr6;
};

/* Peer statistics structure. Response to REQ_PEER_STATS
 * @dstadr: local address
 * @srcadr: remote address
 * @srcport: remote port
 * @flags: peer flags
 * @timereset: time counters were reset
 * @timereceived: time since a packet received
 * @timetosend: time until a packet sent
 * @timereachable: time peer has been reachable
 * @sent: number sent
 * @processed: number processed
 * @badauth: bad authentication
 * @bogusorg: bogus origin
 * @oldpkt: duplicate
 * @seldisp: bad dispersion
 * @selbroken: bad reference time
 * @candidate: select order
 * @v6_flag: is this v6 or not
 * @dstadr6: local address
 * @srcadr6: remote address
 */
struct ntp_info_peer_stats {
	uint32_t dstadr;
	uint32_t srcadr;
	uint16_t srcport;
	uint16_t flags;
	uint32_t timereset;
	uint32_t timereceived;
	uint32_t timetosend;
	uint32_t timereachable;
	uint32_t sent;
	uint32_t unused1;
	uint32_t processed;
	uint32_t unused2;
	uint32_t badauth;
	uint32_t bogusorg;
	uint32_t oldpkt;
	uint32_t unused3;
	uint32_t unused4;
	uint32_t seldisp;
	uint32_t selbroken;
	uint32_t unused5;
	uint8_t candidate;
	uint8_t unused6;
	uint8_t unused7;
	uint8_t unused8;
	int32_t v6_flag;
	int32_t unused9;
	struct in6_addr dstadr6;
	struct in6_addr srcadr6;
};

/* NTP System Info. Mostly the sys.* variables, plus a few unique to
 * the implementation. Response to REQ_SYS_INFO
 * @peer: system peer address (v4)
 * @peer_mode: mode we are syncing to peer in
 * @leap: system leap bits
 * @stratum: our stratum
 * @precision: local clock precision
 * @rootdelay: delay from sync source
 * @rootdispersion: dispersion from sync source
 * @refid: reference ID of sync source
 * @reftime: system reference time
 * @poll: system poll interval
 * @flags: system flags
 * @bdelay: default broarcast offset
 * @frequency: frequency residual (scaled ppm)
 * @authdelay: default authentication delay
 * @stability: clock stability (scalled ppm)
 * @v6_flag: is this v6 or not
 * @peer6: system peer address (v6)
 */
struct ntp_info_sys {
	uint32_t peer;
	uint8_t peer_mode;
	uint8_t leap;
	uint8_t stratum;
	int8_t precision;
	s_fp rootdelay;
	u_fp rootdispersion;
	uint32_t refid;
	l_fp reftime;
	uint32_t poll;
	uint8_t flags;
	uint8_t unused[3];
	s_fp bdelay;
	s_fp frequency;
	l_fp authdelay;
	u_fp stability;
	uint32_t v6_flag;
	uint32_t unused4;
	struct in6_addr peer6;
};

/* Structure for carrying system flags
 * @flags: System configuration flags
 */
struct ntp_sys_flags {
	uint32_t flags;
};

/* NTP client state
 * @sock: Socket for communications with NTP daemon
 * @timeout: Timeout for communication with NTP daemon
 * @key_id: Key ID for authentication with the NTP daemon
 * @key_value: Key for authenticating requests to the NTP daemon
 * @legacy_mode: Index into legacy packet size array for current mode packet
 * size in use.
 * @request_pkt_size: Request packet size in use. Used to try to be compatible
 * with older implementations of NTPD
 * @buffer: Buffer for responses from the NTP daemon
 */
struct sfptpd_ntpclient_state {
	int sock;
	struct sfptpd_timespec timeout;
	int32_t key_id;
	char key_value[SFPTPD_NTP_KEY_MAX];
	unsigned int legacy_mode;
	unsigned int request_pkt_size;
	unsigned char buffer[0x1000];
	struct sfptpd_ntpclient_feature_flags features;
};

/*
 * System flags we can set/clear
 */
#define SYS_FLAG_BCLIENT	0x01
#define SYS_FLAG_PPS		0x02
#define SYS_FLAG_NTP		0x04
#define SYS_FLAG_KERNEL		0x08
#define SYS_FLAG_MONITOR	0x10
#define SYS_FLAG_FILEGEN	0x20
#define SYS_FLAG_AUTH		0x40
#define SYS_FLAG_CAL		0x80


/****************************************************************************
 * Types, Structures & Defines
 ****************************************************************************/

/* NTP UDP port */
#define NTP_PORT 123

/* Host address - always localhost 127.0.0.1 */
#define NTP_ADDRESS 0x7f000001


/****************************************************************************
 * Constants
 ****************************************************************************/

/* Translations for NTPD error values into standard errnos */
static const int mode7_error_to_errno[INFO_ERR_MAX] =
{
	0,           /* INFO_OKAY */
	EMSGSIZE,    /* INFO_ERR_IMPL - incompatible implementation */
	ENOSYS,      /* INFO_ERR_REQ - unknown request code */
	EBADMSG,     /* INFO_ERR_FMT - format error */
	ENODATA,     /* INFO_ERR_NODATA - no data for this request */
	EIO,
	EIO,
	EACCES,      /* INFO_ERR_AUTH - authentication failure */
};

/* Number of legacy modes */
#define NTP_LEGACY_MODE_MAX (2)

/* Array of possible packet sizes for backward compatibility */
static const unsigned int ntp_legacy_pkt_sizes[NTP_LEGACY_MODE_MAX + 1] =
{
	REQ_LEN_NOMAC,
	160,
	48
};


/****************************************************************************
 * Helper Functions
 ****************************************************************************/

static void write_address(struct sockaddr_storage *addr, socklen_t *length,
			  int32_t v6_flag, uint32_t v4_addr,
			  struct in6_addr *v6_addr)
{
	if (v6_flag) {
		struct sockaddr_in6 *sin6 = ((struct sockaddr_in6 *) addr);
		*length = sizeof *sin6;
		memset(sin6, '\0', sizeof *sin6);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_addr = *v6_addr;
	} else {
		struct sockaddr_in *sin = ((struct sockaddr_in *) addr);
		*length = sizeof *sin;
		memset(sin, '\0', sizeof *sin);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = v4_addr;
	}
}


static void read_address(const struct sockaddr_storage *addr,
			 int32_t *v6_flag, uint32_t *v4_addr,
			 struct in6_addr *v6_addr)
{
	if (addr->ss_family == AF_INET6) {
		*v4_addr = 0;
		*v6_addr = ((struct sockaddr_in6 *) addr)->sin6_addr;
		*v6_flag = 1;
	} else {
		*v4_addr = ((struct sockaddr_in *) addr)->sin_addr.s_addr;
		memset(v6_addr, '\0', sizeof *v6_addr);
		*v6_flag = 0;
	}
}


static int cmp_host_address(const struct sockaddr_storage *addr,
			    int32_t v6_flag, uint32_t v4_addr,
			    struct in6_addr *v6_addr)
{
	if (v6_flag) {
		if (addr->ss_family != AF_INET6)
			return -1;
		return memcmp(v6_addr, &((struct sockaddr_in6 *) addr)->sin6_addr, sizeof *v6_addr);
	} else {
		if (addr->ss_family != AF_INET)
			return 1;
		return ((int32_t) ((struct sockaddr_in *) addr)->sin_addr.s_addr) - ((int32_t) v4_addr);
	}
}


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static void mode7_get_systime(l_fp *now)
{
	long double dtemp;
	struct sfptpd_timespec ts;

	/* Convert Unix clock from seconds and nanoseconds to seconds. */
	sfclock_gettime(CLOCK_REALTIME, &ts);
	now->l_i = ts.sec + JAN_1970;
	dtemp = ts.nsec / 1e9;

	/* Renormalize to seconds past 1900 and fraction. */
	if (dtemp >= 1.0) {
		dtemp -= 1.0;
		now->l_i++;
	} else if (dtemp < -1.0) {
		dtemp += 1.0;
		now->l_i--;
	}

	dtemp *= FRAC;
	now->l_uf = (uint32_t)dtemp;
}


static int mode7_send(struct sfptpd_ntpclient_state *ntpclient,
		      void *buf, size_t length)
{
	int rc;
	assert(ntpclient != NULL);

	rc = send(ntpclient->sock, buf, length, 0);
	if (rc <= 0) {
		INFO("ntpclient: mode7: error sending NTP control message, %s\n",
		     strerror(errno));
		return errno;
	}

        return 0;
}


static int mode7_request(struct sfptpd_ntpclient_state *ntpclient,
			 int request_code, bool authenticate,
			 unsigned int num_items, unsigned int item_size,
			 void *data)
{
	l_fp delay_time = { .l_ui = 0, .l_uf = 0x51EB852 };
	l_fp ts;
	l_fp *ptstamp;
	struct ntp_request_pkt pkt;
	int data_size, req_size;
	int mac_len;

	assert(ntpclient != NULL);
	assert((data != NULL) || ((num_items == 0) && (item_size == 0)));

	memset(&pkt, 0, sizeof(pkt));
	pkt.rm_vn_mode = RM_VN_MODE(0, 0, 0);
	pkt.implementation = (u_char)3;
	pkt.request = (u_char)request_code;

	data_size = num_items * item_size;
	if ((data_size > 0) && (data != NULL)) {
		memcpy(pkt.data, data, data_size);
		pkt.err_nitems = ERR_NITEMS(0, num_items);
		pkt.mbz_itemsize = MBZ_ITEMSIZE(item_size);
	} else {
		pkt.err_nitems = ERR_NITEMS(0, 0);
		pkt.mbz_itemsize = MBZ_ITEMSIZE(item_size);  /* allow for optional first item */
	}

	/* If no authentication is required, send the packet now */
	if (!authenticate) {
		pkt.auth_seq = AUTH_SEQ(0, 0);
		return mode7_send(ntpclient, &pkt, ntpclient->request_pkt_size);
	}

	pkt.auth_seq = AUTH_SEQ(1, 0);
	req_size = ntpclient->request_pkt_size;

	/* Write the timestamp to the data padding area immediately before
	 * the hash */
	ptstamp = (void *)((uint8_t *)&pkt + req_size);
	ptstamp--;

	mode7_get_systime(&ts);
	L_ADD(&ts, &delay_time);
	HTONL_FP(&ts, ptstamp);

	mac_len = MD5authencrypt(ntpclient->key_value, (void *)&pkt, req_size,
				 ntpclient->key_id);
	if (!mac_len || (mac_len != (16 + sizeof(keyid_t)))) {
		ERROR("ntpclient: mode7: error while computing NTP MD5 hash\n");
		return EIO;
	}

	return mode7_send(ntpclient, &pkt, req_size + mac_len);
}


static int mode7_check_response_part1(struct sfptpd_ntpclient_state *ntpclient,
				      struct ntp_response_pkt *pkt,
				      unsigned int len,
				      int expected_request_code)
{
	assert(pkt != NULL);

	const uint8_t implementation_code = 3;

	if (len < RESP_HEADER_SIZE) {
		DBG_L3("ntpclient: mode7: received undersize packet, %d\n", len);
		return EAGAIN;
	}

	if ((INFO_VERSION(pkt->rm_vn_mode) > NTP_VERSION) ||
	    (INFO_VERSION(pkt->rm_vn_mode) < NTP_OLDVERSION)) {
		DBG_L3("ntpclient: mode7: received packet with version %d\n",
		       INFO_VERSION(pkt->rm_vn_mode));
		return EAGAIN;
	}

	if (INFO_MODE(pkt->rm_vn_mode) != MODE_PRIVATE) {
		DBG_L3("ntpclient: mode7: received pkt with mode %d\n",
		       INFO_MODE(pkt->rm_vn_mode));
		return EAGAIN;
	}

	if (INFO_IS_AUTH(pkt->auth_seq)) {
		DBG_L3("ntpclient: mode7: encrypted packet received\n");
		return EAGAIN;
	}

	if (!ISRESPONSE(pkt->rm_vn_mode)) {
		DBG_L3("ntpclient: mode7: received request packet, wanted response\n");
		return EAGAIN;
	}

	if (INFO_MBZ(pkt->mbz_itemsize) != 0) {
		DBG_L3("ntpclient: mode7: received packet with non-zero MBZ field\n");
		return EAGAIN;
	}

	if ((pkt->implementation != implementation_code) ||
	    (pkt->request != expected_request_code)) {
		DBG_L3("ntpclient: mode7: received implementation/request of %d/%d, wanted %d/%d",
		       pkt->implementation, pkt->request,
		       implementation_code, expected_request_code);
		return EAGAIN;
	}

	return 0;
}


static int mode7_response(struct sfptpd_ntpclient_state *ntpclient,
			  int request_code,
			  unsigned int *total_items, int required_item_size,
			  void **response)
{
	struct ntp_response_pkt pkt;
	fd_set fds;
	int rc, i, len;
	unsigned int num_items, item_size, pad_size;
	unsigned int seq_num, last_seq_num;
	unsigned int error_code;
	struct sfptpd_timespec end_time, time_now, timeout;
	unsigned int pkts_received;
	bool have_seq[MAXSEQ + 1];
	unsigned char *write_ptr, *read_ptr;

	assert(ntpclient != NULL);
	assert((total_items != NULL) || (required_item_size == 0));
	assert((response != NULL) || (required_item_size == 0));
	
	/* Default return values */
	if (total_items != NULL)
		*total_items = 0;
	if (response != NULL)
		*response = NULL;

	/* Initialise the receive state */
	pkts_received = 0;
	item_size = 0;
	last_seq_num = INT_MAX;
	memset(have_seq, 0, sizeof(have_seq));
	write_ptr = ntpclient->buffer;

	/* Prepare file descriptor set */
	FD_ZERO(&fds);
	FD_SET(ntpclient->sock, &fds);

	(void)sfclock_gettime(CLOCK_MONOTONIC, &end_time);
	sfptpd_time_add(&end_time, &end_time, &ntpclient->timeout);

	/* The algorithm is fairly conplicated because the response may be
	 * split into a series of packets with an increasing sequence number.
	 * As each packet is received, we collect it into a contiguous block.
	 * In addition, we don't know how many packets there will be in the
	 * sequence until we get the packet with the end marker. */
	while (pkts_received <= last_seq_num) {
		struct timespec boring_timeout;

		/* Work out how much time we have left. If we've run out of
		 * time, return now. */
		(void)sfclock_gettime(CLOCK_MONOTONIC, &time_now);
		sfptpd_time_subtract(&timeout, &end_time, &time_now);
		sfptpd_time_to_std_floor(&boring_timeout, &timeout);
		if (timeout.sec < 0)
			return ETIMEDOUT;

		/* Wait on the sockets for the specified period of time */
		rc = pselect(ntpclient->sock+1, &fds, NULL, NULL, &boring_timeout, NULL);
		if (rc < 0) {
			ERROR("ntpclient: mode7: error waiting on socket, %s\n",
			      strerror(errno));
			return errno;
		}

		/* Return immediately if we timed out */
		if (rc == 0)
			return ETIMEDOUT;

		len = recv(ntpclient->sock, &pkt, sizeof(pkt), 0);
		if (len < 0) {
			if (errno != ECONNREFUSED) {
				DBG_L3("ntpclient: mode7: error reading from socket, %s\n",
				       strerror(errno));
			}
			return errno;
		}

		/* Perform various checks for possible problems. If any of the
		 * checks fail, stop processing the packet and wait for the
		 * next one. */
		if (mode7_check_response_part1(ntpclient, &pkt, len, request_code) != 0)
			continue;

		/* Passed the first checks. At this point we know that the
		 * packet is good and is part of the response to our request. */

		/* Check the error code returned in the response. If not
		 * success then return an error */
		error_code = INFO_ERR(pkt.err_nitems);
		if (error_code != INFO_OKAY) {
			if (error_code != INFO_ERR_NODATA)
				DBG_L3("ntpclient: mode7: ntpd error code %d received on non-final packet, %s\n",
				       INFO_ERR(pkt.err_nitems),
				       strerror(mode7_error_to_errno[error_code]));
			if (error_code < INFO_ERR_MAX)
				return mode7_error_to_errno[error_code];
			else
				return EIO;
		}

		/* More checks now that we know the packet is for us... */

		/* Check that the indicated items fit in the packet */
		num_items = INFO_NITEMS(pkt.err_nitems);
		item_size = INFO_ITEMSIZE(pkt.mbz_itemsize);
		if (num_items * item_size > len - RESP_HEADER_SIZE) {
			DBG_L3("ntpclient: mode7: received items %d, size %d too large for pkt %zd\n",
			       num_items, item_size, len - RESP_HEADER_SIZE);
			continue;
		}

		/* If this isn't our first packet, make sure the size isn't
		 * too large */
		if ((pkts_received != 0) && (item_size > required_item_size)) {
			DBG_L3("ntpclient: mode7: received itemsize %d, previous %d\n",
			       item_size, required_item_size);
			continue;
		}

		/* Get the sequence number and discard the packet if we have
		 * already seen it */
		seq_num = INFO_SEQ(pkt.auth_seq);
		if (have_seq[seq_num]) {
			DBG_L3("ntpclient: mode7: received duplicate seq num %d\n", seq_num);
			continue;
		}

		/* Check if this is the last in the sequence */
		if (!ISMORE(pkt.rm_vn_mode) && (last_seq_num <= MAXSEQ)) {
			DBG_L3("ntpclient: mode7: received second end sequence packet\n");
			continue;
		}

		if (!ISMORE(pkt.rm_vn_mode))
			last_seq_num = seq_num;

		/* Work out whether we have to pad the data */
		if (required_item_size > item_size)
			pad_size = required_item_size - item_size;
		else
			pad_size = 0;

		/* Check that ther is enough space in the output buffer for
		 * this chunk of data */
		if (write_ptr + (num_items * (item_size + pad_size)) >
		    ntpclient->buffer + sizeof(ntpclient->buffer)) {
			WARNING("ntpclient: mode7: response larger than buffer %zd\n",
				sizeof(ntpclient->buffer));
			return ENOSPC;
		}

		/* Copy the data into the output buffer */
		read_ptr = pkt.data;
		for (i = 0; i < num_items; i++) {
			memcpy(write_ptr, read_ptr, item_size);
			write_ptr += item_size;
			read_ptr += item_size;
			
			memset(write_ptr, 0, pad_size);
			write_ptr += pad_size;
		}

		/* Update the total number of items received */
		if (total_items != NULL)
			*total_items += num_items;

		pkts_received++;
	}

	/* Return a pointer to the data received. */
	if (response != NULL)
		*response = ntpclient->buffer;
	return 0;
}


static int mode7_query(struct sfptpd_ntpclient_state *ntpclient,
		       int request_code,
		       bool authenticate,
		       unsigned int req_num_items,
		       unsigned int req_item_size,
		       void *req_data,
		       unsigned int *resp_num_items,
		       unsigned int resp_item_size,
		       void **resp_data)
{
	int rc;
	char junk[512];

	assert(ntpclient != NULL);
	assert((req_data != NULL) || ((req_num_items == 0) && (req_item_size == 0)));
	assert((resp_num_items != NULL) || (resp_item_size == 0));
	assert((resp_data != NULL) || (resp_item_size == 0));

legacy_mode_retry:
	/* Before starting the query, make sure the socket is empty */
	while (recv(ntpclient->sock, junk, sizeof(junk), MSG_DONTWAIT) > 0);

	/* Send the request */
	rc = mode7_request(ntpclient, request_code, authenticate,
			   req_num_items, req_item_size, req_data);
	if (rc != 0)
		return rc;

	/* Wait for the response */
	rc = mode7_response(ntpclient, request_code, resp_num_items,
			    resp_item_size, resp_data);

	/* Check whether we failed because we are talking to an old version of
	 * the NTPD daemon. Try again with a smaller message size */
	if ((rc == EMSGSIZE) && (ntpclient->legacy_mode < NTP_LEGACY_MODE_MAX)) {
		ntpclient->legacy_mode++;
		ntpclient->request_pkt_size = ntp_legacy_pkt_sizes[ntpclient->legacy_mode];
		goto legacy_mode_retry;
	}

	return rc;
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_ntpclient_mode7_create(struct sfptpd_ntpclient_state **ntpclient,
				  const struct sfptpd_ntpclient_fns **fns,
				  int32_t key_id, char *key_value)
{
	struct sockaddr_in addr;
	struct sfptpd_ntpclient_state *new;
	int rc;

	new = calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("ntpclient: mode7: failed to allocate memory for client\n");
		return ENOMEM;
	}

	new->key_id = key_id;
	if ((key_id != 0) && (key_value == NULL)) {
		ERROR("ntpclient: mode7: NTP key ID %d specified but key value is null\n", key_id);
		rc = EINVAL;
		goto fail1;
	}

	/* Initialise other members */
	new->legacy_mode = 0;
	new->request_pkt_size = ntp_legacy_pkt_sizes[new->legacy_mode];
	sfptpd_time_from_ns(&new->timeout, SFPTPD_NTP_MODE7_TIMEOUT_NS);

	/* If we have a key, copy it */
	if (key_value != NULL)
		sfptpd_strncpy(new->key_value, key_value, sizeof(new->key_value));

	/* Open a socket for communications with the daemon */
	new->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (new->sock < 0) {
		ERROR("ntpclient: mode7: failed to open a socket, %s\n",
		      strerror(errno));
		rc = errno;
		goto fail1;
	}

	/* Connect the socket */
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(NTP_ADDRESS);
	addr.sin_port = htons(NTP_PORT);

	rc = connect(new->sock, (struct sockaddr *)&addr, sizeof(addr));
	if (rc != 0) {
		ERROR("ntpclient: mode7: failed to connect socket, %s\n",
		      strerror(errno));
		rc = errno;
		goto fail2;
	}

	/* Set feature flags for this protocol instance */
	new->features.detect_presense		= true;
	new->features.get_peers			= true;
	new->features.get_state			= true;
	new->features.get_clock_control		= true;
	new->features.set_clock_control		= false; /* assume no permission */

	/* Success */
	*ntpclient	= new;
	*fns		= &sfptpd_ntpclient_mode7_fns;
	return 0;

fail2:
	close(new->sock);
fail1:
	free(new);
	return rc;
}

static void mode7_destroy(struct sfptpd_ntpclient_state **ntpclient)
{
	if (*ntpclient != NULL)
	{
		/* If we have a valid socket close it */
		if ((*ntpclient)->sock >= 0) {
			close((*ntpclient)->sock);
			(*ntpclient)->sock = -1;
		}

		free(*ntpclient);
		*ntpclient = NULL;
	}
}

static int mode7_get_sys_info(struct sfptpd_ntpclient_state *ntpclient,
			      struct sfptpd_ntpclient_sys_info *sys_info)
{
	int rc;
	unsigned int num_items;
	struct ntp_info_sys *info;
	const uint8_t clock_flags_mask = INFO_FLAG_NTP | INFO_FLAG_KERNEL;
	char host[NI_MAXHOST];
	
	assert(ntpclient != NULL);
	assert(sys_info != NULL);

	rc = mode7_query(ntpclient, REQ_SYS_INFO, false,
			 0, 0, NULL,
			 &num_items, sizeof(*info), (void **)&info);
	if (rc == 0) {
		write_address(&sys_info->peer_address, &sys_info->peer_address_len,
			      info->v6_flag, info->peer, &info->peer6);
		sys_info->clock_control_enabled = ((info->flags & clock_flags_mask) != 0);

		rc = getnameinfo((struct sockaddr *) &sys_info->peer_address,
				 sys_info->peer_address_len,
				 host, sizeof host,
				 NULL, 0, NI_NUMERICHOST);
		if (rc != 0) {
			DBG_L4("ntpclient: mode7: getnameinfo: %s\n", gai_strerror(rc));
		}
		
		DBG_L6("ntp-sys-info: selected-peer-address %s "
		       "leap-flags 0x%hhx, stratum 0x%hhx, flags 0x%hhx, "
		       "clock-control %sabled\n",
		       host,
		       info->leap, info->stratum, info->flags,
		       sys_info->clock_control_enabled? "en": "dis");
	} else if (rc != ECONNREFUSED) {
		DBG_L3("ntpclient: mode7: failed to get system info from NTP daemon, %s\n",
		       strerror(rc));
	}

	return rc;
}


static int mode7_get_peer_info(struct sfptpd_ntpclient_state *ntpclient,
			       struct sfptpd_ntpclient_peer_info *peer_info)
{
	int rc, i;
	unsigned int num_items;
	struct ntp_info_peer_summary *summary;
	struct ntp_info_peer_stats *stats;
	struct ntp_info_peer *info;
	struct ntp_info_peer_list list = {};
	struct sfptpd_ntpclient_peer *peer;
	long double offset;
	int32_t seconds;
	uint32_t fraction;

	assert(ntpclient != NULL);
	assert(peer_info != NULL);

	/* First get the peer list summary */
	rc = mode7_query(ntpclient, REQ_PEER_LIST_SUM, false,
			 0, 0, NULL,
			 &num_items, sizeof(*summary), (void **)&summary);

	/* If NTPd has no peers configured then it will return ENODATA when
	 * queried. This can also happen when NTPd is starting if it is queried
	 * before having completed the DNS lookup of the configured peers. */
	if (rc == ENODATA) {
		DBG_L5("ntpclient: mode7: ntpd did not return any peers\n");
		num_items = 0;
		rc = 0;
	} else if (rc != 0) {
		if (rc != ECONNREFUSED) {
			DBG_L3("ntpclient: mode7: failed to get peer summary from NTP daemon, %s\n",
			       strerror(rc));
		}
		return rc;
	}

	if (num_items > SFPTPD_NTP_PEERS_MAX) {
		num_items = SFPTPD_NTP_PEERS_MAX;
		WARNING("ntpclient: mode7: too many peers - summary limited to %d peers\n",
			num_items);
	}

	peer_info->num_peers = num_items;

	for (i = 0; i < num_items; i++) {
		peer = &peer_info->peers[i];
		seconds = ntohl(summary[i].offset.l_i);
		fraction = ntohl(summary[i].offset.l_uf);
		offset = (long double)seconds + ((long double)fraction / FRAC);
		/* Convert to nanoseconds and invert the offset */
		offset *= -1.0e9;

		write_address(&peer->remote_address, &peer->remote_address_len,
			      summary[i].v6_flag, summary[i].srcadr, &summary[i].srcadr6);
		write_address(&peer->local_address, &peer->local_address_len,
			      summary[i].v6_flag, summary[i].dstadr, &summary[i].dstadr6);
		peer->pkts_sent = 0;
		peer->pkts_received = 0;
		peer->stratum = summary[i].stratum;
		peer->selected = ((summary[i].flags & INFO_FLAG_SYSPEER) != 0);
		peer->shortlist = ((summary[i].flags & INFO_FLAG_SHORTLIST) != 0);
		peer->candidate = (summary[i].hmode == MODE_CLIENT);
		peer->self = ((summary[i].flags & INFO_FLAG_REFCLOCK) != 0);
		peer->offset = offset;
		peer->smoothed_offset = NAN;
		peer->smoothed_root_dispersion = NAN;
	}

	/* For each peer, get the peer stats and info */
	for (i = 0; i < peer_info->num_peers; i++) {
		char remote_host[NI_MAXHOST];
		char local_host[NI_MAXHOST];

		peer = &peer_info->peers[i];

		/* Get the peer stats */
		read_address(&peer->remote_address,
			     &list.v6_flag, &list.addr, &list.addr6);
		list.port = htons(NTP_PORT);
		list.hmode = 0;
		list.flags = 0;

		rc = mode7_query(ntpclient, REQ_PEER_STATS, false,
				 1, sizeof(list), &list,
				 &num_items, sizeof(*stats), (void **)&stats);
		if (rc != 0) {
			if (rc == ENODATA) {
				TRACE_L5("ntpclient: mode7: no data available from peer\n");
				rc = 0;
				continue;
			}
			if (rc != ECONNREFUSED) {
				DBG_L3("ntpclient: mode7: failed to get peer stats from NTP daemon, %s\n",
				       strerror(rc));
			}

			return rc;
		}

		if (num_items > 1) {
			WARNING("ntpclient: mode7: expected 1 set of peer stats, got %d\n",
			num_items);
		}

		if (cmp_host_address(&peer->remote_address,
				     stats->v6_flag, stats->srcadr, &stats->srcadr6) != 0) {
			ERROR("ntpclient: mode7: got peer stats for wrong peer\n");
			return EIO;
		}

		peer->pkts_sent = ntohl(stats->sent);
		peer->pkts_received = ntohl(stats->processed);

		/* Get the peer info */
		read_address(&peer->remote_address,
			     &list.v6_flag, &list.addr, &list.addr6);
		list.port = htons(NTP_PORT);
		list.hmode = 0;
		list.flags = 0;

		rc = mode7_query(ntpclient, REQ_PEER_INFO, false,
				 1, sizeof(list), &list,
				 &num_items, sizeof(*info), (void **)&info);
		if (rc != 0) {
			if (rc != ECONNREFUSED) {
				DBG_L3("ntpclient: mode7: failed to get peer info from NTP daemon, %s\n",
				       strerror(rc));
			}

			return rc;
		}

		if (num_items > 1) {
			WARNING("ntpclient: mode7: expected 1 set of peer stats, got %d\n",
			num_items);
		}

		rc = getnameinfo((struct sockaddr *) &peer->remote_address,
				 peer->remote_address_len,
				 remote_host, sizeof remote_host,
				 NULL, 0, NI_NUMERICHOST);
		
		if (peer->remote_address.ss_family == AF_INET &&
		    cmp_host_address(&peer->remote_address,
				     info->v6_flag, info->srcadr, &info->srcadr6) != 0) {
			/* NB the src address doesn't get populated for v6: possible ntpd bug */
			ERROR("ntpclient: mode7: got peer info for wrong peer (expected %s)\n",
			      remote_host, local_host);
			return EIO;
		}

		peer->root_dispersion
			= (long double)((uint32_t)ntohl(info->rootdispersion))
			* 1.0e9 / 65536.0;
	}

	return rc;
}


static int mode7_clock_control(struct sfptpd_ntpclient_state *ntpclient,
			       bool enable)
{
	int rc;
	int request_code;
	struct ntp_sys_flags sys_flags;

	assert(ntpclient != NULL);

	if (ntpclient->key_id == 0)
		return EACCES;

	sys_flags.flags = SYS_FLAG_NTP | SYS_FLAG_KERNEL;
	/* Get structure into network order */
	sys_flags.flags = htonl(sys_flags.flags);

	request_code = enable? REQ_SET_SYS_FLAG: REQ_CLR_SYS_FLAG;
	
	rc = mode7_query(ntpclient, request_code, true,
			 1, sizeof(sys_flags), &sys_flags,
			 0, 0, NULL);
	if (rc != 0) {
		WARNING("ntpclient: mode7: failed to set NTP daemon system flags, %s\n",
			strerror(rc));
	} else {
		DBG_L1("ntpclient: mode7: %sabled NTP daemon clock control\n",
		       enable? "en": "dis");
	}

	return rc;
}

static int mode7_test_connection(struct sfptpd_ntpclient_state *ntpclient)
{
	/* currently this function is just a wrapper for get_sys_info */
	struct sfptpd_ntpclient_sys_info sys_info = {};
	return mode7_get_sys_info(ntpclient, &sys_info);
}

static struct sfptpd_ntpclient_feature_flags *
mode7_get_features(struct sfptpd_ntpclient_state *ntpclient)
{
	assert(ntpclient != NULL);
	return &(ntpclient->features);
}

/* Define protocol function struct */
const struct sfptpd_ntpclient_fns sfptpd_ntpclient_mode7_fns = {
	.destroy		= mode7_destroy,
	.get_sys_info		= mode7_get_sys_info,
	.get_peer_info		= mode7_get_peer_info,
	.clock_control		= mode7_clock_control,
	.test_connection	= mode7_test_connection,
	.get_features		= mode7_get_features,
};

/* fin */
