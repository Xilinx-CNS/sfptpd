/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

#ifndef _SFPTPD_CRNY_PROTO_H
#define _SFPTPD_CRNY_PROTO_H

#include <stdint.h>

#include "sfptpd_misc.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

/* Chrony command request packet structure */
struct crny_cmd_request {
	uint8_t header[4];
	uint16_t cmd1;
	uint16_t ignore;
	uint32_t randoms;
	uint32_t padding[2];
	uint8_t cmd2[500];
};

/* Chrony command response packet structure */
struct crny_cmd_response {
	uint8_t header[4];
	uint16_t cmd;
	uint16_t reply;
	uint16_t status;
	uint16_t _packing1[3];
	uint32_t seq_id;
	uint32_t _packing2[2];
	uint8_t data[500];
};

/* 20 bytes total. The uint32 means 4-byte alignment is forced on structs that use it. */
struct crny_addr {
	union {
		uint32_t v4_addr;
		uint8_t v6_addr[16];
	} addr_union;
	uint16_t addr_family;
	char padding[2];
};

static_assert(sizeof(struct crny_addr) == 20, "structure matches protocol");

struct crny_tracking {
	uint32_t ref_id;
	struct crny_addr ip_addr;
	uint32_t ignore[4];
	uint32_t tracking_f;
};

struct crny_source {
	struct crny_addr ip_addr; /* we need this to pass to the ntpdata query */
	uint32_t ignore;
	uint16_t state; /* crny_source_data request seems to be the only way to get the state */
	uint16_t mode; /* we use this to filter out reference clocks */
};

struct crny_ntpdata {
	struct crny_addr remote_ip;
	struct crny_addr local_ip;
	char ignore1[4];
	uint8_t mode; /* this has a different meaning to the mode field in crny_source_data */
	uint8_t stratum;
	int8_t poll;
	char ignore2[5];
	uint32_t root_dispersion;
	uint32_t ref_id;
	char ignore3[12];
	uint32_t offset;
	char ignore4[20];
	uint32_t total_sent;
	uint32_t total_received;
	char ignore5[24];
};

struct crny_sourcestats {
	uint32_t ref_id;
	struct crny_addr ignore2;
	uint32_t ignore3[6];
	uint32_t offset_f;
	uint32_t offset_error_f;
};


/****************************************************************************
 * Constants
 ****************************************************************************/

#define CRNY_RUN_PATH "/run/chrony"
#define CRNY_CONTROL_SOCKET_PATH CRNY_RUN_PATH "/" "chronyd.sock"
#define CRNY_CONTROL_CLIENT_FMT CRNY_RUN_PATH "/" "chronyc.%d.sock"


/****************************************************************************
 * Protocol constants
 ****************************************************************************/

/* Request codes */
#define CRNY_REQ_GET_NUM_SOURCES 14
#define CRNY_REQ_SOURCE_DATA_ITEM 15
#define CRNY_REQ_TRACKING_STATE 33
#define CRNY_REQ_SOURCE_STATS 34
#define CRNY_REQ_NTP_DATA 57

/* Response codes */
#define CRNY_RESP_NUM_SOURCES 2
#define CRNY_RESP_SOURCE_DATA_ITEM 3
#define CRNY_RESP_TRACKING_STATE 5
#define CRNY_RESP_SOURCE_STATS 6
#define CRNY_RESP_NTP_DATA 16

/* ntpdata mode codes that are we interested in */
#define CRNY_NTPDATA_MODE_SERVER 4 /* client/server mode */

/* special values for ref_id */
#define REF_ID_LOCAL 0x7f7f0101 /* LOCAL == 127.127.1.1. */
#define REF_ID_LOCL 0x4C4F434C  /* LOCL also means local */
#define REF_ID_UNSYNC 0x0       /* 0x0 means not synchronized */

/* IP addr versions */
#define IP_UNSPEC 0
#define IP_V4 1
#define IP_V6 2

/* chrony state codes */
enum crny_state_code {
	CRNY_STATE_SYSPEER       = 0, /* selected */
	CRNY_STATE_UNREACHABLE   = 1,
	CRNY_STATE_FALSETICKER   = 2,
	CRNY_STATE_JITTERY       = 3,
	CRNY_STATE_CANDIDATE     = 4, /* shortlist */
	CRNY_STATE_OUTLIER       = 5
};

/* chrony source mode codes. These are different from ntpdata mode codes. */
enum crny_src_mode_code {
	CRNY_SRC_MODE_CLIENT     = 0,
	CRNY_SRC_MODE_PEER       = 1,
	CRNY_SRC_MODE_REF        = 2 /* reference clock */
};

#define CMD_REQ_DEFAULT ((struct crny_cmd_request) {{0x06, 0x01, 0x00, 0x00}})


#endif /* _SFPTPD_CRNY_PROTO_H */
