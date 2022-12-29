/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Xilinx, Inc. */

/**
 * @file   sfptpd_link.c
 * @brief  Utility functions for sfptpd link table
 */

#include <stdbool.h>
#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sfptpd_link.h"
#include "sfptpd_logging.h"
#include "sfptpd_thread.h"

/****************************************************************************
 * Defines & Constants
 ****************************************************************************/


/****************************************************************************
 * Local Variables
 ****************************************************************************/


/****************************************************************************
 * Forward function declarations
 ****************************************************************************/


/****************************************************************************
 * Local Functions
 ****************************************************************************/


/****************************************************************************
 * Public Functions
 ****************************************************************************/

const struct sfptpd_link *sfptpd_link_by_name(const struct sfptpd_link_table *link_table,
					      const char *link_name) {
	const struct sfptpd_link *link;
	int row;

	for (row = 0; row < link_table->count; row++) {
		if (strncmp(link_table->rows[row].if_name, link_name, IFNAMSIZ) == 0) {
			TRACE_L4("link: table %d: found link table entry for %s\n",
				 link_table->version, link_name);
			link = link_table->rows + row;
		}
	}
	if (link == NULL) {
		TRACE_L3("link: no entry in link table version %d for %s\n",
		      link_table->version, link_name);
		errno = ENOENT;
	}
	return link;
}


const struct sfptpd_link *sfptpd_link_by_if_index(const struct sfptpd_link_table *link_table,
					          int if_index) {
	const struct sfptpd_link *link;
	int row;

	for (row = 0; row < link_table->count; row++) {
		if (link_table->rows[row].if_index == if_index) {
			TRACE_L4("link: table %d: found link table entry for if_index %d\n",
				 link_table->version, if_index);
			link = link_table->rows + row;
		}
	}
	if (link == NULL) {
		TRACE_L3("link: no entry in link table version %d for if_index %d\n",
		      link_table->version, if_index);
		errno = ENOENT;
	}
	return link;
}
