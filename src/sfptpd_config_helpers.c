/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2025 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_config_helpers.c
 * @brief  Configuration option value parsing helpers for sfptpd
 */

#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <netdb.h>
#include <regex.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "sfptpd_config_helpers.h"


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_config_parse_net_addr(struct sockaddr_storage *ss,
				 const char *addr,
				 const char *context,
				 int af,
				 int socktype,
				 bool passive,
				 const char *def_serv)
{
	struct addrinfo *result;
	regex_t rquot;
	regex_t runquot;
	regmatch_t matches[4];
	char *spec = strdup(addr);
	regoff_t node;
	regoff_t serv;
	int gai_rc;
	struct addrinfo hints = {
		.ai_family = af,
		.ai_socktype = socktype,
		.ai_flags = passive ? AI_PASSIVE : 0,
	};
	int rc;

	assert(regcomp(&rquot, "^\\[(.*)](:([^:]*))?$", REG_EXTENDED) == 0);
	assert(regcomp(&runquot, "^([^:]*)(:([^:]*))?$", REG_EXTENDED) == 0);

	rc = regexec(&rquot, spec, sizeof matches / sizeof *matches, matches, 0);
	if (rc != 0)
		rc = regexec(&runquot, spec, sizeof matches / sizeof *matches, matches, 0);
	node = matches[1].rm_so;
	serv = matches[3].rm_so;
	if (rc != 0 || node == -1) {
		ERROR("invalid %s address: %s\n", context, spec);
		rc = -EINVAL;
		goto finish;
	}
	spec[matches[1].rm_eo] = '\0';
	if (serv != -1)
		spec[matches[3].rm_eo] = '\0';

	rc = 0;
	gai_rc = getaddrinfo(node == -1 || spec[node] == '\0' ? NULL : spec + node,
			     serv == -1 || spec[serv] == '\0' ? def_serv : spec + serv,
			     &hints, &result);
	if (gai_rc != 0 || result == NULL) {
		ERROR("%s address lookup for %s failed, %s\n", context,
		      spec, gai_strerror(gai_rc));
		rc = -EINVAL;
	} else {
		assert(result->ai_addrlen <= sizeof *ss);
		memcpy(ss, result->ai_addr, result->ai_addrlen);
		rc = result->ai_addrlen;
	}
	if (gai_rc == 0)
		freeaddrinfo(result);

finish:
	free(spec);
	regfree(&rquot);
	regfree(&runquot);
	return rc;
}

int sfptpd_config_parse_net_prefix(struct sfptpd_acl_prefix *buf,
				   const char *addr,
				   const char *context)
{
	regex_t r4;
	regex_t r6;
	regmatch_t matches[4];
	char *spec = strdup(addr);
	regoff_t prefix;
	regoff_t length;
	int rc = 0;
	int af;

	assert(regcomp(&r6, "^([[:xdigit:]:.]+)(/([[:digit:]]+))?$", REG_EXTENDED) == 0);
	assert(regcomp(&r4, "^([[:digit:].]+)(/([[:digit:]]+))?$", REG_EXTENDED) == 0);

	af = AF_INET;
	rc = regexec(&r4, spec, sizeof matches / sizeof *matches, matches, 0);
	if (rc != 0) {
		af = AF_INET6;
		rc = regexec(&r6, spec, sizeof matches / sizeof *matches, matches, 0);
	}
	prefix = matches[1].rm_so;
	length = matches[3].rm_so;
	if (rc != 0 || prefix == -1 || spec[prefix] == '\0') {
		ERROR("invalid %s prefix: %s\n", context, addr);
		rc = EINVAL;
		goto finish;
	}
	spec[matches[1].rm_eo] = '\0';
	if (length != -1)
		spec[matches[3].rm_eo] = '\0';

	if (af == AF_INET6) {
		*buf = (struct sfptpd_acl_prefix) { 0 };
		rc = inet_pton(af, spec + prefix, buf->in6.s6_addr);
	} else {
		assert(af == AF_INET);
		*buf = sfptpd_acl_v6mapped_prefix;
		rc = inet_pton(af, spec + prefix, buf->in6.s6_addr + 12);
	}

	if (rc == 0 && errno == 0)
		errno = EDESTADDRREQ;
	if (rc < 1) {
		ERROR("%s address parsing for %s failed, %s\n",
		      context, addr, strerror(errno));
		rc = errno;
	} else if (length != -1 && spec[length] != '\0') {
		buf->length += strtoul(spec + length, NULL, 10);
		rc = 0;
	} else {
		buf->length = 128;
		rc = 0;
	}

	if (rc == 0)
		sfptpd_acl_normalise_prefix(buf);

finish:
	free(spec);
	regfree(&r4);
	regfree(&r6);
	return rc;
}

/* fin */
