/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2024-2025 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_metrics.c
 * @brief  OpenMetrics exposition generation
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_clock.h"
#include "sfptpd_constants.h"
#include "sfptpd_statistics.h"
#include "sfptpd_engine.h"
#include "sfptpd_metrics.h"
#include "sfptpd_sync_module.h"


/****************************************************************************
 * Structures, Types
 ****************************************************************************/

/* Support more than one outstanding query because the time series database
 * may keep their connection open while we may also want to perform one-off
 * queries for diagnostic purposes. */
#define MAX_QUERIES 2
#define QUERIES_MASK ((((bitfield_t )1) << (MAX_QUERIES)) - 1)

enum openmetrics_type {
	OM_T_GAUGE,
	OM_T_STATESET,
	OM_T_INFO,
	OM_T_COUNTER,
};

enum openmetrics_unit {
	OM_U_NONE,
	OM_U_SECONDS,
	OM_U_RATIOS,
};

struct openmetrics_family {
	enum openmetrics_type type;
	const char *name;
	enum openmetrics_unit unit;
	const char *help;
	sfptpd_metrics_flags_t conditional;
};

enum sfptpd_metric_family {
	OM_F_OFFSET,
	OM_F_FREQ_ADJ,
	OM_F_OWD,
	OM_F_PTERM,
	OM_F_ITERM,
	OM_F_IN_SYNC,
	OM_F_IS_DISC,
	OM_F_M_TIME,
	OM_F_S_TIME,
	OM_F_LOG_TIME,
	OM_F_ALARMS,
	OM_F_ALARM,
	OM_F_ALARMTXT,
	OM_F_LOST_RT,
	OM_F_SERVO,
};

struct instance_scope_metric {
	enum sfptpd_rt_stats_key key;
	enum sfptpd_metric_family family;
};

struct rt_stats_buf {
	struct sfptpd_sync_instance_rt_stats_entry *entries;
	int wr_ptr;
	int len;
	int sz;

	int64_t lost_samples;
};

static const size_t net_buf_initial_capacity = 256;
static const size_t net_buf_max_capacity = 512;
struct net_buf {
	char *data;
	size_t capacity;
	size_t len;
	size_t rd_ptr;
};

enum http_parse_st {
	HP_REQ_METHOD,
	HP_REQ_TARGET,
	HP_REQ_PROTOCOL,
	HP_REQ_VERSION_MAJOR,
	HP_REQ_VERSION_MINOR,
	HP_REQ_HDR_CR,
	HP_REQ_HDR_SEP,
	HP_REQ_HDR_NAME,
	HP_REQ_HDR_VALUE,
	HP_REQ_HDR_END,
	HP_REQ_BODY,
};

enum http_req_action {
	HP_REQ_NO_ACTION,
	HP_REQ_ACT_ON_HEADER,
	HP_REQ_ACT_ON_BODY,
};

enum http_headers {
	HEADER_CONTENT_LENGTH = 01,
	HEADER_TRANSFER_ENCODING = 02,
};

enum http_method {
	HTTP_METHOD_OTHER,
	HTTP_METHOD_GET,
	HTTP_METHOD_HEAD,
	HTTP_METHOD_CONNECT,
};

struct http_header {
	const char *name;
	char *value;
	struct http_header *next;
};

struct http_chunk {
	char *data;
	size_t length;
	struct http_chunk *next;
	bool alloced;
};

struct http {
	enum http_parse_st state;
	enum http_req_action action;
	enum http_headers headers;
	enum http_method method;
	ssize_t cursor;
	char protocol[8];
	char method_s[16];
	char target[64];
	char field_name[40];
	char field_value[400];
	int64_t major_version;
	int64_t minor_version;

	struct http_header *reply_headers;
	struct http_chunk *reply_body;
	struct http_chunk **reply_body_tail;
	int reply_chunks;
	size_t reply_length;
	int response_code;
	const char *response_text;
};

struct listener {
	int fd;
	struct listener *next;
};

struct query_state {
	struct http http;
	int fd;
	int fd_flags;
	struct net_buf rx;
	bool abort;
};

typedef unsigned int bitfield_t;
typedef signed int bitindex_t;

struct metrics_state {
	const struct sfptpd_config_metrics *config;
	struct rt_stats_buf rt_stats;
	bool initialised;
	struct listener *listeners;

	char *exemplars;
	size_t exemplars_len;
	struct query_state query[MAX_QUERIES];
	bitfield_t active_queries;
};

enum stats_format {
	/* https://jsonlines.org/ */
	/* https://github.com/ndjson/ndjson-spec */
	JSON_LINES,

	/* RFC7464 */
	JSON_SEQ,

	/* Classic sfptpd stats log text */
	STATS_LOG,
};


/****************************************************************************
 * Defines & Constants
 ****************************************************************************/

#define PREFIX "metrics: "

const char *json_content_type[] = {
	[JSON_LINES] = "application/x-ndjson",
	[JSON_SEQ] = "application/json-seq",
	[STATS_LOG] = "text/plain",
};

const char *sfptpd_metrics_option_names[SFPTPD_METRICS_NUM_OPTIONS] = {
	[SFPTPD_METRICS_OPTION_ALARM_STATESET] = "alarm-stateset",
	[SFPTPD_METRICS_OPTION_SERVO_TIMES] = "servo-times",
};

static const struct openmetrics_family sfptpd_metric_families[] = {
	[ OM_F_OFFSET   ] = { OM_T_GAUGE, "offset",   OM_U_SECONDS, "offset from master" },
	[ OM_F_FREQ_ADJ ] = { OM_T_GAUGE, "freq_adj", OM_U_RATIOS,  "frequency adjustment" },
	[ OM_F_OWD      ] = { OM_T_GAUGE, "owd",      OM_U_SECONDS, "one way delay" },
	[ OM_F_PTERM    ] = { OM_T_GAUGE, "pterm",    OM_U_RATIOS,  "p-term" },
	[ OM_F_ITERM    ] = { OM_T_GAUGE, "iterm",    OM_U_RATIOS,  "i-term" },
	[ OM_F_M_TIME   ] = { OM_T_GAUGE, "m_time",   OM_U_SECONDS, "servo master time snapshot",
			      .conditional = 1 << SFPTPD_METRICS_OPTION_SERVO_TIMES},
	[ OM_F_S_TIME   ] = { OM_T_GAUGE, "s_time",   OM_U_SECONDS, "servo slave time snapshot",
			      .conditional = 1 << SFPTPD_METRICS_OPTION_SERVO_TIMES},
	[ OM_F_LOG_TIME ] = { OM_T_GAUGE, "last_update",
						      OM_U_SECONDS, "time sfptpd recorded rt stat" },
	[ OM_F_IN_SYNC  ] = { OM_T_GAUGE, "in_sync",  OM_U_NONE,    "0 = not in sync, 1 = in sync" },
	[ OM_F_IS_DISC  ] = { OM_T_GAUGE, "is_disciplining",
						      OM_U_NONE,    "0 = comparing, 1 = disciplining" },
	[ OM_F_ALARMS   ] = { OM_T_GAUGE, "alarms",   OM_U_NONE,    "number of alarms" },
	[ OM_F_ALARMTXT ] = { OM_T_INFO,  "alarmtxt", OM_U_NONE,    "alarm text" },
	[ OM_F_LOST_RT  ] = { OM_T_COUNTER,
					  "lost_rt",  OM_U_NONE,    "lost rt stats samples" },
	[ OM_F_ALARM    ] = { OM_T_STATESET,
					  "alarm",    OM_U_NONE,    "alarm",
			      .conditional = 1 << SFPTPD_METRICS_OPTION_ALARM_STATESET},
	[ OM_F_SERVO    ] = { OM_T_INFO,  "servo",    OM_U_NONE,    "information about the servo" },
};
#define NUM_METRIC_FAMILIES (sizeof sfptpd_metric_families/sizeof *sfptpd_metric_families)

static const struct instance_scope_metric sfptpd_instance_metrics[] = {
	{ STATS_KEY_OFFSET, OM_F_OFFSET },
	{ STATS_KEY_FREQ_ADJ, OM_F_FREQ_ADJ },
	{ STATS_KEY_OWD, OM_F_OWD },
	{ STATS_KEY_P_TERM, OM_F_PTERM },
	{ STATS_KEY_I_TERM, OM_F_ITERM },
};
#define NUM_INSTANCE_METRICS (sizeof sfptpd_instance_metrics/sizeof *sfptpd_instance_metrics)

/****************************************************************************
 * Local Variables
 ****************************************************************************/

static struct metrics_state metrics;


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static const char *metric_type_str(enum openmetrics_type t) {
	switch (t) {
	case OM_T_GAUGE:
		return "gauge";
	case OM_T_STATESET:
		return "stateset";
	default:
		return "unknown";
	}
}

static const char *metric_unit_str(enum openmetrics_unit u) {
	switch (u) {
	case OM_U_NONE:
		return "";
	case OM_U_SECONDS:
		return "seconds";
	case OM_U_RATIOS:
		return "ratios";
	default:
		return "unknown";
	}
}

static sfptpd_time_t metric_float_value(struct sfptpd_sync_instance_rt_stats_entry *entry,
					enum sfptpd_rt_stats_key key)
{
	switch (key) {
		case STATS_KEY_OFFSET:
			return entry->offset / 1000000000.0L;
		case STATS_KEY_FREQ_ADJ:
			return entry->freq_adj;
		case STATS_KEY_OWD:
			return entry->one_way_delay / 1000000000.0L;
		case STATS_KEY_PPS_OFFSET:
			return entry->pps_offset / 1000000000.0L;
		case STATS_KEY_BAD_PERIOD:
			return (sfptpd_time_t) entry->bad_period_count;
		case STATS_KEY_OVERFLOWS:
			return (sfptpd_time_t) entry->overflow_count;
		case STATS_KEY_P_TERM:
			return entry->p_term;
		case STATS_KEY_I_TERM:
			return entry->i_term;
		default:
			return NAN;
	}
}

static struct sfptpd_thread_readyfd *get_event_for(int num_fds,
						   struct sfptpd_thread_readyfd *fds,
						   int fd)
{
	int i;

	for (i = 0; fd != -1 && i < num_fds; i++)
		if (fds[i].fd == fd)
			return fds + i;
	return NULL;
}

static void http_init_reply(struct http *http)
{
	http->reply_headers = NULL;
	http->reply_body = NULL;
	http->reply_body_tail = &http->reply_body;
	http->reply_length = 0;
	http->reply_chunks = 0;
	http->response_code = 500;
	http->response_text = NULL;
}

static void http_finit_reply(struct http *http)
{
	struct http_header *header;
	struct http_header *header_next;
	struct http_chunk *chunk;
	struct http_chunk *chunk_next;

	for (header = http->reply_headers; header; header = header_next) {
		header_next = header->next;
		free(header->value);
		free(header);
	}

	for (chunk = http->reply_body; chunk; chunk = chunk_next) {
		chunk_next = chunk->next;
		if (chunk->alloced)
			free(chunk->data);
		free(chunk);
	}

	/* Now zero out values */
	http_init_reply(http);
}

static void http_abort(struct query_state *q, const char *reason)
{
	ERROR("metrics: http request abort (%s)\n", reason);
	http_finit_reply(&q->http);
	q->abort = true;
}

static int writev_all(struct query_state *q, struct iovec *iov, int iovcnt)
{
	ssize_t ret = 0;

	while (iovcnt && ret != -1) {
		if (ret == 0) {
			ret = writev(q->fd, iov, iovcnt);
		} else if (ret >= iov->iov_len) {
			ret -= iov->iov_len;
			iov++;
			iovcnt--;
		} else {
			iov->iov_base = ((char *) iov->iov_base) + ret;
			iov->iov_len -= ret;
		}
	}

	if (ret == -1) {
		ret = errno;
		ERROR("metrics: error writing response: %s\n", strerror(ret));
		return ret;
	} else {
		return 0;
	}
}

static int http_add_header(struct http *http, const char *name, const char *fmt, ...)
{
	va_list ap;
	struct http_header *header;
	int ret;

	header = calloc(1, sizeof *header);
	if (header == NULL) {
		CRITICAL("metrics: allocating http header, %s\n", strerror(ret = errno));
		goto fail;
	}

	header->name = name;
	va_start(ap, fmt);
	ret = vasprintf(&header->value, fmt, ap);
	va_end(ap);

	if (ret == -1) {
		CRITICAL("metrics: adding http header, %s\n", strerror(ret = errno));
		free(header);
		goto fail;
	}
	ret = 0;

	header->next = http->reply_headers;
	http->reply_headers = header;
fail:
	return ret;
}

static int http_add_chunk(struct http *http, bool alloced, char *data, size_t length)
{
	struct http_chunk *chunk;
	int ret = 0;

	chunk = calloc(1, sizeof *chunk);
	if (chunk == NULL) {
		CRITICAL("metrics: allocating http chunk, %s\n", strerror(ret = errno));
		return ret;
	}

	chunk->data = data;
	chunk->length = length;
	chunk->alloced = alloced;

	*http->reply_body_tail = chunk;
	http->reply_body_tail = &chunk->next;
	http->reply_length += length;
	http->reply_chunks++;
	return ret;
}

static int http_write_chunk(struct http *http, const char *fmt, ...)
{
	va_list ap;
	char *data;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(&data, fmt, ap);
	va_end(ap);

	if (ret == -1) {
		CRITICAL("metrics: writing http reply chunk, %s\n", strerror(ret = errno));
		return ret;
	}

	ret = http_add_chunk(http, true, data, ret);
	if (ret != 0)
		free(data);
	return ret;
}

static int http_response(struct query_state *q)
{
	struct http *http = &q->http;
	struct http_header *header;
	struct http_chunk *chunk;
	char *response;
	ssize_t len;
	ssize_t ret = 0;
	FILE *block_stream;
	char *block = NULL;
	size_t block_sz = 0;
	struct iovec *iov = NULL;
	bool forbidden_body = false;
	int iovcnt;
	int i;

	if (http->response_text == NULL) {
		switch (http->response_code) {
		case 404:
			http->response_text = "Not Found";
			break;
		case 200:
			http->response_text = "OK";
			break;
		case 500:
		default:
			http->response_text = "Internal Server Error";
		}
	}

	if (http->reply_length == 0) {
		char *text_reply = NULL;
		switch (http->response_code) {
		case 404:
			text_reply = "Resource not found\n";
			break;
		case 500:
			text_reply = "Internal server error\n";
			break;
		default:
			break;
		}
		if (text_reply) {
			http_write_chunk(http, text_reply);
			http_add_header(http, "Content-Type", "text/plain");
		}
	}

	len = asprintf(&response, "HTTP/1.1 %d %s\r\n",
		       http->response_code, http->response_text);
	if (len == -1) {
		CRITICAL("formatting HTTP response\n");
		response = NULL;
		goto fail;
	}

	/* RFC7230 3.3.3 */
	assert(http->method != HTTP_METHOD_CONNECT ||
	       (http->response_code >= 500 && http->response_code <= 599));
	if (http->method == HTTP_METHOD_HEAD ||
	    (http->response_code >= 100 && http->response_code < 199) ||
	    http->response_code == 204 ||
	    http->response_code == 304) {
		forbidden_body = true;
		iovcnt = 2;
	} else {
		iovcnt = 2 + http->reply_chunks;
	}

	if ((!forbidden_body ||
	     (http->method == HTTP_METHOD_HEAD && http->reply_length != 0)) &&
	    http_add_header(http, "Content-Length", "%zd", http->reply_length) != 0)
		goto fail;

	if ((block_stream = open_memstream(&block, &block_sz)) == NULL) {
		http_abort(q, "allocating header block");
		goto fail;
	}
	for (header = http->reply_headers; header; header = header->next) {
		fprintf(block_stream, "%s: %s\r\n", header->name, header->value);
	}
	fprintf(block_stream, "\r\n");
	if (fclose(block_stream) == -1) {
		http_abort(q, "preparing header block");
		goto fail;
	}

	if ((iov = calloc(iovcnt, sizeof *iov)) == NULL) {
		http_abort(q, "allocating sending iov");
		goto fail;
	}
	iov[0].iov_base = response;
	iov[0].iov_len = len;
	iov[1].iov_base = block;
	iov[1].iov_len = block_sz;
	chunk = http->reply_body;
	for (i = 2; i < iovcnt; i++) {
		assert(chunk);
		assert(i - 2 < http->reply_chunks);
		iov[i].iov_base = chunk->data;
		iov[i].iov_len = chunk->length;
		chunk = chunk->next;
	}
	ret = writev_all(q, iov, iovcnt);
	if (ret != 0)
		http_abort(q, "sending response code");
	free(iov);

fail:
	free(block);
	free(response);
	http_finit_reply(http);
	if (ret != 0)
		http_abort(q, "sending reply");
	return ret;
}

static void write_exemplar_family(FILE *stream,
				  const struct openmetrics_family *family,
				  const char *qualifier, const char *help)
{
	const char *prefix = metrics.config->family_prefix;

	fprintf(stream, "# TYPE %s%s%s%s%s %s\n", prefix, family->name,
		qualifier ? qualifier : "",
		family->unit ? "_" : "",
		family->unit ? metric_unit_str(family->unit) : "",
		metric_type_str(family->type));
	if (family->unit != OM_U_NONE)
		fprintf(stream, "# UNIT %s%s%s%s%s %s\n", prefix, family->name,
			qualifier ? qualifier : "",
			family->unit ? "_" : "",
			family->unit ? metric_unit_str(family->unit) : "",
			metric_unit_str(family->unit));
	if (family->help)
		fprintf(stream, "# HELP %s%s%s%s%s %s%s\n", prefix, family->name,
			qualifier ? qualifier : "",
			family->unit ? "_" : "",
			family->unit ? metric_unit_str(family->unit) : "",
			family->help, help ? help : "");
}

static int write_exemplars(void)
{
	size_t buf_sz = 0;
	char *buf = NULL;
	FILE *stream;
	int rc;
	int m;

	stream = open_memstream(&buf, &buf_sz);
	if (stream == NULL) {
		CRITICAL("metrics: could not prepare exemplars, %s", strerror(rc = errno));
		return rc;
	}

	/* Write exemplars */
	for (m = 0; m < NUM_METRIC_FAMILIES; m++) {
		const struct openmetrics_family *family = sfptpd_metric_families + m;

		if (!(family->conditional & ~metrics.config->flags))
			write_exemplar_family(stream, family, NULL, "");
	}
	for (m = 0; m < NUM_INSTANCE_METRICS; m++) {
		const struct instance_scope_metric *metric = sfptpd_instance_metrics + m;
		const struct openmetrics_family *family = sfptpd_metric_families + metric->family;

		if (!(family->conditional & ~metrics.config->flags))
			write_exemplar_family(stream, family, "_snapshot", " (snapshot)");
	}

	fclose(stream);
	assert(buf && buf_sz > 0);
	metrics.exemplars = buf;
	metrics.exemplars_len = buf_sz;
	return 0;
}

static int sfptpd_metrics_send(struct query_state *q, bool peek)
{
	const char *content_type = "application/openmetrics-text"
				   "; version=1.0.0"
				   "; charset=utf-8";
	struct rt_stats_buf *stats = &metrics.rt_stats;
	char alarm_str[SYNC_MODULE_ALARM_ALL_TEXT_MAX];
	const char *prefix = metrics.config->family_prefix;
	struct http *h = &q->http;
	size_t buf_sz = 0;
	char *buf = NULL;
	int count = 0;
	FILE *stream;
	int rc = 0;
	int m;

	if (h->method == HTTP_METHOD_GET) {
		stream = open_memstream(&buf, &buf_sz);
		if (stream == NULL) {
			http_abort(q, "open_memstream");
			return errno;
		}

		/* Write snapshot that the ingestor will timestamp */
		if (stats->len) {
			struct sfptpd_sync_instance_rt_stats_entry *entry = stats->entries + stats->wr_ptr - 1;
			if (entry < stats->entries)
				entry += stats->sz;

			for (m = 0; m < NUM_INSTANCE_METRICS; m++) {
				const struct instance_scope_metric *metric = sfptpd_instance_metrics + m;
				const struct openmetrics_family *family = sfptpd_metric_families + metric->family;

				if ((family->conditional & ~metrics.config->flags) == 0 &&
				    (entry->stat_present & (1 << metric->key)))
					fprintf(stream, "%s%s_snapshot%s%s{sync=\"%s\"} %.12Lf\n",
						prefix, family->name,
						family->unit ? "_" : "",
						family->unit ? metric_unit_str(family->unit) : "",
						entry->instance_name,
						metric_float_value(entry, metric->key));
			}

			const struct openmetrics_family *family = sfptpd_metric_families + OM_F_ALARM;
			sfptpd_sync_module_alarms_t abit;

			if ((family->conditional & ~metrics.config->flags) == 0) {
				for (abit = 1; abit != SYNC_MODULE_ALARM_MAX; abit <<= 1) {
					sfptpd_sync_module_alarms_text(abit, alarm_str, sizeof alarm_str);
					fprintf(stream, "%s%s{sync=\"%s\",%s=\"%s\"} %c\n", prefix,
						family->name, entry->instance_name,
						family->name, alarm_str,
						entry->alarms & abit ? '1' : '0');
				}
			}

			family = sfptpd_metric_families + OM_F_ALARMTXT;
			sfptpd_sync_module_alarms_text(entry->alarms, alarm_str, sizeof alarm_str);
			fprintf(stream, "%s%s{sync=\"%s\",alarms=\"%s\"} 1\n", prefix,
				family->name, entry->instance_name,
				alarm_str);

			family = sfptpd_metric_families + OM_F_ALARMS;
			fprintf(stream, "%s%s{sync=\"%s\"} %d\n", prefix,
				family->name, entry->instance_name,
				__builtin_popcount(entry->alarms));

			family = sfptpd_metric_families + OM_F_IN_SYNC;
			fprintf(stream, "%s%s{sync=\"%s\"} %d\n", prefix,
				family->name, entry->instance_name, entry->is_in_sync);

			family = sfptpd_metric_families + OM_F_IS_DISC;
			fprintf(stream, "%s%s{sync=\"%s\"} %d\n", prefix,
				family->name, entry->instance_name, entry->is_disciplining);

			family = sfptpd_metric_families + OM_F_LOG_TIME;
			fprintf(stream, "%s%s%s%s{sync=\"%s\"} " SFPTPD_FMT_SSFTIMESPEC_NS "\n",
				prefix, family->name,
				family->unit ? "_" : "",
				family->unit ? metric_unit_str(family->unit) : "",
				entry->instance_name,
				SFPTPD_ARGS_SSFTIMESPEC_NS(entry->log_time));

			family = sfptpd_metric_families + OM_F_LOST_RT;
			fprintf(stream, "%s%s %" PRId64 "\n", prefix,
				family->name, metrics.rt_stats.lost_samples);

			family = sfptpd_metric_families + OM_F_M_TIME;
			if ((family->conditional & ~metrics.config->flags && entry->has_m_time) == 0)
				fprintf(stream, "%s%s{sync=\"%s\"} " SFPTPD_FMT_SSFTIMESPEC_NS "\n",
					prefix, family->name, entry->instance_name,
					SFPTPD_ARGS_SSFTIMESPEC_NS(entry->time_master));

			family = sfptpd_metric_families + OM_F_S_TIME;
			if ((family->conditional & ~metrics.config->flags && entry->has_s_time) == 0)
				fprintf(stream, "%s%s{sync=\"%s\"} " SFPTPD_FMT_SSFTIMESPEC_NS "\n",
					prefix, family->name, entry->instance_name,
					SFPTPD_ARGS_SSFTIMESPEC_NS(entry->time_slave));


			family = sfptpd_metric_families + OM_F_SERVO;
			fprintf(stream, "%s%s_info{sync=\"%s\",clock=\"%s\",desc=\"%s%s%s%s%s%s%s%s%s\"} 1\n",
				prefix, family->name, entry->instance_name,
				sfptpd_clock_get_short_name(entry->clock_slave),
				sfptpd_clock_get_long_name(entry->clock_slave),
				entry->source ? "\",source=\"" : "",
				entry->source,
				entry->clock_master ? "\",master=\"" : "",
				entry->clock_master ? sfptpd_clock_get_short_name(entry->clock_master) : "",
				entry->stat_present & (1 << STATS_KEY_ACTIVE_INTF) ? "\",active_intf=\"" : "",
				entry->stat_present & (1 << STATS_KEY_ACTIVE_INTF) ? sfptpd_interface_get_name(entry->active_intf) : "",
				entry->stat_present & (1 << STATS_KEY_BOND_NAME) ? "\",bond=\"" : "",
				entry->stat_present & (1 << STATS_KEY_BOND_NAME) ? entry->bond_name : ""
				);
		}

		/* Write exposition of RT stats with our timestamp */
		for (count = 0; count < stats->len; count++) {
			struct sfptpd_sync_instance_rt_stats_entry *entry;

			entry = stats->entries + stats->wr_ptr - stats->len + count;
			if (entry < stats->entries)
				entry += stats->sz;

			for (m = 0; m < NUM_INSTANCE_METRICS; m++) {
				const struct instance_scope_metric *metric = sfptpd_instance_metrics + m;
				const struct openmetrics_family *family = sfptpd_metric_families + metric->family;

				if ((family->conditional & ~metrics.config->flags) == 0 &&
				    entry->stat_present & (1 << metric->key))
					fprintf(stream, "%s%s%s%s{sync=\"%s\"} %.12Lf " SFPTPD_FMT_SSFTIMESPEC_NS "\n",
						prefix, family->name,
						family->unit ? "_" : "",
						family->unit ? metric_unit_str(family->unit) : "",
						entry->instance_name,
						metric_float_value(entry, metric->key),
						SFPTPD_ARGS_SSFTIMESPEC_NS(entry->log_time));
			}
		}

		/* End OpenMetrics */
		fprintf(stream, "# EOF\n");
		if (fclose(stream) == EOF) {
			http_abort(q, "formatting body");
			rc = errno;
			goto finish;
		}
	}

	if ((rc = http_add_header(h, "Content-Type", "%s", content_type))) {
		http_abort(q, "adding headers");
		goto finish;
	}

	if (h->method == HTTP_METHOD_GET &&
	    ((rc = http_add_chunk(h, false, metrics.exemplars, metrics.exemplars_len)) ||
	     (rc = http_add_chunk(h, true, buf, buf_sz)))) {
		http_abort(q, "adding body");
		goto finish;
	}
	/* We don't own the buffer anymore so don't free it. */
	buf = NULL;

	if (h->method == HTTP_METHOD_GET && !peek && stats->len != 0) {
		/* Always leave one record left for stateless
		 * ingestion of current state. Yes this can result in
		 * repetition; no, they don't mind that. */
		stats->len = 1;
		stats->lost_samples = 0;
	}

	TRACE_L5("metrics: completed query, writing %d rt stats entries\n",
		 count);
	h->response_code = 200;
finish:
	free(buf);
	return rc;
}

static int sfptpd_rt_stats_send(struct query_state *q, bool peek,
				enum stats_format format)
{
	struct sfptpd_log_time_cache log_time_cache = { 0 };
	struct rt_stats_buf *stats = &metrics.rt_stats;
	struct http *h = &q->http;
	size_t buf_sz = 0;
	char *buf = NULL;
	int count = 0;
	FILE *stream;
	int rc = 0;

	if (h->method == HTTP_METHOD_GET) {
		stream = open_memstream(&buf, &buf_sz);
		if (stream == NULL) {
			http_abort(q, "open_memstream");
			return errno;
		}

		/* Write exposition of RT stats with our timestamp */
		for (count = 0; count < stats->len; count++) {
			struct sfptpd_sync_instance_rt_stats_entry *entry;

			entry = stats->entries + stats->wr_ptr - stats->len + count;
			if (entry < stats->entries)
				entry += stats->sz;

			if (format == JSON_SEQ)
				fputc('\x1e', stream);

			if (format == STATS_LOG)
				sfptpd_log_render_rt_stat_text(&log_time_cache, stream, entry);
			else if (sfptpd_log_render_rt_stat_json(&log_time_cache, stream, entry) == -1) {
				http_abort(q, "rendering stat");
				rc = errno;
				goto finish;
			}
		}
		if (fclose(stream) == EOF) {
			http_abort(q, "formatting body");
			rc = errno;
			goto finish;
		}
		if (buf_sz)
			http_add_chunk(h, true, buf, buf_sz);
		else
			free(buf);
	}

	/* Output header */
	if ((rc = http_add_header(h, "Content-Type", "%s; charset=utf-8", json_content_type[format])) ||
	    (rc = http_add_header(h, "X-Sfptpd-Lost-Samples", "%" PRId64, stats->lost_samples))) {
		http_abort(q, "adding headers");
		goto finish;
	}

	if (q->http.method == HTTP_METHOD_GET && !peek) {
		stats->len = 0;
		stats->lost_samples = 0;
	}

	TRACE_L5("metrics: completed query, writing %d rt stats entries as JSON\n",
		 count);
	h->response_code = 200;
finish:
	return rc;
}

static void netbuf_free(struct net_buf *nb)
{
	free(nb->data);
	memset(nb, '\0', sizeof *nb);
}

static int netbuf_init(struct net_buf *nb)
{
	void *newalloc;
	int rc;

	if (nb->capacity < net_buf_initial_capacity) {
		nb->capacity = net_buf_initial_capacity;
		newalloc = realloc(nb->data, nb->capacity);
		if (newalloc == NULL) {
			rc = errno;
			netbuf_free(nb);
			return rc;
		}
		nb->data = newalloc;
	}

	nb->len = 0;
	nb->rd_ptr = 0;
	return 0;
}

static inline bool queries_busy(void)
{
	return (metrics.active_queries ^ QUERIES_MASK) == 0;
}

static void listeners_xoff(void)
{
	struct listener *l;

	for (l = metrics.listeners; l; l = l->next)
		sfptpd_thread_user_fd_remove(l->fd);
}

static void listeners_xon(void)
{
	struct listener *l;

	for (l = metrics.listeners; l; l = l->next)
		sfptpd_thread_user_fd_add(l->fd, true, false);
}

static int metrics_handle_connection(int fd)
{
	struct sockaddr_storage peer;
	socklen_t peer_len = sizeof peer;
	char str[INET6_ADDRSTRLEN] = "<>";
	struct in6_addr addr;
	bool pass = false;
	int rc = 0;
	int qi;

	if (queries_busy()) {
		ERROR("metrics: too many active queries; discarding\n");
		goto fail;
	}

	rc = getpeername(fd, (struct sockaddr *) &peer, &peer_len);
	if (rc != 0) {
		ERROR("metrics: getpeername: %s\n", strerror(errno));
		goto fail;
	}

	if (peer.ss_family == AF_INET) {
		addr = sfptpd_acl_map_v4_addr(((struct sockaddr_in *) &peer)->sin_addr);
	} else if (peer.ss_family == AF_INET6) {
		addr = ((struct sockaddr_in6 *) &peer)->sin6_addr;
	} else {
		pass = true;
	}

	if (!pass) {
		inet_ntop(AF_INET6, &addr, str, sizeof str);
		pass = sfptpd_acl_match(&metrics.config->acl, &addr);
	}

	TRACE_LX(pass ? 5 : 3,
		 "metrics: incoming connection %s from %s\n",
		 pass ? "accepted" : "denied", str);

	if (!pass)
		goto fail;

	rc = sfptpd_thread_user_fd_add(fd, true, false);
	if (rc != 0)
		goto fail;

	/* Find first free query slot */
	qi = __builtin_ctz(~metrics.active_queries);

	memset(&metrics.query[qi].http, '\0', sizeof metrics.query[qi].http);
	metrics.query[qi].abort = false;
	if (netbuf_init(&metrics.query[qi].rx))
		goto fail;
	metrics.query[qi].fd = fd;
	metrics.query[qi].fd_flags = fcntl(fd, F_GETFL);
	metrics.active_queries |= (1 << qi);

	/* Rate control the backlog handling so we never reach the
	 * above discard case. */
	if (queries_busy())
		listeners_xoff();
	return 0;

fail:
	close(fd);
	return rc;
}

static void netbuf_advance(struct net_buf *nb, size_t amount)
{
	assert(amount <= nb->len);
	nb->rd_ptr += amount;
	if (nb->rd_ptr > nb->capacity)
		nb->rd_ptr -= nb->capacity;
	nb->len -= amount;
}

static void http_advance(struct query_state *q, size_t amount)
{
	netbuf_advance(&q->rx, amount);
	q->http.cursor -= amount;
}

static char netbuf_read(struct net_buf *nb, size_t cursor)
{
	/* This should already have been checked. */
	assert(cursor < nb->len);

	if (cursor >= nb->len)
		return -1;
	else
		return nb->data[nb->rd_ptr +
				((cursor + nb->rd_ptr < nb->capacity) ?
				 cursor : cursor - nb->capacity)];
}

static bool http_copystr_into(struct query_state *q,
			      char *target,
			      size_t capacity,
			      const char *on_error)
{
	struct net_buf *nb = &q->rx;
	size_t n1, n2;
	size_t len = q->http.cursor;
	bool success = true;

	if (len >= capacity) {
		success = false;
		if (on_error != NULL) {
			http_abort(q, on_error);
		} else {
			assert(capacity > 0);
			target[0] = '\0';
		}
		goto finish;
		return false;
	}

	if (nb->rd_ptr + len >= nb->capacity) {
		n1 = nb->capacity - nb->rd_ptr;
		n2 = len - n1;
	} else {
		n1 = len;
		n2 = 0;
	}

	memcpy(target, nb->data + nb->rd_ptr, n1);
	if (n2)
		memcpy(target + n1, nb->data, n2);
	target[len] = '\0';
finish:
	http_advance(q, len);
	return success;
}

static void http_copydec_into(struct query_state *q,
			      int64_t *target)
{
	struct net_buf *nb = &q->rx;
	size_t len = q->http.cursor;
	bool negative = false;
	int64_t v = 0LL;
	int ptr;

	for (ptr = 0; ptr < len && !q->abort; ptr++) {
		char c = netbuf_read(nb, ptr);
		if (c == '-')
			if (ptr != 0)
				http_abort(q, "negation out of place");
			else
				negative = true;
		else if (!isdigit(c))
			http_abort(q, "non-digit found");
		else
			if ((v = v * 10 + c - '0') < 0LL)
				http_abort(q, "overflow");
	}
	if (negative)
		v = -v;
	if (!q->abort)
		*target = v;
	http_advance(q, len);
}

static void handle_query_data(struct query_state *q)
{
	struct net_buf *nb = &q->rx;
	struct http *http = &q->http;
	char c;

	c = netbuf_read(nb, http->cursor);
	http->action = HP_REQ_NO_ACTION;

	switch (http->state) {
	case HP_REQ_METHOD:
		if (c == ' ') {
			http->state = HP_REQ_TARGET;
			http_copystr_into(q, http->method_s,
					  sizeof http->method_s,
					  "method too long");
			http_advance(q, 1); /* swallow delimiter */
		}
		break;
	case HP_REQ_TARGET:
		if (c == ' ') {
			http->state = HP_REQ_PROTOCOL;
			http_copystr_into(q, http->target,
					  sizeof http->target,
					  "resource name too long");
			http_advance(q, 1); /* swallow delimiter */
		}
		break;
	case HP_REQ_PROTOCOL:
		if (c == '/') {
			http->state = HP_REQ_VERSION_MAJOR;
			http_copystr_into(q, http->protocol,
					  sizeof http->protocol,
					  "protocol name too long");
			http_advance(q, 1); /* swallow delimiter */
		}
		break;
	case HP_REQ_VERSION_MAJOR:
		if (c == '.') {
			http->state = HP_REQ_VERSION_MINOR;
			http_copydec_into(q, &http->major_version);
			http_advance(q, 1); /* swallow delimiter */
		} else if (!isdigit(c)) {
			http_abort(q, "non-numeric http version");
		}
		break;
	case HP_REQ_VERSION_MINOR:
		if (c == '\r' || c == '\n') {
			http->state = c == '\r' ? HP_REQ_HDR_CR : HP_REQ_HDR_NAME;
			http_copydec_into(q, &http->minor_version);
			http_advance(q, 1); /* swallow delimiter */
		} else if (!isdigit(c)) {
			http_abort(q, "non-numeric http version");
		}
		break;
	case HP_REQ_HDR_CR:
		if (c == '\n') {
			http->state = HP_REQ_HDR_NAME;
			http_advance(q, 1); /* swallow delimiter */
		} else {
			http_abort(q, "expected LF");
		}
		break;
	case HP_REQ_HDR_NAME:
		if (c == ':') {
			http->state = HP_REQ_HDR_SEP;
			http_copystr_into(q, http->field_name,
					  sizeof http->field_name,
					  "field name too long");
			http_advance(q, 1); /* swallow delimiter */
		} else if (c == ' ' || c == '\t') {
			http_abort(q, "obs-fold not supported");
		} else if (c == '\r' || c == '\n') {
			if (http->cursor == 0) {
				if (c == '\r') {
					http->state = HP_REQ_HDR_END;
				} else {
					http->state = HP_REQ_BODY;
					http->action = HP_REQ_ACT_ON_BODY;
				}
			} else {
				http_abort(q, "missing field value");
			}
			http_advance(q, 1); /* swallow delimiter */
		}
		break;
	case HP_REQ_HDR_END:
		if (c == '\n') {
			http->state = HP_REQ_BODY;
			http->action = HP_REQ_ACT_ON_BODY;
			http_advance(q, 1); /* swallow delimiter */
		} else {
			http_abort(q, "expected LF");
		}
		break;
	case HP_REQ_HDR_SEP:
		if (c == ' ' || c == '\t') {
			http_advance(q, 1); /* swallow whitespace */
			break;
		}
		http->state = HP_REQ_HDR_VALUE;
		/* fallthrough */
	case HP_REQ_HDR_VALUE:
		if (c == '\r' || c == '\n') {
			size_t length = http->cursor;
			http->state = c == '\r' ? HP_REQ_HDR_CR : HP_REQ_HDR_NAME;
			if (http_copystr_into(q, http->field_value,
					       sizeof http->field_value, NULL)) {
				http->action = HP_REQ_ACT_ON_HEADER;
			} else {
				WARNING("metrics: ignoring %s: header of %zd bytes\n",
					http->field_name, length);
			}
			http_advance(q, 1); /* swallow delimiter */
		}
		break;
	case HP_REQ_BODY:
		/* Ignore body */
		TRACE_L3("ignoring http request body\n");
		http_advance(q, 1);
		break;
	}

	http->cursor++;
}

static void metrics_execute_query(struct query_state *q)
{
	struct http *http = &q->http;
	const char *target = http->target;

	/* peek: true if stats should not be consumed from the circular
	 * buffer when delivered. */
	bool peek = false;

	/* Currently we do writes synchronously */
	fcntl(q->fd, F_SETFL, q->fd_flags & ~O_NONBLOCK);

	TRACE_L4("metrics: got HTTP query: %s %s %s/%" PRId64 ".%" PRId64 "\n",
	     http->method_s, http->target,
	     http->protocol,
	     http->major_version, http->minor_version);

	http_init_reply(http);
	http_add_header(http, "Server", "%s/%s", SFPTPD_MODEL, SFPTPD_VERSION_TEXT);

	if (http->method == HTTP_METHOD_GET ||
	    http->method == HTTP_METHOD_HEAD) {
		char *s;
		if (*target &&
		    (s = strchr(target + 1, '/')) &&
		    !strncmp(target, "/peek", strlen("/peek"))) {
			peek = true;
			target = s;
		}

		if (!strcmp(target, "/metrics"))
			sfptpd_metrics_send(q, peek);
		else if (!strcmp(target, "/rt-stats.jsonl"))
			sfptpd_rt_stats_send(q, peek, JSON_LINES);
		else if (!strcmp(target, "/rt-stats.json-seq"))
			sfptpd_rt_stats_send(q, peek, JSON_SEQ);
		else if (!strcmp(target, "/rt-stats.txt"))
			sfptpd_rt_stats_send(q, peek, STATS_LOG);
		else
			http->response_code = 404;
	}

	http_response(q);

	/* Restore asynchronous mode */
	fcntl(q->fd, F_SETFL, q->fd_flags);
}

static void resolve_http_method(struct http *h)
{
	if (!strcmp(h->method_s, "GET"))
		h->method = HTTP_METHOD_GET;
	else if (!strcmp(h->method_s, "HEAD"))
		h->method = HTTP_METHOD_HEAD;
	else if (!strcmp(h->method_s, "CONNECT"))
		h->method = HTTP_METHOD_CONNECT;
	else
		h->method = HTTP_METHOD_OTHER;
}

static void metrics_process_query(struct sfptpd_thread_readyfd *event,
				  int qi)
{
	struct query_state *q = &metrics.query[qi];
	struct net_buf *nb = &q->rx;
	struct iovec iov[2];
	ssize_t res;
	ssize_t wr_ptr;

	assert(q->fd == event->fd);

	/* Keep expanding the buffer size, when it is over half full,
	 * until we hit a level we've decided in advance will be enough
	 * to handle a line atomically. Note this is not currently
	 * required if the initial buffer size exceeds the size of the
	 * largest element to be extracted. */
	if (nb->len > (nb->capacity >> 1) &&
	    nb->capacity < net_buf_max_capacity) {
		char *next;
		size_t next_cap = nb->capacity << 1;

		next = realloc(nb->data, next_cap);
		if (next == NULL) {
			CRITICAL("out of memory expanding receive buffer\n");
		} else {
			/* If the unread data wraps then move the wrapped part
			 * into the newly expanded half of the buffer. */
			if (nb->rd_ptr + nb->len > nb->capacity) {
				memmove(next + nb->capacity,
					next,
					nb->len - nb->capacity + nb->rd_ptr);
			}
			nb->data = next;
			nb->capacity = next_cap;
		}
	}

	if (nb->len == 0)
		nb->rd_ptr = 0;

	/* Read new data into circular buffer */
	wr_ptr = nb->rd_ptr + nb->len;
	if (wr_ptr >= nb->capacity)
		wr_ptr -= nb->capacity;

	iov[0].iov_base = nb->data + wr_ptr;
	iov[1].iov_base = nb->data;

	if (wr_ptr > nb->rd_ptr || nb->len == 0) {
		iov[0].iov_len = nb->capacity - wr_ptr;
		iov[1].iov_len = nb->rd_ptr;
	} else {
		iov[0].iov_len = nb->rd_ptr - wr_ptr;
		iov[1].iov_len = 0;
	}

	if (iov[0].iov_len == 0)
		WARNING("netbuf: no capacity in rx buffer. TODO: need to removed from poll set as flow control\n");

	res = readv(q->fd, iov, iov[1].iov_len == 0 ? 1 : 2);
	if (res == -1) {
		int rc = errno;
		ERROR("failed to read from metrics request connection, %s\n",
		      strerror(rc));
		if (rc == EIO || rc == ENOTCONN || rc == ENOTSOCK || rc == ECONNRESET || rc == EBADF)
			q->abort = true;
	} else {
		nb->len += res;
	}

	/* Scan for a line to process */
	do {
		if (res > 0 && !q->abort)
			handle_query_data(q);
		if (q->http.action == HP_REQ_ACT_ON_HEADER) {
			if (!strcasecmp(q->http.field_name, "Content-Length"))
				q->http.headers |= HEADER_CONTENT_LENGTH;
			else if (!strcasecmp(q->http.field_name, "Transfer-Encoding"))
				q->http.headers |= HEADER_TRANSFER_ENCODING;
			else
				TRACE_L4("metrics: ignoring HTTP header %s: %s\n", q->http.field_name, q->http.field_value);
		} else if (q->http.action == HP_REQ_ACT_ON_BODY) {
			if (q->http.headers & HEADER_CONTENT_LENGTH ||
			    q->http.headers & HEADER_TRANSFER_ENCODING) {
				http_abort(q, "don't know how to handle requests with a body");
			} else {
				ssize_t saved_cursor = q->http.cursor;

				resolve_http_method(&q->http);
				metrics_execute_query(q);

				/* Reset HTTP state for next query */
				memset(&q->http, '\0', sizeof q->http);
				q->http.cursor = saved_cursor;
			}
		}
		q->http.action = HP_REQ_NO_ACTION;
	} while (q->http.cursor < nb->len);

	if (res == 0)
		TRACE_L4("metrics: EOF received on connection\n");

	if (q->abort || res == 0) {
		TRACE_L5("metrics: closing the connection\n");
		sfptpd_thread_user_fd_remove(q->fd);
		if (queries_busy())
			listeners_xon();
		close(q->fd);
		metrics.active_queries &= ~(1 << qi);
	}
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

void sfptpd_metrics_destroy(void)
{
	if (metrics.initialised) {
		int qi;

		for (qi = 0; qi < MAX_QUERIES; qi++)
			netbuf_free(&metrics.query[qi].rx);
		metrics.active_queries = 0;
	}

	if (metrics.exemplars)
		free(metrics.exemplars);

	if (metrics.rt_stats.entries)
		free(metrics.rt_stats.entries);

	metrics.initialised = false;
	metrics.rt_stats.entries = NULL;
	metrics.exemplars = NULL;
	metrics.exemplars_len = 0;
}

int sfptpd_metrics_init(void)
{
	memset(&metrics, '\0', sizeof metrics);
	metrics.initialised = true;

	return 0;
}

void sfptpd_metrics_service_fds(unsigned int num_fds,
				struct sfptpd_thread_readyfd events[])
{
	struct sfptpd_thread_readyfd *ev;
	struct listener *l;
	unsigned queries;
	int qi;

	for (l = metrics.listeners; !queries_busy() && l; l = l->next) {
		if ((ev = get_event_for(num_fds, events, l->fd))) {
			int fd = accept4(ev->fd, NULL, 0, SOCK_NONBLOCK);
			if (fd == -1) {
				ERROR("metrics: accept() failed: %s\n", strerror(errno));
			} else {
				metrics_handle_connection(fd);
			}
		}
	}

	queries = metrics.active_queries;
	for (qi = __builtin_ctz(queries);
	     __builtin_popcount(queries);
	     qi = __builtin_ctz(queries &= ~ (1 << qi))) {
		if ((ev = get_event_for(num_fds, events, metrics.query[qi].fd))) {
			metrics_process_query(ev, qi);
			break;
		}
	}
}

void sfptpd_metrics_push_rt_stats(struct sfptpd_sync_instance_rt_stats_entry *entry)
{
	struct rt_stats_buf *stats = &metrics.rt_stats;

	if (!metrics.initialised || !metrics.listeners)
		return;

	stats->entries[stats->wr_ptr] = *entry;

	/* Pointer wraps */
	if (++stats->wr_ptr == stats->sz)
		stats->wr_ptr = 0;

	/* But length saturates */
	if (stats->len < stats->sz)
		stats->len++;
	else
		stats->lost_samples++;
}

static void close_listeners(void)
{
	struct listener *l;
	struct listener **hd = &metrics.listeners;

	while ((l = *hd)) {
		*hd = l->next;
		sfptpd_thread_user_fd_remove(l->fd);
		close(l->fd);
		free(l);
	}
	metrics.listeners = NULL;
}

void sfptpd_metrics_listener_close(void)
{
	unsigned queries;
	int qi;

	if (metrics.initialised) {
		close_listeners();
		queries = metrics.active_queries;
		for (qi = __builtin_ctz(queries);
		     __builtin_popcount(queries);
		     qi = __builtin_ctz(queries &= ~ (1 << qi))) {
			sfptpd_thread_user_fd_remove(metrics.query[qi].fd);
			close(metrics.query[qi].fd);
			metrics.active_queries &= ~(1 << qi);
		}
	}
}

static int activate_listener(int fd)
{
	struct listener *l = NULL;
	int flags;
	int rc;

	flags = fcntl(fd, F_GETFL);
	rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (rc != 0) {
		ERROR("metrics: listener: fcntl: %s\n", strerror(errno));
		goto fail;
	}
	rc = listen(fd, MAX_QUERIES);
	if (rc != 0) {
		ERROR("metrics: listener: listen: %s\n", strerror(errno));
		goto fail;
	}
	l = calloc(1, sizeof *l);
	if (l == NULL) {
		ERROR("metrics: listener: calloc: %s\n", strerror(errno));
		goto fail;
	}
	rc = sfptpd_thread_user_fd_add(fd, true, false);
	if (rc != 0) {
		ERROR("metrics: listener: thread_user_fd_add: %s\n", strerror(rc));
		errno = rc;
		goto fail;
	}

	/* Add listener to list */
	l->fd = fd;
	l->next = metrics.listeners;
	metrics.listeners = l;
	return 0;

fail:
	/* Only clean up latest listener - the caller will close
	 * the ones added to the list already. */
	free(l);
	close(fd);
	return errno;
}

static int listen_tcp(struct sfptpd_config_general *general_config)
{
	struct sfptpd_config_metrics *mconf = &general_config->openmetrics;
	int fd;
	int rc;
	int i;

	for (i = 0; i < mconf->num_tcp_addrs; i++) {
		struct sockaddr_storage *ss = mconf->tcp + i;

		fd = socket(ss->ss_family,
			    SOCK_STREAM, 0);
		if (fd == -1)
			goto fail;

		rc = bind(fd, (struct sockaddr *) ss, sizeof *ss);
		if (rc == -1) {
			ERROR("metrics: listener: bind: %s\n", strerror(errno));
			goto fail;
		}

		rc = activate_listener(fd);
		if (rc != 0)
			return rc;
	}
	return 0;
fail:
	/* Only clean up latest listener - the caller will close
	 * the ones added to the list already. */
	close(fd);
	return errno;
}

static int listen_unix(struct sfptpd_config_general *general_config)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX
	};
	char *metrics_path;
	size_t sz;
	int fd = -1;
	int rc;

	/* Size-up path */
	sz = sfptpd_format(sfptpd_log_get_format_specifiers(), NULL,
			   NULL, 0,
			   general_config->metrics_path);
	if (sz < 0)
		return errno;

	metrics_path = malloc(++sz);
	if (metrics_path == NULL)
		return errno;

	/* Format path */
	rc = sfptpd_format(sfptpd_log_get_format_specifiers(), NULL,
			   metrics_path, sz,
			   general_config->metrics_path);
	if (rc < 0)
		goto fail;

	if (strlen(metrics_path) >= sizeof addr.sun_path) {
		errno = ENAMETOOLONG;
		rc = -1;
		goto fail;
	}

	sfptpd_strncpy(addr.sun_path, metrics_path, sizeof addr.sun_path);

	/* Remove any existing socket, ignoring errors */
	unlink(metrics_path);

	/* Create a Unix domain socket for receiving metrics requests */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		ERROR(PREFIX "couldn't create socket\n");
            rc = -1;
	        goto fail;
	}

	/* Set access mode. Be louder because this is explicit config. */
	if (general_config->metrics_socket_mode != (mode_t) -1 &&
	    fchmod(fd, general_config->metrics_socket_mode) == -1)
		WARNING(PREFIX "could not set socket mode, %s\n",
			strerror(errno));

	/* Bind to the path in the filesystem. */
	rc = bind(fd, (const struct sockaddr *) &addr, sizeof addr);
	if (rc == -1) {
		ERROR(PREFIX "couldn't bind socket to %s, %s\n",
		      metrics_path, strerror(errno));
	        goto fail;
	}

	return activate_listener(fd);
fail:
	/* Up until now, errno-style errors have been collecting in 'errno'
	 * itself. Capture in 'rc'. */
	if (rc != 0)
		rc = errno;

	/* Tidy up any claimed resources. */
	free(metrics_path);
	if (rc != 0 && fd != -1) {
		close(fd);
		fd = -1;
	}

	return rc;
}

int sfptpd_metrics_listener_open(struct sfptpd_config *app_config)
{
	struct sfptpd_config_general *general_config;
	int rc;

	general_config = sfptpd_general_config_get(app_config);
	metrics.config = &general_config->openmetrics;

	rc = write_exemplars();
	if (rc != 0)
		return errno;

	if (metrics.rt_stats.entries == NULL) {
		metrics.rt_stats.sz = metrics.config->rt_stats_buf;
		metrics.rt_stats.entries = malloc(metrics.rt_stats.sz * sizeof *metrics.rt_stats.entries);
	}

	rc = listen_unix(general_config);
	if (rc == 0)
		rc = listen_tcp(general_config);
	if (rc != 0)
		close_listeners();
	return rc;
}
