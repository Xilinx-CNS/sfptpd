/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2023 Xilinx, Inc. */

/**
 * @file   sfptpd_logging.c
 * @brief  Message and statistics handling
 */

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
#include <glob.h>

#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_clock.h"
#include "sfptpd_constants.h"
#include "sfptpd_statistics.h"


/****************************************************************************
 * Structures, Types
 ****************************************************************************/

struct sfptpd_log {
	const char *type;
	FILE *stream;
	char final_path[PATH_MAX];
	char temp_path[PATH_MAX];
};


/****************************************************************************
 * Defines & Constants
 ****************************************************************************/

#define APPROX_RT_STATS_LENGTH 512
#define APPROX_RT_SERVOS 2
#define APPROX_RT_UPDATES 16


/* Message logging uses the linux kernel priority level. Define strings for
 * each level */
const char *sfptpd_log_priority_text[] = {
	"emergency", "alert", "critical", "error", "warning", "notice", "info", "debug"
};
const char *sfptpd_state_file_format = "state-%s";
const char *sfptpd_statistics_file_format = "stats-%s";
const char *sfptpd_statistics_json_file_format = "stats-%s.json";
const char *sfptpd_freq_correction_file_format = "freq-correction-%s";
const char *sfptpd_topology_file = "topology";
const char *sfptpd_interfaces_file = "interfaces";
const char *sfptpd_nodes_file = "ptp-nodes";
const char *sfptpd_remote_monitor_file = "remote-monitor";
const char *sfptpd_config_log_file = "config";
const char *sfptpd_sync_instances_file = "sync-instances";

enum path_format_id {
	PATH_FMT_HOSTNAME,
	PATH_FMT_HOSTID,
	PATH_FMT_PID,
	PATH_FMT_CTIME_LOCAL,
};

static size_t path_interpolate(char *buffer, size_t space, int id, void *context, char opt);
static size_t path_interpolate_time(char *buffer, size_t space, int id, void *context, char opt);

/* %H   hostname
 * %I   hostid
 * %P   pid
 * %Cd  creation date, local time (ISO 8601)
 * %Ct  creation date and local time (ISO 8601)
 */
const static struct sfptpd_interpolation path_format_specifiers[] = {
	{ PATH_FMT_HOSTNAME,		'H', false, path_interpolate },
	{ PATH_FMT_HOSTID,		'I', false, path_interpolate },
	{ PATH_FMT_PID,			'P', false, path_interpolate },
	{ PATH_FMT_CTIME_LOCAL,		'C', true,  path_interpolate_time },
	{ SFPTPD_INTERPOLATORS_END }
};


/****************************************************************************
 * Local Variables

 ****************************************************************************/

static enum sfptpd_msg_log_config message_log = SFPTPD_MSG_LOG_TO_STDERR;
static enum sfptpd_stats_log_config stats_log = SFPTPD_STATS_LOG_OFF;
static int message_log_fd = -1;
static int stats_log_fd = -1;
static FILE *json_remote_monitor_fp = NULL;
static pthread_mutex_t vmsg_mutex;
static char freq_correction_file_format[PATH_MAX];
static char state_file_format[PATH_MAX];
static char state_next_file_format[PATH_MAX];
static char sfptpd_config_log_tmpfile[] = "/tmp/sfptpd.conf.lexed.XXXXXX";
static FILE *config_log_tmp = NULL;

/* JSON stats is block-buffered and we ensure lines get written whole. */
static FILE *json_stats_fp = NULL;
static char *json_stats_buf = NULL;
const static size_t json_stats_bufsz = APPROX_RT_STATS_LENGTH * APPROX_RT_SERVOS * APPROX_RT_UPDATES;
static int json_stats_ptr = 0;

static unsigned int trace_levels[SFPTPD_COMPONENT_ID_MAX] =
{
	SFPTPD_DEFAULT_TRACE_LEVEL, 0
};


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static size_t path_interpolate(char *buffer, size_t space, int id, void *context, char opt)
{
	assert(buffer != NULL || space == 0);

	char hostname[HOST_NAME_MAX];
	int ret;

	switch (id) {
	case PATH_FMT_HOSTNAME:
		ret = gethostname(hostname, sizeof hostname);
		if (ret != 0)
			return -1;
		hostname[sizeof hostname - 1] = '\0';
		return snprintf(buffer, space, "%s", hostname);
	case PATH_FMT_HOSTID:
		return snprintf(buffer, space, "%lx", gethostid());
	case PATH_FMT_PID:
		return snprintf(buffer, space, "%lu", (unsigned long) getpid());
	default:
		return 0;
	}
}

static size_t path_interpolate_time(char *buffer, size_t space, int id, void *context, char opt)
{
	assert(buffer != NULL || space == 0);

	const char *fmt = "";
	char tmp[128];
	struct tm *t_local;
	time_t t_sys;

	assert(id == PATH_FMT_CTIME_LOCAL);

	t_sys = time(NULL);
	t_local = localtime(&t_sys);
	if (t_local == NULL)
		return -1;
	if (opt == 'd')
		fmt = "%F";
	else if (opt == 't')
		fmt = "%F %T";

	if (space == 0) {
		buffer = tmp;
		space = sizeof tmp;
	}
	strftime(buffer, space, fmt, t_local);
	return strnlen(buffer, space);
}

static int construct_log_paths(struct sfptpd_log *log,
			       const char *filename_pattern,
			       va_list ap)
{
	va_list aq;
	size_t space;
	int ptr;

	assert(log != NULL);
	assert(filename_pattern != NULL);

	va_copy(aq, ap);

	/* Write full path */
	space = sizeof log->final_path;
	ptr = snprintf(log->final_path, space, "%s", state_file_format);
	if (ptr >= space)
		return ENOMEM;
	space -= ptr;
	ptr = vsnprintf(log->final_path + ptr, space, filename_pattern, ap);
	if (ptr >= space)
		return ENOMEM;

	/* Write temporary path */
	space = sizeof log->temp_path;
	ptr = snprintf(log->temp_path, space, "%s", state_next_file_format);
	if (ptr >= space)
		return ENOMEM;
	space -= ptr;
	ptr = vsnprintf(log->temp_path + ptr, space, filename_pattern, aq);
	if (ptr >= space)
		return ENOMEM;

	return 0;
}


static struct sfptpd_log *create_log(const char *type,
				     const char *filename_pattern, ...)
{
	va_list ap;
	struct sfptpd_log *log;
	int rc;

	assert(filename_pattern != NULL);

	log = (struct sfptpd_log *) calloc(1, sizeof *log);
	if (log == NULL) {
		ERROR("failed to allocate %s log file, %s\n",
		      type, strerror(errno));
		return NULL;
	}

	log->type = type;

	va_start(ap, filename_pattern);
	rc = construct_log_paths(log, filename_pattern, ap);
	va_end(ap);
	if (rc != 0) {
		ERROR("failed to construct %s log filename, %s\n",
		      type, strerror(rc));
		free(log);
		return NULL;
	}

	log->stream = fopen(log->temp_path, "w");
	if (log->stream == NULL) {
		ERROR("failed to open %s log file \"%s\", %s\n",
		      type, log->temp_path, strerror(errno));
		free(log);
		return NULL;
	}

	return log;
}


#ifndef SFPTPD_BUILDTIME_CHECKS
static void sfptpd_log_vmessage(int priority, const char * format, va_list ap)
{
	assert(priority >= 0);
	assert(format != NULL);

	/* Syslog only has 8 message levels (3 bits) so saturate at level DEBUG.
	 * Note that messages will only appear if "*.debug /var/log/debug" is
	 * set in /etc/rsyslog.conf */
	if(priority > LOG_DEBUG)
		priority = LOG_DEBUG;
	
	if (message_log == SFPTPD_MSG_LOG_TO_SYSLOG) {
		vsyslog(priority, format, ap);
	} else {
		struct sfptpd_log_time time;
		sfptpd_log_get_time(&time);

		pthread_mutex_lock(&vmsg_mutex);
		fprintf(stderr, "%s: %s: ", time.time,
			sfptpd_log_priority_text[priority]);
		vfprintf(stderr, format, ap);
		pthread_mutex_unlock(&vmsg_mutex);
	}
}
#endif /* SFPTPD_BUILDTIME_CHECKS */


static void log_topology_write_entry(FILE *stream, const char *field,
				     char pre, char post, bool new_line)
{
	int before, after, len;

	assert(stream != NULL);
	assert(field != NULL);

	len = strlen(field);
	before = (SFPTPD_TOPOLOGY_FIELD_WIDTH - len)/2;
	after = SFPTPD_TOPOLOGY_FIELD_WIDTH - len - before;

	for ( ; before > 0; before--)
		fputc(pre, stream);
	fputs(field, stream);
	for ( ; after > 0; after--)
		fputc(post, stream);
	if (new_line)
		fputc('\n', stream);
}


static void log_copy_file(const char *src, const char *dest)
{
	FILE *load;
	FILE *save;
	char buf[128];
	char *str;
	bool error = true;

	assert(dest);

	if ((save = fopen(dest, "w"))) {
		if ((load = fopen(src, "r"))) {
			while ((str = fgets(buf, sizeof buf, load)) && fputs(str, save) >= 0);
			error = ferror(load) || ferror(save);
			fclose(load);
		}
		fclose(save);
	}

	if (error)
		ERROR("could not save a copy of the configuration, %s\n",
		      strerror(errno));
}

static char *format_path(const char *pattern)
{
	char *path;
	int len;
	int len2;

	len = sfptpd_format(path_format_specifiers, NULL, NULL, 0, pattern);
	if (len < 0) {
		ERROR("logging: error formatting path: %s\n",
		      pattern, strerror(errno));
		return NULL;
	}

	path = (char *) malloc(len + 1);
	if (path == NULL) {
		ERROR("logging: error allocating path of %d\n",
		      len, strerror(errno));
		return NULL;
	}

	len2 = sfptpd_format(path_format_specifiers, NULL, path, len + 1, pattern);
	if (len2 > len) {
		ERROR("logging: truncated formatted path that expanded after sizing (%d > %d)\n",
		      len2, len);
		errno = ENAMETOOLONG;
		return NULL;
	}

	return path;
}

static void free_path(char *path)
{
	free(path);
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_log_open(struct sfptpd_config *config)
{
	struct sfptpd_config_general *general_config = sfptpd_general_config_get(config);
	char *state_path = format_path(general_config->state_path);
	glob_t glob_results;
	char path[PATH_MAX];
	FILE *file;
	int rc_dircreate;
	int rc;
	int i;

	/* Patterns for state files to be deleted */
	const char *to_delete[] = { "state-*",
				    "stats-*",
				    "topology",
				    "interfaces",
				    "ptp-nodes",
				    "remote-monitor",
				    "sync-instances",
				    ".next.*"
	};

	if (state_path == NULL)
		return errno;

	/* Take copies of the message and stats logging targets and the trace level */
	message_log = general_config->message_log;
	stats_log = general_config->stats_log;
	trace_levels[SFPTPD_COMPONENT_ID_SFPTPD] = general_config->trace_level;
	trace_levels[SFPTPD_COMPONENT_ID_THREADING] = general_config->threading_trace_level;
	trace_levels[SFPTPD_COMPONENT_ID_BIC] = general_config->bic_trace_level;
	trace_levels[SFPTPD_COMPONENT_ID_NETLINK] = general_config->netlink_trace_level;
	trace_levels[SFPTPD_COMPONENT_ID_NTP] = general_config->ntp_trace_level;
	trace_levels[SFPTPD_COMPONENT_ID_SERVO] = general_config->servo_trace_level;
	trace_levels[SFPTPD_COMPONENT_ID_CLOCKS] = general_config->clocks_trace_level;

	/* Ratchet up some component trace levels based on the general level
	 * where appropriate. */
	if (trace_levels[SFPTPD_COMPONENT_ID_NETLINK] < 1 &&
	    trace_levels[SFPTPD_COMPONENT_ID_SFPTPD] >= 1) {
		trace_levels[SFPTPD_COMPONENT_ID_NETLINK] = 1;
	}

	/* Make sure that the directory for saved clock state exists */
	rc_dircreate = (mkdir(state_path, 0777) < 0) ? errno : 0;
	if (chown(state_path, general_config->uid, general_config->gid))
		TRACE_L4("could not set state directory ownership, %s\n",
			 strerror(errno));

	/* If messages are being logged to the syslog, open it */
	if (message_log == SFPTPD_MSG_LOG_TO_SYSLOG)
		openlog("sfptpd", 0, LOG_DAEMON);

	/* Call log rotate to open log files if logging to file. */
	rc = sfptpd_log_rotate(config);
	if (rc != 0) {
		goto fail;
	}

	/* Send the warning for failed directory creation to the log */
	if (rc_dircreate != 0 && rc_dircreate != EEXIST)
		WARNING("couldn't create directory for saved state %s, error %s\n",
			state_path, strerror(errno));

	/* Save the lexed config */
	if (config_log_tmp) {
		fclose(config_log_tmp);
		config_log_tmp = NULL;
		rc = snprintf(path, sizeof path, "%s/%s", state_path,
			      sfptpd_config_log_file);
		assert(rc < sizeof path);
		log_copy_file(sfptpd_config_log_tmpfile, path);
		if (chown(path, general_config->uid, general_config->gid))
			TRACE_L4("could not set config copy ownership, %s\n", strerror(errno));
		unlink(sfptpd_config_log_tmpfile);
	}

	/* Delete all state and stats files, interfaces and topology file before we begin */
	for (i = 0; i < sizeof(to_delete) / sizeof(*to_delete); i++) {
		rc = snprintf(path, sizeof path, "%s/%s", state_path, to_delete[i]);
		assert(rc < sizeof path);
		glob(path, (i == 0) ? 0 : GLOB_APPEND, NULL, &glob_results);
	}
	for (i = 0; i < glob_results.gl_pathc; i++) {
		unlink(glob_results.gl_pathv[i]);
	}
	globfree(&glob_results);

	/* Write the version number to the state path */
	rc = snprintf(path, sizeof path, "%s/version", state_path);
	if (rc >= sizeof path) return ENOMEM;
	file = fopen(path, "w");
	if (file == NULL) {
		ERROR("couldn't open %s/version\n", state_path);
		rc = errno;
		goto fail;
	}
	if (chown(path, general_config->uid, general_config->gid))
		TRACE_L4("could not set version file ownership, %s\n",
			 strerror(errno));

	fprintf(file, "%s\n", SFPTPD_VERSION_TEXT);
	fclose(file);

	/* Store state file path formats */
	rc = snprintf(freq_correction_file_format,
		      sizeof freq_correction_file_format,
		      "%s/%s", state_path, sfptpd_freq_correction_file_format);
	if (rc >= sizeof path) return ENOMEM;

	rc = snprintf(state_file_format,
		      sizeof state_file_format,
		      "%s/", state_path);
	if (rc >= sizeof path) return ENOMEM;

	rc = snprintf(state_next_file_format,
		      sizeof state_next_file_format,
		      "%s/.next.", state_path);
	if (rc >= sizeof path) return ENOMEM;
	rc = 0;

	pthread_mutex_init(&vmsg_mutex, NULL);

fail:
	if (rc != 0) {
		sfptpd_log_close();
	}

	free_path(state_path);

	return rc;
}

int sfptpd_log_rotate(struct sfptpd_config *config)
{
	struct sfptpd_config_general *general_config;
	char *path;
	int rc = 0;
	bool shared_file;

	general_config = sfptpd_general_config_get(config);

	shared_file = (message_log == SFPTPD_MSG_LOG_TO_FILE &&
		       stats_log == SFPTPD_STATS_LOG_TO_FILE &&
		       0 == strcmp(general_config->message_log_filename,
				   general_config->stats_log_filename));

	fflush(NULL);

	if (message_log == SFPTPD_MSG_LOG_TO_FILE) {
		/* Close and then reopen the log file */
		if(message_log_fd != -1)
			close(message_log_fd);

		path = format_path(general_config->message_log_filename);
		if (path == NULL) {
			rc = errno;
		} else {
			message_log_fd = open(path,
					      O_CREAT | O_APPEND | O_RDWR, 0644);
			if (message_log_fd < 0) {
				ERROR("Failed to open message%s log file %s, error %s\n",
				      shared_file ? "/stats" : "",
				      path, strerror(errno));
				rc = errno;
			} else {
				if (chown(path, general_config->uid, general_config->gid))
					TRACE_L4("could not set message%s log ownership, %s\n",
						 shared_file ? "/stats" : "",
						 strerror(errno));

				/* Redirect stderr to the log file */
				dup2(message_log_fd, STDERR_FILENO);
			}
			free_path(path);
		}
	}

	if (stats_log == SFPTPD_STATS_LOG_TO_FILE) {
		if (shared_file) {
			stats_log_fd = message_log_fd;
			dup2(stats_log_fd, STDOUT_FILENO);
		} else {
			/* Close and then reopen the log file */
			if(stats_log_fd != -1)
				close(stats_log_fd);

			path = format_path(general_config->stats_log_filename);
			if (path == NULL) {
				rc = errno;
			} else {
				stats_log_fd = open(path,
						    O_CREAT | O_APPEND | O_RDWR, 0644);

				if (stats_log_fd < 0) {
					ERROR("Failed to open stats log file %s, error %s\n",
					      path, strerror(errno));
					rc = errno;
				} else {
					if (chown(path, general_config->uid, general_config->gid))
						TRACE_L4("could not set stats log ownership, %s\n", strerror(errno));

					/* Redirect stdout to the log file */
					dup2(stats_log_fd, STDOUT_FILENO);
				}
				free_path(path);
			}
		}
	}

	if (strlen(general_config->json_stats_filename) > 0) {
		char *path = format_path(general_config->json_stats_filename);

		/* Close and then reopen the log file */
		if(json_stats_fp != NULL)
			fclose(json_stats_fp);
		else
			json_stats_buf = malloc(json_stats_bufsz);

		json_stats_fp = path ? fopen(path, "a") : NULL;
		if (json_stats_fp == NULL) {
			ERROR("Failed to open json stats file %s, error %s\n",
			      path ? path : general_config->json_stats_filename, strerror(errno));
			if (json_stats_buf)
				free(json_stats_buf);
			/* We don't set rc = errno because this log is non-critical. */
		} else {
			setvbuf(json_stats_fp, json_stats_buf, _IOFBF, json_stats_bufsz);
			json_stats_ptr = 0;
		}
		free_path(path);
	}

	if (strlen(general_config->json_remote_monitor_filename) > 0) {
		/* Close and then reopen the log file */
		if(json_remote_monitor_fp != NULL)
			fclose(json_remote_monitor_fp);

		json_remote_monitor_fp = fopen(general_config->json_remote_monitor_filename, "a");
		if (json_remote_monitor_fp == NULL) {
			ERROR("Failed to open json remote monitor file %s, error %s\n",
				  general_config->json_remote_monitor_filename, strerror(errno));
			/* We don't set rc = errno because this log is non-critical. */
		}
	}
	return rc;
}


bool sfptpd_log_isatty(void)
{
	return ((stats_log == SFPTPD_STATS_LOG_TO_STDOUT) &&
	        (isatty(STDOUT_FILENO) != 0));
}


void sfptpd_log_close(void)
{
	if (message_log == SFPTPD_MSG_LOG_TO_SYSLOG)
		closelog();
	
	if (message_log_fd != -1 &&
	    message_log_fd != stats_log_fd)
		close(message_log_fd);

	if (stats_log_fd != -1)
		close(stats_log_fd);

	if (json_stats_fp != NULL) {
		fclose(json_stats_fp);
		json_stats_fp = NULL;
		free(json_stats_buf);
	}

	if (json_remote_monitor_fp != NULL) {
		fclose(json_remote_monitor_fp);
		json_remote_monitor_fp = NULL;
	}

	pthread_mutex_destroy(&vmsg_mutex);
}


FILE *sfptpd_log_file_get_stream(struct sfptpd_log *log)
{
	assert(log != NULL);

	return log->stream;
}


int sfptpd_log_file_close(struct sfptpd_log *log)
{
	int rc;

	assert(log != NULL);
	assert(log->stream != NULL);

	/* Close the stream */
	fclose(log->stream);
	log->stream = NULL;

	/* Replace the old log file with the newly-constructed one */
	rc = rename(log->temp_path, log->final_path);
	if (rc != 0) {
		ERROR("failed to install %s log file \"%s\", %s\n",
		      log->type, log->final_path, strerror(errno));
	}

	/* Free the log object */
	free(log);

	return rc;
}


void sfptpd_log_set_trace_level(sfptpd_component_id_e component, int level)
{
	assert(component < SFPTPD_COMPONENT_ID_MAX);
	trace_levels[component] = level;
}


#ifndef SFPTPD_BUILDTIME_CHECKS
void sfptpd_log_message(int priority, const char * format, ...)
{
	va_list ap;
	assert(format != NULL);
	va_start(ap, format);
	sfptpd_log_vmessage(priority, format, ap);
	va_end(ap);
}


void sfptpd_log_trace(sfptpd_component_id_e component, unsigned int level,
		      const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	assert(component < SFPTPD_COMPONENT_ID_MAX);
	assert(format != NULL);

	/* Permit trace level 0, using it for explicit user requests for
	 * diagnostics at runtime. */

	/* For trace, we suppress the output if above the current trace level. */
	if (level > trace_levels[component])
		return;

	sfptpd_log_vmessage(level + LOG_INFO, format, ap);

	va_end(ap);
}


void sfptpd_log_stats(const char *format, ...)
{
	va_list ap;

	assert(format != NULL);

	if (stats_log != SFPTPD_STATS_LOG_OFF) {
		va_start(ap, format);
		vprintf(format, ap);
		va_end(ap);
	}
}


FILE *sfptpd_log_get_rt_stats_out_stream(void)
{
	return json_stats_fp;
}


bool sfptpd_log_rt_stats_written(size_t chars, bool flush)
{
	size_t headroom;
	bool flushed = false;

	json_stats_ptr += chars;
	headroom = json_stats_bufsz - json_stats_ptr;

	if (flush ||
	    headroom < APPROX_RT_STATS_LENGTH ||
	    headroom < chars * 2) {
		fflush(json_stats_fp);
		json_stats_ptr = 0;
		flushed = true;
	}

	return flushed;
}


FILE *sfptpd_log_get_remote_monitor_out_stream(void)
{
	return json_remote_monitor_fp;
}


void sfptpd_log_write_state(struct sfptpd_clock *clock,
			    const char *sync_instance_name,
			    const char *format, ...)
{
	va_list ap;
	struct sfptpd_log *log;
	const char *name;

	assert(clock != NULL);
	assert(format != NULL);

	if (sync_instance_name != NULL)
		name = sync_instance_name;
	else
		name = sfptpd_clock_get_fname_string(clock);

	/* Create the path of the state file along the lines of either with
	 * either the clock or the sync module instance name.
	 *      /var/lib/sfptpd/state-system or
	 *      /var/lib/sfptpd/state-1122:3344:5566:7788 or
	 *      /var/lib/sfptpd/state-ptp1
	 */
	log = create_log("state", sfptpd_state_file_format, name);
	if (log != NULL) {
		va_start(ap, format);
		vfprintf(sfptpd_log_file_get_stream(log), format, ap);
		va_end(ap);

		sfptpd_log_file_close(log);
	}
}
#endif /* SFPTPD_BUILDTIME_CHECKS */


int sfptpd_log_write_freq_correction(struct sfptpd_clock *clock, long double freq_adj_ppb)
{
	struct sfptpd_log *log;
	
	assert(clock != NULL);
	
	/* Create the path of the frequency correction file along the lines of either
	 *      /var/lib/sfptpd/freq-correction-system or
	 *      /var/lib/sfptpd/freq-correction-1122:3344:5566:7788 or
	 */
	log = create_log("freq-correction", sfptpd_freq_correction_file_format,
			 sfptpd_clock_get_fname_string(clock));
	if (log != NULL) {
		fprintf(sfptpd_log_file_get_stream(log),
			"%Lf\n", freq_adj_ppb);
		sfptpd_log_file_close(log);
	}

	return log == NULL ? EIO : 0;
}


int sfptpd_log_read_freq_correction(struct sfptpd_clock *clock, long double *freq_adj_ppb)
{
	FILE *file;
	char path[PATH_MAX];
	int tokens;
	
	assert(clock != NULL);
	assert(freq_adj_ppb != NULL);
	
	/* Create the path name of the frequency adjustment file for this clock */
	snprintf(path, sizeof(path), freq_correction_file_format,
		 sfptpd_clock_get_fname_string(clock));
	
	file = fopen(path, "r");
	if (file == NULL) {
		INFO("no clock frequency correction file %s\n", path);
		*freq_adj_ppb = 0.0;
		return ENODATA;
	}

	tokens = fscanf(file, "%Lf", freq_adj_ppb);
	if (tokens != 1) {
		WARNING("clock %s: couldn't load clock correction\n",
			sfptpd_clock_get_long_name(clock));
		*freq_adj_ppb = 0.0;
		fclose(file);
		return ENODATA;
	}
	
	fclose(file);
	
	return 0;
}


void sfptpd_log_delete_freq_correction(struct sfptpd_clock *clock)
{
	char path[PATH_MAX];

	assert(clock != NULL);

	/* Create the path name of the frequency adjustment file for this clock */
	snprintf(path, sizeof(path), freq_correction_file_format,
		 sfptpd_clock_get_fname_string(clock));

	unlink(path);
}


struct sfptpd_log *sfptpd_log_open_topology(void)
{
	return create_log("topology", sfptpd_topology_file);
}


struct sfptpd_log *sfptpd_log_open_interfaces(void)
{
	return create_log("interfaces", sfptpd_interfaces_file);
}


struct sfptpd_log *sfptpd_log_open_ptp_nodes(void)
{
	return create_log("ptp-nodes", sfptpd_nodes_file);
}


struct sfptpd_log *sfptpd_log_open_sync_instances(void)
{
	return create_log("sync-instances", sfptpd_sync_instances_file);
}


#ifndef SFPTPD_BUILDTIME_CHECKS
void sfptpd_log_topology_write_field(FILE *stream, bool new_line,
				     const char *format, ...)
{
	char field[SFPTPD_TOPOLOGY_FIELD_WIDTH];
	va_list args;

	assert(stream != NULL);
	assert(format != NULL);

	va_start(args, format);
	vsnprintf(field, sizeof(field), format, args);
	va_end(args);

	log_topology_write_entry(stream, field, ' ', ' ', new_line);
}
#endif /* SFPTPD_BUILDTIME_CHECKS */


void sfptpd_log_topology_write_1to1_connector(FILE *stream, bool arrow_top,
					      bool arrow_bottom,
					      const char *label, ...)
{
	char l[SFPTPD_TOPOLOGY_FIELD_WIDTH];
	va_list args;
	assert(stream != NULL);

	if (arrow_top)
		log_topology_write_entry(stream, "^", ' ', ' ', true);

	log_topology_write_entry(stream, "|", ' ', ' ', true);
	log_topology_write_entry(stream, "|", ' ', ' ', true);

	if (label != NULL) {
		va_start(args, label);
		vsnprintf(l, sizeof(l), label, args);
		va_end(args);
		log_topology_write_entry(stream, l, ' ', ' ', true);
	}

	log_topology_write_entry(stream, "|", ' ', ' ', true);
	log_topology_write_entry(stream, "|", ' ', ' ', true);

	if (arrow_bottom)
		log_topology_write_entry(stream, "v", ' ', ' ', true);
}


void sfptpd_log_topology_write_1ton_connector_start(FILE *stream, int num_nodes,
						    bool arrow)
{
	int i;
	assert(stream != NULL);
	assert(num_nodes >= 1);

	if (arrow)
		log_topology_write_entry(stream, "^", ' ', ' ', true);

	log_topology_write_entry(stream, "|", ' ', ' ', true);
	log_topology_write_entry(stream, "|", ' ', ' ', true);

	if (num_nodes > 1) {
		log_topology_write_entry(stream, "o", ' ', '-', false);
		for (i = num_nodes - 2; i > 0; i--)
			log_topology_write_entry(stream, "o", '-', '-', false);
		log_topology_write_entry(stream, "o", '-', ' ', true);

		for (i = num_nodes; i > 0; i--)
			log_topology_write_entry(stream, "|", ' ', ' ', false);
		fputc('\n', stream);
		for (i = num_nodes; i > 0; i--)
			log_topology_write_entry(stream, "|", ' ', ' ', false);
		fputc('\n', stream);
	}
}


void sfptpd_log_topology_write_1ton_connector_end(FILE *stream, int num_nodes,
						  bool arrow)
{
	int i;
	assert(stream != NULL);
	assert(num_nodes >= 1);

	for (i = num_nodes; i > 0; i--)
		log_topology_write_entry(stream, "|", ' ', ' ', false);
	fputc('\n', stream);
	for (i = num_nodes; i > 0; i--)
		log_topology_write_entry(stream, "|", ' ', ' ', false);
	fputc('\n', stream);

	if (arrow) {
		for (i = num_nodes; i > 0; i--)
			log_topology_write_entry(stream, "v", ' ', ' ', false);
		fputc('\n', stream);
	}
}


void sfptpd_log_table_row(FILE *stream, bool draw_line, const char *format, ...)
{
	int len, ii;
	va_list ap;
	va_start(ap, format);
	len = vfprintf(stream, format, ap);
	va_end(ap);

	if (draw_line) {
		for(ii = 0; ii < len - 1; ii++)
			fputc('-', stream);
		fputc('\n', stream);
	}
}


struct sfptpd_log *sfptpd_log_open_statistics(struct sfptpd_clock *clock,
					      const char *sync_instance_name)
{
	const char *name;

	assert(clock != NULL);

	if (sync_instance_name != NULL)
		name = sync_instance_name;
	else
		name = sfptpd_clock_get_fname_string(clock);

	/* Create the path of the stats file along the lines of either with
	 * either the clock or the sync module instance name.
	 *      /var/lib/sfptpd/stats-system or
	 *      /var/lib/sfptpd/stats-1122:3344:5566:7788 or
	 *      /var/lib/sfptpd/stats-ptp1
	 */
	return create_log("statistics", sfptpd_statistics_file_format, name);
}


struct sfptpd_log *sfptpd_log_open_statistics_json(struct sfptpd_clock *clock,
					      const char *sync_instance_name)
{
	const char *name;

	assert(clock != NULL);

	if (sync_instance_name != NULL)
		name = sync_instance_name;
	else
		name = sfptpd_clock_get_fname_string(clock);

	/* Create the path of the stats file along the lines of either with
	 * either the clock or the sync module instance name.
	 *      /var/lib/sfptpd/stats-system.json or
	 *      /var/lib/sfptpd/stats-1122:3344:5566:7788.json or
	 *      /var/lib/sfptpd/stats-ptp1.json
	 */
	return create_log("statistics_json",
			  sfptpd_statistics_json_file_format, name);
}


struct sfptpd_log *sfptpd_log_open_remote_monitor(void)
{
	return create_log("remote-monitor", sfptpd_remote_monitor_file);
}


void sfptpd_log_get_time(struct sfptpd_log_time *time)
{
	struct timeval now;
	char temp[SFPTPD_LOG_TIME_STR_MAX];
	int rc;
	
	assert(time != NULL);
	
	gettimeofday(&now, 0);
	sfptpd_local_strftime(temp, sizeof(temp), "%Y-%m-%d %X", &now.tv_sec);

	rc = snprintf(time->time, sizeof(time->time), "%s.%06ld", temp, now.tv_usec);
	assert(rc < sizeof(time->time));
}


void sfptpd_log_lexed_config(const char *format, ...)
{
	va_list ap;

	assert(format != NULL);

	va_start(ap, format);

	if (!config_log_tmp) {
		int log_fd = mkstemp(sfptpd_config_log_tmpfile);
		assert(log_fd != -1);

		config_log_tmp = fdopen(log_fd, "w");
		assert(config_log_tmp);
	}

	vfprintf(config_log_tmp, format, ap);

	va_end(ap);
}

void sfptpd_log_config_abandon(void)
{
	if (config_log_tmp) {
		fclose(config_log_tmp);
		unlink(sfptpd_config_log_tmpfile);
		config_log_tmp = NULL;
	}
}

const struct sfptpd_interpolation *sfptpd_log_get_format_specifiers(void)
{
	return path_format_specifiers;
}

/* fin */
