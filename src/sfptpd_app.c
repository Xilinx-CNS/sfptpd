/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

/**
 * @file   sfptpd_app.c
 * @brief  Routines for building a generic application
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "sfptpd_logging.h"
#include "sfptpd_thread.h"
#include "sfptpd_message.h"
#include "sfptpd_app.h"


/****************************************************************************
 * Types
 ****************************************************************************/


/****************************************************************************
 * Constants
 ****************************************************************************/


/****************************************************************************
 * Private functions
 ****************************************************************************/


/****************************************************************************
 * Public functions
 ****************************************************************************/


void sfptpd_app_run(struct sfptpd_thread *component)
{
	struct message {
		sfptpd_msg_hdr_t hdr;
	} *msg;

	msg = (struct message *) sfptpd_msg_alloc(SFPTPD_MSG_POOL_GLOBAL, false);

	if (msg == NULL) {
		SFPTPD_MSG_LOG_ALLOC_FAILED("global");
		return;
	}

	(void)SFPTPD_MSG_SEND(msg, component,
			      SFPTPD_APP_MSG_RUN, false);
}


/* fin */
