/* SPDX-License-Identifier: BSD-3-Clause AND NTP */
/* Copyright (c) 2017-2024 Advanced Micro Devices, Inc.
 * Copyright (c) 2011-2015 Network Time Foundation
 * Copyright (c) 1992-2015 University of Delaware
 * This file is adapted from ntp-4.2.8p9.
 */

/**
 * @file   sfptpd_ntpd_mode6_client.c
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
#include <netdb.h>
#include <inttypes.h>

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
 * ntp-4.2.8p9 in the following files:
 * 	ntp.h
 * 	ntp_types.h
 * 	ntpq.h
 * 	ntp_control.h
 * 	ntp_safecast.h
 * 	ntpq-subs.c
 * 
 ****************************************************************************/

/*
 * Values for peer mode and packet mode. Only the modes through
 * MODE_BROADCAST and MODE_BCLIENT appear in the transition
 * function. MODE_CONTROL and MODE_PRIVATE can appear in packets,
 * but those never survive to the transition function.
 */
#define	MODE_UNSPEC	0	/* unspecified (old version) */
#define	MODE_ACTIVE	1	/* symmetric active mode */
#define	MODE_PASSIVE	2	/* symmetric passive mode */
#define	MODE_CLIENT	3	/* client mode */
#define	MODE_SERVER	4	/* server mode */
#define	MODE_BROADCAST	5	/* broadcast mode */

/* NTP UDP port */
#define NTP_PORT 123

/* Host address - always localhost 127.0.0.1 */
#define NTP_ADDRESS 0x7f000001

/* NTP versions */
#define NTP_VERSION     ((u_char)4)
#define NTP_OLDVERSION  ((u_char)1)

#define	MAX_MAC_LEN	(6 * sizeof(uint32_t))	/* SHA */

/* NTP types */
typedef uint16_t	associd_t; /* association ID */
typedef int32_t 	keyid_t;

/* Limit on packets in a single response */
#define	MAXFRAGS	32

/* Control packet header macros... */
#define VN_MODE(v, m)		((((v) & 7) << 3) | ((m) & 0x7))
#define	PKT_LI_VN_MODE(l, v, m) ((((l) & 3) << 6) | VN_MODE((v), (m)))

/*
 * Variable list data space
 */
#define MAXLINE		512	/* maximum length of a line */
#define MAXLIST		128	/* maximum variables in list */
#define LENHOSTNAME	256	/* host name limit */

/*
 * These can appear in packets
 */
#define	MODE_CONTROL	6	/* control mode */
#define	MODE_PRIVATE	7	/* private mode */

#define MODE6_RESP_HEADER_SIZE	(offsetof(struct ntp_mode6_packet, offset))
    
#define MODE6_RESP_DATA_SIZE (500) /* variable length */

/*
 * A mode 6 packet is used for exchanging data between an NTP server and a
 * client for purposes other than time syncronization (just like mode 7
 * packets), e.g monitoring, statistics gathering and configuration.
 *
 * A mode 6 packet has the following format:
 *
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |LI | VN  |Mode |R|E|M| Opcode  |         Sequence              |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |             Status            |      Association ID           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |             Offset            |          Count                |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                                                               |
 *    .                                                               .
 *    .                  Payload (variable length)                    .
 *    .                                                               .
 *    |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                          Key Identifier                       |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                                                               |
 *    |                            MAC (128)                          |
 *    |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *	Leap Indicator:		Ignored and usually zero
 *	Version:		NTP protocol major version, currently 4
 *	Mode:			Mode is 6, control mode
 *  	Response bit:		1 in a response, 0 in a request
 * 	Error bit:		1 in an error response, 0 otherwise
 *	More:			1 if payload is continued in next packet,
 *				0 otherwise
 *	Sequence:		Sequence number for multi-packet reassembly
 *	Status:			System status word
 * 	Association ID:		Association ID of peer, or 0 for the ntpd host
 * 	Offset:			Octet offset of this fragment in the response
 *	Count:			Octet count of fragment payload
 */

typedef union ctl_pkt_u_tag {
	uint8_t data[480 + MAX_MAC_LEN]; /* data + auth */
	uint32_t u32[(480 + MAX_MAC_LEN) / sizeof(uint32_t)];
} ctl_pkt_u;

/* ntp control packet structure
 * @li_vn_mode: leap, version, mode
 * @r_e_m_op: response, more, error, opcode
 * @sequence: sequence number of request
 * @status: status word for association
 * @associd: association ID
 * @offset: offset of this batch of data
 * @count: count of data in this packet
 * @u: data (plus optional encryption key and auth code)
 */
struct ntp_mode6_packet {
	uint8_t li_vn_mode;
	uint8_t r_e_m_op;
	uint16_t sequence;
	uint16_t status;
	uint16_t associd;
	uint16_t offset;
	uint16_t count;	
	ctl_pkt_u u;	
};

#define MODE6_ISMORE(li_vn_mode)	(((li_vn_mode)&MORE_BIT)!=0)
#define MODE6_INFO_VERSION(li_vn_mode) ((uint8_t)(((li_vn_mode)>>3)&0x7))
#define MODE6_INFO_MODE(li_vn_mode)	((li_vn_mode)&0x7)

/*
 * Stuff for extracting things from li_vn_mode
 */
#define	PKT_MODE(li_vn_mode)	((u_char)((li_vn_mode) & 0x7))
#define	PKT_VERSION(li_vn_mode)	((u_char)(((li_vn_mode) >> 3) & 0x7))
#define	PKT_LEAP(li_vn_mode)	((u_char)(((li_vn_mode) >> 6) & 0x3))

/*
 * Sequence number used for requests.  It is incremented before
 * it is used.
 */
uint16_t sequence;

static inline int size2int_sat(size_t v)
{
	return (v > INT_MAX) ? INT_MAX : (int)v;
}

/*
 * Requests are automatically retried once, so total timeout with no
 * response is a bit over 2 * DEFTIMEOUT, or 10 seconds.  At the other
 * extreme, a request eliciting 32 packets of responses each for some
 * reason nearly DEFSTIMEOUT seconds after the prior in that series,
 * with a single packet dropped, would take around 32 * DEFSTIMEOUT, or
 * 93 seconds to fail each of two times, or 186 seconds.
 * Some commands involve a series of requests, such as "peers" and
 * "mrulist", so the cumulative timeouts are even longer for those.
 */
#define	DEFDELAY	0x51EB852	/* 20 milliseconds, l_fp fraction */
#define	LENHOSTNAME	256		/* host name is 256 characters long */
#define	MAXCMDS		100		/* maximum commands on cmd line */
#define	MAXHOSTS	200		/* maximum hosts on cmd line */
#define	MAXLINE		512		/* maximum line length */
#define	MAXTOKENS	(1+MAXARGS+2)	/* maximum number of usable tokens */
#define	MAXVARLEN	256		/* maximum length of a variable name */
#define	MAXVALLEN	2048		/* maximum length of a variable value */
#define	MAXOUTLINE	72		/* maximum length of an output line */
#define SCREENWIDTH	76		/* nominal screen width in columns */

/*
 * Macro definitions we use
 */
#define	ISSPACE(c)	((c) == ' ' || (c) == '\t')
#define	min(a,b)	(((a) < (b)) ? (a) : (b))

/* NTP Mode 6 (Control mode) */

/*
 * Length of the control header, in octets
 */
#define	CTL_HEADER_LEN		(offsetof(struct ntp_mode6_packet, u))
#define	CTL_MAX_DATA_LEN	468

/*
 * Decoding for the r_e_m_op field
 */
#define	CTL_RESPONSE	0x80
#define	CTL_ERROR	0x40
#define	CTL_MORE	0x20
#define	CTL_OP_MASK	0x1f

#define	CTL_ISRESPONSE(r_e_m_op) ((CTL_RESPONSE	& (r_e_m_op)) != 0)
#define	CTL_ISMORE(r_e_m_op)	 ((CTL_MORE	& (r_e_m_op)) != 0)
#define	CTL_ISERROR(r_e_m_op)	 ((CTL_ERROR	& (r_e_m_op)) != 0)
#define	CTL_OP(r_e_m_op)	 (CTL_OP_MASK	& (r_e_m_op))

/*
 * Opcodes
 */
#define	CTL_OP_UNSPEC		0	/* unspeciffied */
#define	CTL_OP_READSTAT		1	/* read status */
#define	CTL_OP_READVAR		2	/* read variables */
#define	CTL_OP_WRITEVAR		3	/* write variables */
#define	CTL_OP_READCLOCK	4	/* read clock variables */
#define	CTL_OP_WRITECLOCK	5	/* write clock variables */
#define	CTL_OP_SETTRAP		6	/* set trap address */
#define	CTL_OP_ASYNCMSG		7	/* asynchronous message */
#define CTL_OP_CONFIGURE	8	/* runtime configuration */
#define CTL_OP_SAVECONFIG	9	/* save config to file */
#define CTL_OP_READ_MRU		10	/* retrieve MRU (mrulist) */
#define CTL_OP_READ_ORDLIST_A	11	/* ordered list req. auth. */
#define CTL_OP_REQ_NONCE	12	/* request a client nonce */
#define	CTL_OP_UNSETTRAP	31	/* unset trap */

/*
 * {En,De}coding of the system status word
 */
#define	CTL_SST_TS_UNSPEC	0	/* unspec */
#define	CTL_SST_TS_ATOM		1	/* pps */
#define	CTL_SST_TS_LF		2	/* lf radio */
#define	CTL_SST_TS_HF		3	/* hf radio */
#define	CTL_SST_TS_UHF		4	/* uhf radio */
#define	CTL_SST_TS_LOCAL	5	/* local */
#define	CTL_SST_TS_NTP		6	/* ntp */
#define	CTL_SST_TS_UDPTIME	7	/* other */
#define	CTL_SST_TS_WRSTWTCH	8	/* wristwatch */
#define	CTL_SST_TS_TELEPHONE	9	/* telephone */

#define	CTL_SYS_MAXEVENTS	15

#define	CTL_SYS_STATUS(li, source, nevnt, evnt) \
		(((((unsigned short)(li))<< 14)&0xc000) | \
		(((source)<<8)&0x3f00) | \
		(((nevnt)<<4)&0x00f0) | \
		((evnt)&0x000f))

#define	CTL_SYS_LI(status)	(((status)>>14) & 0x3)
#define	CTL_SYS_SOURCE(status)	(((status)>>8) & 0x3f)
#define	CTL_SYS_NEVNT(status)	(((status)>>4) & 0xf)
#define	CTL_SYS_EVENT(status)	((status) & 0xf)

/*
 * {En,De}coding of the peer status word
 */
#define	CTL_PST_CONFIG		0x80
#define	CTL_PST_AUTHENABLE	0x40
#define	CTL_PST_AUTHENTIC	0x20
#define	CTL_PST_REACH		0x10
#define	CTL_PST_BCAST		0x08

#define	CTL_PST_SEL_REJECT	0	/*   reject */
#define	CTL_PST_SEL_SANE	1	/* x falsetick */
#define	CTL_PST_SEL_CORRECT	2	/* . excess */
#define	CTL_PST_SEL_SELCAND	3	/* - outlier */
#define	CTL_PST_SEL_SYNCCAND	4	/* + candidate */
#define	CTL_PST_SEL_EXCESS	5	/* # backup */
#define	CTL_PST_SEL_SYSPEER	6	/* * sys.peer */
#define	CTL_PST_SEL_PPS		7	/* o pps.peer */

#define	CTL_PEER_MAXEVENTS	15

#define	CTL_PEER_STATUS(status, nevnt, evnt) \
		((((status)<<8) & 0xff00) | \
		(((nevnt)<<4) & 0x00f0) | \
		((evnt) & 0x000f))

#define	CTL_PEER_STATVAL(status)(((status)>>8) & 0xff)
#define	CTL_PEER_NEVNT(status)	(((status)>>4) & 0xf)
#define	CTL_PEER_EVENT(status)	((status) & 0xf)

/*
 * {En,De}coding of the clock status word
 */
#define	CTL_CLK_OKAY		0
#define	CTL_CLK_NOREPLY		1
#define	CTL_CLK_BADFORMAT	2
#define	CTL_CLK_FAULT		3
#define	CTL_CLK_PROPAGATION	4
#define	CTL_CLK_BADDATE		5
#define	CTL_CLK_BADTIME		6

#define	CTL_CLK_STATUS(status, event) \
		((((status)<<8) & 0xff00) | \
		((event) & 0x00ff))

/*
 * Error code responses returned when the E bit is set.
 */
#define	CERR_UNSPEC	0
#define	CERR_PERMISSION	1
#define	CERR_BADFMT	2
#define	CERR_BADOP	3
#define	CERR_BADASSOC	4
#define	CERR_UNKNOWNVAR	5
#define	CERR_BADVALUE	6
#define	CERR_RESTRICT	7
/* there is no specific code for noresource */
#define	CERR_NORESOURCE	CERR_PERMISSION
#define CERR_MAX	8

static const int ntp_cerr2errno[CERR_MAX] =
{
	EIO,			/* CERR_UNSPEC */
	EACCES,			/* CERR_PERMISSION / CERR_NORESOURCE */
	EBADMSG,		/* CERR_BADFMT */
	ENOSYS,			/* CERR_BADOP */
	ENOENT,   		/* CERR_UNKNOWNVAR */
	EINVAL,			/* CERR_BADVALUE */
	EPERM,			/* CERR_RESTRICT */
};

struct varlist {
	const char *name;
	char *value;
} g_varlist[MAXLIST] = { { 0, 0 } };

/* A list of peer variables required, read using ctl_read_var */
struct varlist peervarlist[MAXLIST] = {
	{ "srcadr",	0 },
	{ "dstadr",	0 },
	{ "stratum",	0 },
	{ "offset",	0 },
	{ "hmode",	0 },
	{ "sent",	0 },
	{ "received",	0 },
	{ "rootdisp",	0 },
	{ "refid",	0 },
	{ 0,		0 }
};

/* Structure to hold association data, this is NTP mode 7 specific
 * @assid: Association ID for peer
 * @status: Peer status word */
struct association {
	uint16_t assid;
	uint16_t status;
};
static_assert(sizeof(struct association) == 4,
	       "structure is 4 bytes long to let addressing work correctly");

/* End fo NTP defitions borrowed/modified from NTPD source code */

/* Mode 6 requires two buffers of the same size, the size is defined here.
 * The buffers hold query responses while collecting the peer_info data.
 * The data is copied from one generic buffer to the other association buffer,
 * so the buffers must have the same size. */
#define NTPCLIENT_BUFFER_SIZE	0x1000

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
 * @assoc_cache: Cache for association list received from the NTP daemon
 * @features: Array of feature flags, describes which abilities this protocol
 * does and doesn't have
 */
struct sfptpd_ntpclient_state {
	int sock;
	struct sfptpd_timespec timeout;
	int32_t key_id;
	char key_value[SFPTPD_NTP_KEY_MAX];
	unsigned int legacy_mode;
	unsigned int request_pkt_size;
	unsigned char buffer[NTPCLIENT_BUFFER_SIZE];
	struct association assoc_cache[NTPCLIENT_BUFFER_SIZE];
	struct sfptpd_ntpclient_feature_flags features;
};

/****************************************************************************
 * Local Functions
 ****************************************************************************/

static int mode6_send(struct sfptpd_ntpclient_state *ntpclient,
		      void *buf, size_t length)
{
	int rc;
	assert(ntpclient != NULL);

	rc = send(ntpclient->sock, buf, length, 0);
	if (rc <= 0) {
		INFO("ntpclient: mode6: error sending NTP message, %s\n",
		     strerror(errno));
		return errno;
	}

        return 0;
}

static int mode6_request(struct sfptpd_ntpclient_state *ntpclient,
			 int request_code,
			 associd_t associd, bool authenticate,
			 unsigned int num_items, size_t item_size,
			 void *data)
{
	/* - num_items - will always be zero or one. we either just send
	 * the request_code/opcode, or we send the code and ONE packet
	 * containing something like "disable ntp" */
	/* - authenticate - some commmands need authentication, some dont */
	/* - item size, is number of bytes payload is */
	struct ntp_mode6_packet pkt;
	int mac_len;
	size_t	req_size;

	assert(ntpclient != NULL);
	assert((data != NULL) || ((num_items == 0) && (item_size == 0)));

	/* Check to make sure specified packet size is less than maximum */
	if (item_size > CTL_MAX_DATA_LEN) {
		fprintf(stderr,
			"ntpclient: mode6: error, item_size (%zu) too large\n",
			(size_t)item_size);
		return E2BIG;
	}
	
	/* Fill in the packet */
	memset(&pkt, 0, sizeof(pkt));
	pkt.li_vn_mode = PKT_LI_VN_MODE(0, NTP_VERSION, MODE_CONTROL);
	pkt.r_e_m_op = (uint8_t)(request_code & CTL_OP_MASK);
	pkt.sequence = htons(sequence); /* sequence number to id this query */
	pkt.status = htons(0);	       	/* only set in response */
	pkt.associd = htons(associd); 	/* requests are always to ntpd, id = 0 */
	pkt.offset = htons(0);		/* requests are single messages */
	pkt.count = htons(item_size);	/* payload size */

	req_size = CTL_HEADER_LEN;
	
	/* If we have data, copy and pad it out to a 32-bit boundary. */
	/* Note that requests are always 0 or 1 packets in mode 6.
	 * That may just be because we only ever send a small control
	 * command e.g disable ntp disable kernel */
	if (item_size > 0)
	{
		if (data != NULL)
		{
			memcpy(&pkt.u, data, (size_t)item_size);
			req_size += item_size;
			while (req_size & (sizeof(uint32_t) - 1)) {
				pkt.u.data[item_size++] = 0;
				req_size++;
			}
		}
		else
			ERROR("ntpclient: mode6: data size specified but data pointer is NULL\n");
	}

	/* If no authentication is required, send the packet now */
	if (!authenticate) {
		return mode6_send(ntpclient, &pkt, req_size);
	}

	/* Pad out packet to a multiple of 8 octets to be sure
	 * receiver can handle it */
	while (req_size & 7) {
		pkt.u.data[item_size++] = 0;
		req_size++;
	}

	/* Encryption */
	mac_len = MD5authencrypt(ntpclient->key_value, (void *)&pkt, req_size,
				 ntpclient->key_id);
	if (!mac_len || (mac_len != (16 + sizeof(keyid_t)))) {
		ERROR("ntpclient: mode6: error while computing NTP MD5 hash\n");
		return EIO;
	}

	return mode6_send(ntpclient, &pkt, req_size + mac_len);
}


static int mode6_validate_response_packet(struct ntp_mode6_packet *pkt,
					  unsigned int len,
					  int expected_request_code)
{
	assert(pkt != NULL);

	if (len < CTL_HEADER_LEN) {
		DBG_L3("ntpclient: mode6: received undersize packet, %d\n", len);
		return EAGAIN;
	}

	if ((PKT_VERSION(pkt->li_vn_mode) > NTP_VERSION) ||
	    (PKT_VERSION(pkt->li_vn_mode) < NTP_OLDVERSION)) {
		DBG_L3("ntpclient: mode6: received packet with version %d\n",
		       PKT_VERSION(pkt->li_vn_mode));
		return EAGAIN;
	}

	if (PKT_MODE(pkt->li_vn_mode) != MODE_CONTROL) {
		DBG_L3("ntpclient: mode6: received pkt with mode %d\n",
		       PKT_MODE(pkt->li_vn_mode));
		return EAGAIN;
	}

	if (!CTL_ISRESPONSE(pkt->r_e_m_op)) {
		DBG_L3("ntpclient: mode6: received request packet, wanted response\n");
		return EAGAIN;
	}

	/* Check opcode and sequence number for match incase this is old data
	   and not a response to our request */

	if (ntohs(pkt->sequence) != sequence) {
		DBG_L3("ntpclient: mode6: received sequence number %d, wanted %d\n",
		       pkt-sequence, sequence);
		return EAGAIN;
	}

	if (CTL_OP(pkt->r_e_m_op) != expected_request_code) {
		DBG_L3("ntpclient: mode6: received opcode %d, wanted %d (sequence number"
		       " correct)\n", CTL_OP(pkt->r_e_m_op),
		       expected_request_code);
		return EAGAIN;
	}

	return 0;
}


static int mode6_response(struct sfptpd_ntpclient_state *ntpclient,
			  int request_code,
			  associd_t associd,
			  unsigned int *resp_status,
			  size_t *resp_item_size,
			  void **resp_data)
{
	struct ntp_mode6_packet pkt;
	fd_set fds;
	int rc, len;
	struct sfptpd_timespec end_time, time_now, timeout;
	u_short offsets[MAXFRAGS+1];
	u_short counts[MAXFRAGS+1];
	uint16_t offset;
	uint16_t count;
	int err_code;
	int should_be_size;
	int seen_last_frag;
	size_t num_frags;
	size_t frag_idx;
	unsigned char *read_ptr;
	int i;

	assert(ntpclient != NULL);
	assert(resp_status != NULL);
	assert(resp_item_size != NULL);
	assert(resp_data != NULL);

	/* The algorithm is fairly complicated because the response may be
	 * split into a series of packets with an increasing offset.
	 * As each packet is received, we collect it into a contiguous block
	 * with a specific offset. In addition, we don't know how many packets
	 * there will be in the sequence until we get the packet with the end
	 * marker, and the packets may not arrive in order. */
		
	/* Set response variables initial values */
	*resp_item_size = 0;
	if (resp_status)
		*resp_status = 0;

	/* Set response fragment variables initial values */
	num_frags = 0;
	seen_last_frag = 0;

	/* Set time end to now + timeout duration */
	(void)sfclock_gettime(CLOCK_MONOTONIC, &end_time);
	sfptpd_time_add(&end_time, &end_time, &ntpclient->timeout);

	/* Prepare file descriptor set */
	FD_ZERO(&fds);
	FD_SET(ntpclient->sock, &fds);

	/* Loop until we have an error or a complete response.  Nearly all
	 * code paths to loop again use continue. */
	while (1) {
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
			ERROR("ntpclient: mode6: error waiting on socket, %s\n",
			      strerror(errno));
			return errno;
		}

		/* Return immediately if we timed out */
		if (rc == 0)
			return ETIMEDOUT;

		/* Copy data on socket into pkt struture */
		len = recv(ntpclient->sock, &pkt, sizeof(pkt), 0);
		if (len < 0) {
			if (errno != ECONNREFUSED) {
				DBG_L3("ntpclient: mode6: error reading from socket,"
				       " %s\n", strerror(errno));
			}
			return errno;
		}

		/* Perform various checks for possible problems. If any of the
		 * checks fail, stop processing the packet and wait for the
		 * next one. */
		if (mode6_validate_response_packet(&pkt, len, request_code) != 0)
			continue;

		/* Passed the first checks. At this point we know that the
		 * packet structure is good and is part of the response to our
		 * request. */

		/* Check the error code returned in the response. If not
		 * success then return an error */
		if (CTL_ISERROR(pkt.r_e_m_op)) {
			err_code = (ntohs(pkt.status) >> 8) & 0xff;
			if (CTL_ISMORE(pkt.r_e_m_op))
				DBG_L3("ntpclient: mode6: error code %d received on"
				       " non-final packet\n", pkt.r_e_m_op);
			return ntp_cerr2errno[err_code];	
		}

		/* Check the association ID to make sure it matches what we
		 * sent */
		if (ntohs(pkt.associd) != associd) {
			DBG_L3("ntpclient: mode6: Association ID %d doesn't match "
			       "expected %d\n", ntohs(pkt.associd), associd);
		}

		/* Collect offset and count. Make sure they make sense. */
		offset = ntohs(pkt.offset);
		count = ntohs(pkt.count);

		/* Validate received payload size is padded to next 32-bit
		 * boundary and no smaller than claimed by pkt.count */
		if (len & 0x3) {
			DBG_L3("ntpclient: mode6: Response packet not padded, "
			       "size = %d\n", len);
			continue;
		}

		should_be_size = (CTL_HEADER_LEN + count + 3) & ~3;

		if (len < should_be_size) {
			printf("ntpclient: mode6: Response packet claims %u octets payload,"
			       "above %ld received\n", count,
			       (long)len - CTL_HEADER_LEN);
			return EPROTO;
		}

		/* Packet fragment checks */
		/* fragment larger than stated in packet header */
		if (count > (len - CTL_HEADER_LEN)) {
			DBG_L3("ntpclient: mode6: Received count of %u octets, data in packet is"
			       " %ld\n", count, (long)len - CTL_HEADER_LEN);
			continue;
		}
		/* count is zero but there are more packets to come */
		if (count == 0 && CTL_ISMORE(pkt.r_e_m_op)) {
			DBG_L3("ntpclient: mode6: Received count of 0 in non-final fragment\n");
			continue;
		}
		/* check packet fragment fits in buffer */
		if (offset + count > sizeof(ntpclient->buffer)) {
			DBG_L3("ntpclient: mode6: Offset %u, count %u, too big for buffer\n",
			        offset, count);
			return ENOSPC;
		}
		/* check if we've received duplicate 'last' fragments */
		if (seen_last_frag && !CTL_ISMORE(pkt.r_e_m_op)) {
			DBG_L3("ntpclient: mode6: Received second last fragment packet\n");
			continue;
		}

		/* Checks so far indicate packet is good */
		
		/* Record fragment, making sure it doesn't overlap anything */
		if (num_frags > (MAXFRAGS - 1)) {
			DBG_L3("ntpclient: mode6: Number of fragments exceeds maximum"
			       " %d\n", (MAXFRAGS - 1));
			return EFBIG;
		}
		
		/* Find position of this fragment relative to others received
		 * by comparing packet offsets */
		/* Find position of this frament in fragment array */
		for (frag_idx = 0;
		     (frag_idx < num_frags) && (offsets[frag_idx] < offset);
		     frag_idx++) {
			/* empty body */ ;
		}
		/* Fragment validation checks... */
		if (frag_idx < num_frags && offset == offsets[frag_idx]) {
			DBG_L3("ntpclient: mode6: duplicate %u octets at %u ignored, prior %u"
			        " at %u\n", count, offset, counts[frag_idx],
			        offsets[frag_idx]);
			continue;
		}
		if (frag_idx > 0 && (offsets[frag_idx-1] + counts[frag_idx-1]) > offset) {
			DBG_L3("ntpclient: mode6: received frag at %u overlaps with %u octet"
			        " frag at %u\n", offset, counts[frag_idx-1],
			        offsets[frag_idx-1]);
			continue;
		}
		if (frag_idx < num_frags && (offset + count) > offsets[frag_idx]) {
			DBG_L3("ntpclient: mode6: received %u octet frag at %u overlaps with"
			        " frag at %u\n", count, offset, offsets[frag_idx]);
			continue;
		}
		/* Move all later fragments +1 index to make room for new fragment */
		for (i = num_frags; i > frag_idx; i--) {
			offsets[i] = offsets[i-1];
			counts[i] = counts[i-1];
		}
		/* Insert the values of this frament into the fragment array */
		offsets[frag_idx] = offset;
		counts[frag_idx] = count;
		num_frags++;

		/* For last fragment */
		if (!CTL_ISMORE(pkt.r_e_m_op)) {
			seen_last_frag = 1;
		}

		/* Check that there is enough space in the output buffer for
		 * this chunk of data */
		if (offset + count > sizeof(ntpclient->buffer)) {
			WARNING("ntpclient: mode6: response larger than buffer %zd\n",
				sizeof(ntpclient->buffer));
			return ENOSPC;
		}
    
		/* Copy data into data buffer using packet fragment octet count
		 * and packet fragment offset of total message */
		read_ptr = pkt.u.data;
		memcpy(ntpclient->buffer + offset, read_ptr, count);
    
		/* Reset timer for next packet */
		(void)sfclock_gettime(CLOCK_MONOTONIC, &end_time);
		sfptpd_time_add(&end_time, &end_time, &ntpclient->timeout);

		/* If last fragment was seen, look for missing fragments in
		 * sequence. If none exist, end. */
		if (seen_last_frag && offsets[0] == 0) {
			for (frag_idx = 1; frag_idx < num_frags; frag_idx++)
				if (offsets[frag_idx-1] + counts[frag_idx-1] !=
				    offsets[frag_idx])
					break;
			if (frag_idx == num_frags) {
				*resp_item_size = offsets[frag_idx-1] + counts[frag_idx-1];
				*resp_data = (void *)&(ntpclient->buffer);
				return 0;
			}
		}
	}
}

static int mode6_query(struct sfptpd_ntpclient_state *ntpclient,
		       int request_code,
		       associd_t associd,
		       bool authenticate,
		       unsigned int req_num_items,
		       size_t req_item_size,
		       void *req_data,
		       unsigned int *resp_status,
		       size_t *resp_size,
		       void **resp_data)
{
	int rc;
	char junk[512];

	assert(ntpclient != NULL);
	assert((req_data != NULL) || ((req_num_items == 0) && (req_item_size == 0)));

	/* Before starting the query, make sure the socket is empty */
	while (recv(ntpclient->sock, junk, sizeof(junk), MSG_DONTWAIT) > 0);

	sequence++;

	/* Send the request */
	rc = mode6_request(ntpclient, request_code, associd, authenticate,
			   req_num_items, req_item_size, req_data);
	if (rc != 0)
		return rc;

	/* Wait for the response */
	rc = mode6_response(ntpclient, request_code, associd, resp_status,
			    resp_size, resp_data);

	/* Note: Unlike mode 7, this function doesn't accommodate old ntpd
	 * versions which use legacy packet length limits. This shouldn't be an
	 * issue assuming mode 6 is only used with newer versions of ntpd where
	 * mode 7 is disabled by default */
	
	return rc;
	
}

/****************************************************************************
 * NTP Helper Functions
 * 
 * The reference for the 'make_query_data' and 'next_var' is in NTPd source
 * code ntp-4.2.8p9 in the following files:
 * 	ntpq.c
 * 
 ****************************************************************************/

/*
 * make_query_data - form a data buffer to be included with a query
 * data_len is input and output:
 * 	input	- maximum size of request buffer
 * 	output	- actual size of request buffer
 */
static void
make_query_data(struct varlist *var_list,
		size_t *data_len,
		char *data)
{
	struct varlist *vl;
	char *cp, *cp_end;
	size_t name_len, value_len;
	size_t total_len;

	cp = data;
	cp_end = data + *data_len;

	for (vl = var_list; vl < var_list + MAXLIST && vl->name != 0; vl++) {
		/* Caculate string length of this name/value pair */
		name_len = strlen(vl->name);
		if (vl->value == 0)
			value_len = 0;
		else
			value_len = strlen(vl->value);
		total_len = name_len + value_len + (value_len != 0) + (cp != data);
		/* Check if we've exceeded maximum request string length */
		if (cp + total_len > cp_end) {
		    DBG_L4("ntpclient: mode6: make_query_data: Ignoring variables "
		           "starting with '%s'\n",
		           vl->name);
		    break;
		}

		if (cp != data)
			*cp++ = ',';
		memcpy(cp, vl->name, (size_t)name_len);
		cp += name_len;
		if (value_len != 0) {
			*cp++ = '=';
			memcpy(cp, vl->value, (size_t)value_len);
			cp += value_len;
		}
	}
	*data_len = (size_t)(cp - data);
}

/*
 * next_var - find the next variable in the buffer
 * This function takes an input name=value list, outputting {name,value} pairs,
 * and removing them from the input buffer until the input buffer is empty.
 * Return value:
 * 	1 means at least one character was found in the string
 * 	0 means the buffer is now empty, or that the name/value was too large
 * Whitespace and commas are skipped at the beginning of strings.
 * Whitespace is skipped at the end of strings.
 * ",=\r\n" are used as string delimiters.
 * '\r' and '\n' characters will be individually removed from the input buffer,
 * set name='\0' valueptr=NULL, and return 1.
 */
static int
next_var(size_t *data_len,	
	 const char **data_ptr,
	 char **v_name,
	 char **v_value)
{
	const char *cp;
	const char *np;
	const char *cp_end;
	const char *val_end;
	size_t src_len;
	size_t len;
	static char name[MAXVARLEN];
	static char value[MAXVALLEN];

	cp = *data_ptr;
	cp_end = cp + *data_len;

	/*
	 * Space past commas and white space
	 */
	while (cp < cp_end && (*cp == ',' || ISSPACE(*cp)))
		cp++;
	/* If the input buffer is now empty, return 0 to signal we are done */
	if (cp >= cp_end)
		return 0;

	/*
	 * Copy name until we hit a ',', an '=', a '\r' or a '\n'.  Backspace
	 * over any white space and terminate it.
	 */
	src_len = strcspn(cp, ",=\r\n");
	src_len = min(src_len, (size_t)(cp_end - cp));
	len = src_len;
	while (len > 0 && ISSPACE((unsigned char)cp[len - 1]))
		len--;
	if (len >= sizeof(name))
	    return 0;
	if (len > 0)
		memcpy(name, cp, len);
	name[len] = '\0';
	*v_name = name;
	cp += src_len;

	/*
	 * Check if we hit the end of the buffer or a ','.  If so, update
	 * the input buffer past this character and we are done.
	 */
	if (cp >= cp_end || *cp == ',' || *cp == '\r' || *cp == '\n') {
		if (cp < cp_end)
			cp++;
		*data_ptr = cp;
		*data_len = size2int_sat(cp_end - cp);
		*v_value = NULL;
		return 1;
	}

	/*
	 * So far, so good.  Copy out the value
	 */
	cp++;	/* past '=' */
	while (cp < cp_end && (ISSPACE((unsigned char)*cp) && *cp != '\r' && *cp != '\n'))
		cp++;
	np = cp;
	if ('"' == *np) {
		cp++;
		do {
			np++;
		} while (np < cp_end && '"' != *np);
		val_end = np;
		if (np < cp_end && '"' == *np)
			np++;
	} else {
		while (np < cp_end && ',' != *np && '\r' != *np)
			np++;
		val_end = np;
	}
	len = val_end - cp;
	if (np > cp_end || len >= sizeof(value) ||
	    (np < cp_end && ',' != *np && '\r' != *np))
		return 0;
	memcpy(value, cp, len);
	/*
	 * Trim off any trailing whitespace
	 */
	while (len > 0 && ISSPACE((unsigned char)value[len - 1]))
		len--;
	value[len] = '\0';

	/*
	 * Return this.  All done.
	 */
	if (np < cp_end && ',' == *np)
		np++;
	*data_ptr = np;
	*data_len = size2int_sat(cp_end - np);
	*v_value = value;
	return 1;
}


/****************************************************************************
 * Helper Functions
 ****************************************************************************/

/* Convert from ip4/6 numeric string to sockaddr */
static int parse_addr_string(struct sockaddr_storage *sockaddr,
			     socklen_t *length, char *address,
			     uint32_t address_len)
{
	int gai_rc;
	struct addrinfo *result;
	const struct addrinfo hints = {	.ai_flags = AI_NUMERICHOST };
	int i = 0;

	sockaddr->ss_family = AF_UNSPEC;
	*length = 0;

	/* NTPD can append port number (and brackets for ipv6) which must be
	 * stripped. Possible address strings are:
	 * ipv4		x.x.x.x
	 * ipv4+port	x.x.x.x:port
	 * ipv6		xxxx:xxxx:xxxx:xxxx
	 * ipv6+port	[xxxx:xxxx:xxxx:xxxx]:port */
	if (address[0] == '[') {
		/* assume [ipv6]:port */
		address++;
		address_len--;
		while (address[i] != ']') {
			if (i >= address_len) {
				DBG_L5("ntpclient: mode6: parse_addr_string: "
				       "address starting with '[' terminated "
				       "without matching ']'\n");
				return EINVAL;
			}
			i++;
		}
		address[i] = '\0';
	} else {
		uint32_t colon_idx = -1;
		for (i = 0; i < address_len; i++) {
			if (address[i] == ':') {
				/* this is either ipv4:port or ipv6 */
				if (colon_idx != -1) /* >=two colons, assume ipv6 */
					break;
				else
					colon_idx = i;
			} else if (address[i] == '\0') {
				if (colon_idx != -1) {
					/* one colon, assume ipv4:port, remove port */
					address[colon_idx] = '\0';
					break;
				} else {
					/* no colon, assume ipv4 */
					break;
				}
			}
		}
	}

	gai_rc = getaddrinfo(address, NULL, &hints, &result);
	if (gai_rc != 0) {
		ERROR("ntpclient: mode6: failed to interpret NTP peer address "
		      "%s, %s\n", address, gai_strerror(gai_rc));
		return (gai_rc == EAI_NODATA || gai_rc == EAI_NONAME) ? ENOENT : EINVAL;
	} else {
		if (result != NULL) {
			assert(result->ai_addrlen <= sizeof *sockaddr);
			*length = result->ai_addrlen;
			memcpy(sockaddr, result->ai_addr, *length);
		}
		freeaddrinfo(result);
		return 0;
	}
}

/* Convert from integer format string to integer */
static uint32_t parse_u32_string(char **int_string)
{
	uint32_t converted_int;;
	sscanf(*int_string, "%"SCNu32, &converted_int); 
	return converted_int;
}

/* Convert from float format string to long double */
static long double parse_float_string(char **double_string)
{
	long double converted_double;;
	sscanf(*double_string, "%Lf", &converted_double); 
	return converted_double;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_ntpclient_mode6_create(struct sfptpd_ntpclient_state **ntpclient,
				  const struct sfptpd_ntpclient_fns **fns,
				  int32_t key_id, char *key_value)
{
	struct sockaddr_in addr;
	struct sfptpd_ntpclient_state *new;
	int rc;

	new = calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("ntpclient: mode6: failed to allocate memory for client\n");
		return ENOMEM;
	}

	new->key_id = key_id;
	if ((key_id != 0) && (key_value == NULL)) {
		ERROR("ntpclient: mode6: NTP key ID %d specified but key value is null\n", key_id);
		rc = EINVAL;
		goto fail1;
	}

	/* Initialise other members */
	new->legacy_mode = 0;
	new->request_pkt_size = sizeof(struct ntp_mode6_packet);
	sfptpd_time_from_s(&new->timeout, 1);

	/* If we have a key, copy it */
	if (key_value != NULL)
		sfptpd_strncpy(new->key_value, key_value, sizeof(new->key_value));

	/* Open a socket for communications with the daemon */
	new->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (new->sock < 0) {
		ERROR("ntpclient: mode6: failed to open a socket, %s\n",
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
		ERROR("ntpclient: mode6: failed to connect socket, %s\n",
		      strerror(errno));
		rc = errno;
		goto fail2;
	}

	/* Set feature flags for this protocol instance */
	new->features.detect_presense		= true;
	new->features.get_peers			= true;
	new->features.get_state			= true;
	new->features.get_clock_control		= false;
	new->features.set_clock_control		= false; /* assume no permission */

	/* Success */
	*ntpclient	= new;
	*fns		= &sfptpd_ntpclient_mode6_fns;
	return 0;

fail2:
	close(new->sock);
fail1:
	free(new);
	return rc;
}

static void mode6_destroy(struct sfptpd_ntpclient_state **ntpclient)
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

static int mode6_get_sys_info(struct sfptpd_ntpclient_state *ntpclient,
			      struct sfptpd_ntpclient_sys_info *sys_info)
{
	int rc;
	int gai_rc;
	void *req_data;
	size_t req_datalen;
	const char *resp_data;
	size_t resp_size;
	unsigned int resp_status;
	char *name;
	char *value;
	char host[NI_MAXHOST];

	assert(ntpclient != NULL);
	assert(sys_info != NULL);

	/* Note: sys_info->clock_control_enabled is not modified as we have no
	 * way of collecting this information from the daemon in mode 6. Instead,
	 * we assume the state will not changed unless we change it. The NTP
	 * sync module is responsible for changing this value when a clock
	 * control command is sent successfully to the daemon */
	
	/* Create request */
	/* The 'peeradr' variable was added to ntpd after 4.2.6p5 (mode 7
	 * enabled by default) and exists in 4.2.8 (mode 7 enabled by default).
	 * It is assumed that when using mode 6, the 'peeradr' variable will
	 * exist. */
        /* Note: It is possible to get the selected peer address by iterating
	 * through the assoc cache, which would require many more packets to
	 * be sent and processed, but would be safer if the above assumption
	 * turned out to be false */
	req_data = "peeradr";	
	req_datalen = strlen(req_data);
	
	/* Send request to collect system address */
	rc = mode6_query(ntpclient, CTL_OP_READVAR, 0, false,
			 1, req_datalen, req_data,
			 &resp_status, &resp_size, (void **)&resp_data);

	/* Populate sys_info object */
	if (rc == 0) {

		/* get peer address */
		rc = ENOENT;
		while (next_var(&resp_size, &resp_data, &name, &value)) {
			if (strcmp("peeradr", name) == 0) {
				rc = parse_addr_string(&sys_info->peer_address,
						       &sys_info->peer_address_len,
						       value, MAXVALLEN);
				goto finish;
			}
		}

		/* Turn the address back into a string for presentation. We could
		 * use the string from the protocol but this will be a canonically-
		 * formatted representation. */
		gai_rc = getnameinfo((struct sockaddr *) &sys_info->peer_address,
				    sys_info->peer_address_len,
				    host, sizeof host,
				    NULL, 0, NI_NUMERICHOST);
		if (gai_rc != 0) {
			DBG_L4("ntpclient: mode6: getnameinfo: %s\n", gai_strerror(gai_rc));
			rc = ENOENT;
		} else {
			DBG_L6("ntp-sys-info: selected-peer-address %s\n", host);
		}
	}

finish:
	/* Overall error handling */
	if (rc == ENOENT) {
		/* In cases where mode 7 is not available and the 'peeradr' variable
		 * does not exist, we output a WARNING. */
		WARNING("ntpclient: mode6: mode 6 is being used but there is no support for "
			"the peeradr variable. %s\n",
			 strerror(rc));
	} else if (rc != 0 && rc != ECONNREFUSED) {
		/* this may be because peeradr is not implemented in this
		 * instance of ntpd, as I found with rhel 7.1 */
		DBG_L3("ntpclient: mode6: failed to get system info from NTP daemon, %s\n",
			strerror(abs(rc)));
		rc = ENOENT;
	}

	return rc;
}

static int mode6_get_peer_info(struct sfptpd_ntpclient_state *ntpclient,
			       struct sfptpd_ntpclient_peer_info *peer_info)
{	
	int rc, i;
	char req_data[CTL_MAX_DATA_LEN];
	size_t req_datalen = CTL_MAX_DATA_LEN;
	unsigned int resp_status;
	size_t resp_size;
	struct association *resp_stat_data;
	const char *resp_var_data;
	struct association *assoc_ptr;
	struct sfptpd_ntpclient_peer *peer;
	char *name;
	char *value;
	u_char statval;

	assert(ntpclient != NULL);
	assert(peer_info != NULL);

	/* First, get a list of associations */
	
	rc = mode6_query(ntpclient, CTL_OP_READSTAT, 0, 0,
			 0, 0, NULL,
			 &resp_status, &resp_size, (void **)&resp_stat_data);
	if (rc != 0)
		return 0;

	if (resp_size == 0) {
		DBG_L5("ntpclient: mode6: ntpd did not return any peers\n");
		return 0;
	}

	if (resp_size & 0x3) {
		ERROR("ntpclient: mode6: Server returned %zu octets, should be multiple of 4\n",
		      resp_size);
		return 0;
	}

	if (resp_size > NTPCLIENT_BUFFER_SIZE) {
		ERROR("ntpclient: mode6: Server returned %zu octets, which does not fit in"
		      "maximum buffer size", resp_size);
		return ENOMEM;
	}

	/* copy associations from general ntpclient buffer to ntpclient assoc cache */
	memcpy(ntpclient->assoc_cache, ntpclient->buffer, resp_size);

	/* we don't know yet whether any of the peers are accessable */
	peer_info->num_peers = 0;
	
	/* Run through the associations and fill peer stat info for each */
	int num_assocations = resp_size/sizeof(struct association);
	for (i = 0; i < num_assocations; i++)
	{
		/* Set assoc ptr to point to next unread assocation in buffer */
		assoc_ptr = ntpclient->assoc_cache + i;

		/* Skip this association if status flags show that host is not
		   reachable or is not a peristent association */
		if (!(CTL_PEER_STATVAL(ntohs(assoc_ptr->status)) &
		      (CTL_PST_CONFIG | CTL_PST_REACH)))
			continue;

		/* Increment peers count */
		peer_info->num_peers++;

		/* Create list of variables to ask for in the request (peervarlist -> data) */
		make_query_data(peervarlist, &req_datalen, req_data);
		
		/* Send request to collect variables */
		rc = mode6_query(ntpclient, CTL_OP_READVAR, ntohs(assoc_ptr->assid), false,
				 1, req_datalen, (void *)req_data,
				 &resp_status, &resp_size, (void **)&resp_var_data);

		/* Set up our peer object to fill with information */
		peer = &peer_info->peers[i];

		/* Populate peer object */
		peer->smoothed_offset = NAN;
		peer->smoothed_root_dispersion = NAN;

		/* Parse text-based packet payload of requested peer variables */
		while (next_var(&resp_size, &resp_var_data, &name, &value))
		{
			if (strcmp("srcadr", name) == 0)
				parse_addr_string(&peer->remote_address,
						  &peer->remote_address_len,
						  value, MAXVALLEN);
			else if (strcmp("dstadr", name) == 0)
				parse_addr_string(&peer->local_address,
						  &peer->local_address_len,
						  value, MAXVALLEN);
			else if (strcmp("stratum", name) == 0)
				peer->stratum = parse_u32_string(&value);
			else if (strcmp("hmode", name) == 0)
				peer->candidate = (parse_u32_string(&value) == MODE_CLIENT);
			else if (strcmp("offset", name) == 0)
			{
				long double offset = parse_float_string(&value);
				/* Convert from RMS milliseconds to nanoseconds
				 * and invert the offset */
				offset *= -1.0e6;
				peer->offset = offset;
			}
			else if (strcmp("rootdisp", name) == 0) {
				/* Convert from milliseconds to nanoseconds */
				peer->root_dispersion = parse_float_string(&value) * 1.0e6;
			} else if (strcmp("sent", name) == 0)
				peer->pkts_sent = parse_u32_string(&value);
			else if (strcmp("received", name) == 0)
				peer->pkts_received = parse_u32_string(&value);
			else if (strcmp("refid", name) == 0)
			{
				/* NTPDC sets refid depending on if the 'peer' is
				 * a peer or a reference clock.
				 *   Reference clock: refid = <=4 char identifier
				 *   Peer: refid = IP address of reference clock
				 * See NTPD source code:
				 *   ntp_control.c:ctl_putpeer
				 */
				if (strlen(value) <= 4)
					peer->self = true;
				peer->self = false;
			}
		}
		/* Parse peer status word */
		statval = CTL_PEER_STATVAL(ntohs(assoc_ptr->status));
		/* * System peer */
		peer->selected = ((statval & 0x7) == CTL_PST_SEL_SYSPEER); 
		/* # Backup */
		peer->shortlist = ((statval & 0x7) == CTL_PST_SEL_EXCESS); 

	}
	
	return rc;
}

static int mode6_clock_control(struct sfptpd_ntpclient_state *ntpclient,
				   bool enable)
{
	int rc;
	const char *cfgcmd;
	const char *resp_data;
	size_t resp_size;
	unsigned int resp_status;

	assert(ntpclient != NULL);

	if (ntpclient->key_id == 0)
		return EACCES;

	/* The request payload should be a line of text in the syntax of the
	 * ntp.conf configuration file. The response payload will begin with
	 * either an error message or the string "Config Succeeded", followed
	 * by a NUL. */

	if (enable)
		cfgcmd = "enable ntp kernel";
	else
		cfgcmd = "disable ntp kernel";
			
	rc = mode6_query(ntpclient, CTL_OP_CONFIGURE, 0, true, /* auth required */
			 1, strlen(cfgcmd), (void *)cfgcmd,
			 &resp_status, &resp_size, (void **)&resp_data);
	if (rc != 0) {
		WARNING("ntpclient: mode6: failed to set NTP daemon system flags, %s\n",
			strerror(rc));
	} else {
		/* Check response */
		char *response_success = "Config Succeeded\r\n";
		if (strncmp(resp_data, response_success,
			    strlen(response_success)) != 0) {
			/* due to ntpd implementation error the response will
			 * contain garbage after the string. The string ends
			 * with "\r\n", so resp_size-2 will print the message
			 * with no carriage return, newline, or garbage */
			WARNING("ntpclient: mode6: Config failed with error message from "
				"ntpclient: \"%.*s\"\n", resp_size-2, resp_data);
			/* Assume that failure was due to invalid key */
			return EACCES; 
		}
		else {
			/* Successfully set clock control */
			DBG_L1("ntpclient: mode6: %sabled NTP daemon clock control\n",
			       enable? "en": "dis");
			ntpclient->features.set_clock_control = true;
		}	
	}

	return rc;
}

static int mode6_test_connection(struct sfptpd_ntpclient_state *ntpclient)
{
	/* currently this function is just a wrapper for get_sys_info */
	struct sfptpd_ntpclient_sys_info sys_info = {};
	return mode6_get_sys_info(ntpclient, &sys_info);
}

static struct sfptpd_ntpclient_feature_flags *
mode6_get_features(struct sfptpd_ntpclient_state *ntpclient)
{
	assert(ntpclient != NULL);
	return &(ntpclient->features);
}

/* Define protocol function struct */
const struct sfptpd_ntpclient_fns sfptpd_ntpclient_mode6_fns = {
	.destroy		= mode6_destroy,
	.get_sys_info		= mode6_get_sys_info,
	.get_peer_info		= mode6_get_peer_info,
	.clock_control		= mode6_clock_control,
	.test_connection	= mode6_test_connection,
	.get_features		= mode6_get_features,
};

/* fin */
