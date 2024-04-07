/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024 Advanced Micro Devices, Inc. */

#ifndef _SFPTPD_PRIV_OPS_H
#define _SFPTPD_PRIV_OPS_H


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

typedef char sfptpd_short_text_t[16];

enum sfptpd_priv_req {
	SFPTPD_PRIV_REQ_CLOSE,
	SFPTPD_PRIV_REQ_SYNC,
	SFPTPD_PRIV_REQ_OPEN_CHRONY,
	SFPTPD_PRIV_REQ_OPEN_DEV,
};

enum sfptpd_priv_resp {
	SFPTPD_PRIV_RESP_OK,
	SFPTPD_PRIV_RESP_OPEN_CHRONY,
	SFPTPD_PRIV_RESP_OPEN_DEV,
};

struct sfptpd_priv_req_open_dev {
	char path[128];
};

struct sfptpd_priv_req_msg {
	enum sfptpd_priv_req req;
	union {
		struct sfptpd_priv_req_open_dev open_dev;
	};
};

struct sfptpd_priv_resp_open_chrony {
	int rc;
	sfptpd_short_text_t failing_step;
};

struct sfptpd_priv_resp_open_dev {
	int rc;
};

struct sfptpd_priv_resp_msg {
	enum sfptpd_priv_resp resp;
	union {
		struct sfptpd_priv_resp_open_chrony open_chrony;
		struct sfptpd_priv_resp_open_chrony open_dev;
	};
};

#endif /* _SFPTPD_PRIV_OPS_H */
