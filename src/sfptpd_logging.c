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

static const char *rundir_to_interpolate;

enum path_format_id {
	PATH_FMT_HOSTNAME,
	PATH_FMT_HOSTID,
	PATH_FMT_PID,
	PATH_FMT_CTIME_LOCAL,
	PATH_FMT_RUNDIR,
};

static ssize_t path_interpolate(char *buffer, size_t space, int id, void *context, char opt);
static ssize_t path_interpolate_time(char *buffer, size_t space, int id, void *context, char opt);

/* %H   hostname
 * %I   hostid
 * %P   pid
 * %Cd  creation date, local time (ISO 8601)
 * %Ct  creation date and local time (ISO 8601)
 * %R   run directory
 */
const static struct sfptpd_interpolation path_format_specifiers[] = {
	{ PATH_FMT_HOSTNAME,		'H', false, path_interpolate },
	{ PATH_FMT_HOSTID,		'I', false, path_interpolate },
	{ PATH_FMT_PID,			'P', false, path_interpolate },
	{ PATH_FMT_CTIME_LOCAL,		'C', true,  path_interpolate_time },
	{ PATH_FMT_RUNDIR,		'R', false, path_interpolate },
	{ SFPTPD_INTERPOLATORS_END }
};

/* Used when serialising text output */
const char *RT_STATS_KEY_NAMES[] = {
	[STATS_KEY_OFFSET] = "offset",
	[STATS_KEY_FREQ_ADJ] = "freq-adj",
	[STATS_KEY_OWD] = "one-way-delay",
	[STATS_KEY_PARENT_ID] = "parent-id",
	[STATS_KEY_GM_ID] = "gm-id",
	[STATS_KEY_PPS_OFFSET] = "pps-offset",
	[STATS_KEY_BAD_PERIOD] = "pps-bad-periods",
	[STATS_KEY_OVERFLOWS] = "pps-overflows",
	[STATS_KEY_ACTIVE_INTF] = "active-interface",
	[STATS_KEY_BOND_NAME] = "bond-interface",
	[STATS_KEY_P_TERM] = "p-term",
	[STATS_KEY_I_TERM] = "i-term",
	[STATS_KEY_M_TIME] = "m-time",
	[STATS_KEY_S_TIME] = "s-time",
};

static_assert(sizeof(RT_STATS_KEY_NAMES)/sizeof(*RT_STATS_KEY_NAMES) == STATS_KEY_END,
	      "exactly one name defined for each stat");


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
char *config_log_buf = NULL;
size_t config_log_bufsz = 0;
static FILE *config_log_stream = NULL;
static bool config_log_attempted = false;

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

static ssize_t path_interpolate(char *buffer, size_t space, int id, void *context, char opt)
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
	case PATH_FMT_RUNDIR:
		return snprintf(buffer, space, "%s", rundir_to_interpolate);
	default:
		return 0;
	}
}

static ssize_t path_interpolate_time(char *buffer, size_t space, int id, void *context, char opt)
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


static void log_write_file(const char *dest, const char *buf, size_t len)
{
	FILE *save;
	bool error = true;

	if ((save = fopen(dest, "w"))) {
		error = fwrite(buf, len, 1, save) != 1;
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

	rundir_to_interpolate = general_config->run_dir;

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
	rc_dircreate = (mkdir(state_path, general_config->state_dir_mode) < 0) ? errno : 0;
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
	if (config_log_stream) {
		fclose(config_log_stream);
		rc = snprintf(path, sizeof path, "%s/%s", state_path,
			      sfptpd_config_log_file);
		assert(rc < sizeof path);
		log_write_file(path, config_log_buf, config_log_bufsz);
		if (chown(path, general_config->uid, general_config->gid))
			TRACE_L4("could not set config copy ownership, %s\n", strerror(errno));
		free(config_log_buf);
		config_log_buf = NULL;
		config_log_stream = NULL;
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


void sfptpd_log_stats(FILE *stream, const char *format, ...)
{
	va_list ap;

	assert(format != NULL);

	if (stats_log != SFPTPD_STATS_LOG_OFF) {
		va_start(ap, format);
		vfprintf(stream, format, ap);
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
					      const char *entity_name)
{
	const char *name;

	assert(clock || entity_name);

	if (entity_name != NULL)
		name = entity_name;
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
						   const char *entity_name)
{
	const char *name;

	assert(clock || entity_name);

	if (entity_name != NULL)
		name = entity_name;
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
	char temp[SFPTPD_LOG_TIME_STR_MAX];
	struct sfptpd_timespec now;
	int rc;

	assert(time != NULL);

	sfclock_gettime(CLOCK_REALTIME, &now);
	sfptpd_local_strftime(temp, sizeof(temp), "%Y-%m-%d %X", &now.sec);

	rc = snprintf(time->time, sizeof(time->time), "%s.%06" PRId32,
		      temp, now.nsec / 1000);
	assert(rc < sizeof(time->time));
}


void sfptpd_log_format_time(struct sfptpd_log_time *time,
			    const struct sfptpd_timespec *timestamp)
{
	char temp[SFPTPD_LOG_TIME_STR_MAX];
	int rc;

	assert(time != NULL);

	sfptpd_local_strftime(temp, sizeof(temp), "%Y-%m-%d %X", &timestamp->sec);

	rc = snprintf(time->time, sizeof(time->time), "%s.%06" PRId32,
		      temp, timestamp->nsec / 1000);
	assert(rc < sizeof(time->time));
}


void sfptpd_log_lexed_config(const char *format, ...)
{
	const char *error="could not record config copy, %s\n";
	va_list ap;

	assert(format != NULL);

	va_start(ap, format);

	if (!config_log_stream && !config_log_attempted) {
		config_log_stream = open_memstream(&config_log_buf, &config_log_bufsz);
		if (config_log_stream == NULL) {
			WARNING(error, strerror(errno));
		}
		config_log_attempted = true;
	}

	if (config_log_stream)
		vfprintf(config_log_stream, format, ap);

	va_end(ap);
}

void sfptpd_log_config_abandon(void)
{
	if (config_log_stream) {
		fclose(config_log_stream);
		free(config_log_buf);
		config_log_buf = NULL;
		config_log_stream = NULL;
	}
}

const struct sfptpd_interpolation *sfptpd_log_get_format_specifiers(void)
{
	return path_format_specifiers;
}

const char *sfptpd_log_render_log_time(struct sfptpd_log_time_cache *log_time_cache,
				       struct sfptpd_sync_instance_rt_stats_entry *entry)
{
	/* Treat log time within 50us as identical. */
	static const struct sfptpd_timespec equivalent_time = {
		.sec = 0,
		.nsec = 50000,
		.nsec_frac = 0,
	};

	if (!sfptpd_time_equal_within(&entry->log_time,
				      &log_time_cache->log_time,
				      &equivalent_time)) {
		sfptpd_log_format_time(&log_time_cache->log_time_text,
				       &entry->log_time);
		log_time_cache->log_time = entry->log_time;
	}

	return log_time_cache->log_time_text.time;
}

void sfptpd_log_render_rt_stat_text(struct sfptpd_log_time_cache *log_time_cache,
				    FILE *o,
				    struct sfptpd_sync_instance_rt_stats_entry *entry)
{
	char *comma = "";

	assert(entry != NULL);

	sfptpd_log_stats(o, "%s [%s%s%s%s",
			 sfptpd_log_render_log_time(log_time_cache, entry),
			 entry->instance_name ? entry->instance_name : "",
			 entry->instance_name ? ":" : "",
			 entry->clock_master ? sfptpd_clock_get_short_name(entry->clock_master) : entry->source,
			 entry->is_blocked ? "-#" : (entry->is_disciplining ? "->" : "--")
			);

	if (entry->active_intf != NULL)
		sfptpd_log_stats(o, "%s(%s)", sfptpd_clock_get_short_name(entry->clock_slave),
						 sfptpd_interface_get_name(entry->active_intf));
	else
		sfptpd_log_stats(o, "%s", sfptpd_clock_get_long_name(entry->clock_slave));

	sfptpd_log_stats(o, "], "); /* To maintain backwards compatibility the comma var is actually useless */

	#define FLOAT_STATS_OUT(k, v, red) \
		if (entry->stat_present & (1 << k)) { \
			if (red) \
				sfptpd_log_stats(o, "%s%s: " SFPTPD_FORMAT_FLOAT_RED, comma, RT_STATS_KEY_NAMES[k], v); \
			else \
				sfptpd_log_stats(o, "%s%s: " SFPTPD_FORMAT_FLOAT, comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ", "; \
		}
	#define INT_STATS_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			sfptpd_log_stats(o, "%s%s: %d", comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ", "; \
		}
	#define EUI64_STATS_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			sfptpd_log_stats(o, "%s%s: " SFPTPD_FORMAT_EUI64, comma, RT_STATS_KEY_NAMES[k], \
						v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); \
			comma = ", "; \
		}

	bool alarm_red = sfptpd_log_isatty() && entry->alarms != 0;

	FLOAT_STATS_OUT(STATS_KEY_OFFSET, entry->offset, alarm_red);
	FLOAT_STATS_OUT(STATS_KEY_FREQ_ADJ, entry->freq_adj, 0);
	sfptpd_log_stats(o, "%sin-sync: %s", comma, entry->is_in_sync ? "1" : "0");
	comma = ", ";
	FLOAT_STATS_OUT(STATS_KEY_OWD, entry->one_way_delay, alarm_red);
	EUI64_STATS_OUT(STATS_KEY_PARENT_ID, entry->parent_id);
	EUI64_STATS_OUT(STATS_KEY_GM_ID, entry->gm_id);
	FLOAT_STATS_OUT(STATS_KEY_PPS_OFFSET, entry->pps_offset, 0);
	INT_STATS_OUT(STATS_KEY_BAD_PERIOD, entry->bad_period_count);
	INT_STATS_OUT(STATS_KEY_OVERFLOWS, entry->overflow_count);

	#undef FLOAT_STATS_OUT
	#undef INT_STATS_OUT
	#undef EUI64_STATS_OUT

	sfptpd_log_stats(o, "\n");
}

ssize_t sfptpd_log_render_rt_stat_json(struct sfptpd_log_time_cache *log_time_cache,
				       FILE* json_stats_fp,
				       struct sfptpd_sync_instance_rt_stats_entry *entry)
{
	char* comma = "";
	char ftime[24];
	size_t len = 0;

	assert(json_stats_fp != NULL);
	assert(entry != NULL);

	#define LPRINTF(...) { \
		int _ret = fprintf(__VA_ARGS__); \
		if (_ret < 0) { \
			 TRACE_L4("error writing json stats, %s\n", \
				  strerror(errno)); \
			 return -1; \
		} \
		len += _ret; \
	}

	LPRINTF(json_stats_fp, "{\"instance\":\"%s\",\"time\":\"%s\","
		"\"clock-master\":{\"name\":\"%s\"",
		entry->instance_name ? entry->instance_name : "",
		sfptpd_log_render_log_time(log_time_cache, entry),
		entry->clock_master ?
			sfptpd_clock_get_long_name(entry->clock_master) : entry->source);

	/* Add clock time */
	if (entry->clock_master != NULL) {
		if (entry->has_m_time) {
			sfptpd_secs_t secs = entry->time_master.sec;
			sfptpd_local_strftime(ftime, (sizeof ftime) - 1, "%Y-%m-%d %H:%M:%S", &secs);
			LPRINTF(json_stats_fp, ",\"time\":\"%s.%09" PRIu32 "\"",
				ftime, entry->time_master.nsec);
		}

		/* Extra info about clock interface, mostly useful when using bonds */
		if (entry->clock_master != sfptpd_clock_get_system_clock())
			LPRINTF(json_stats_fp, ",\"primary-interface\":\"%s\"",
				sfptpd_interface_get_name(
					sfptpd_clock_get_primary_interface(entry->clock_master)));
	}

	/* Slave clock info */
	LPRINTF(json_stats_fp, "},\"clock-slave\":{\"name\":\"%s\"",
		sfptpd_clock_get_long_name(entry->clock_slave));
	if (entry->has_s_time) {
		sfptpd_secs_t secs = entry->time_slave.sec;
		sfptpd_local_strftime(ftime, (sizeof ftime) - 1, "%Y-%m-%d %H:%M:%S", &secs);
		LPRINTF(json_stats_fp, ",\"time\":\"%s.%09" PRIu32 "\"",
			ftime, entry->time_slave.nsec);
	}

	/* Extra info about clock interface, mostly useful when using bonds */
	if (entry->clock_slave != sfptpd_clock_get_system_clock())
		 LPRINTF(json_stats_fp, ",\"primary-interface\":\"%s\"",
				 sfptpd_interface_get_name(
					 sfptpd_clock_get_primary_interface(entry->clock_slave)));

	LPRINTF(json_stats_fp, "},\"is-disciplining\":%s,\"in-sync\":%s,"
			       "\"alarms\":[",
			entry->is_disciplining ? "true" : "false",
			entry->is_in_sync ? "true" : "false");

	/* Alarms */
	len += sfptpd_sync_module_alarms_stream(json_stats_fp, entry->alarms, ",");

	LPRINTF(json_stats_fp, "],\"stats\":{");

	/* Print those stats which are present */
	#define FLOAT_JSON_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			LPRINTF(json_stats_fp, "%s\"%s\":%Lf", comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ","; \
		}
	#define INT_JSON_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			LPRINTF(json_stats_fp, "%s\"%s\":%d", comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ","; \
		}
	#define STRING_JSON_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			LPRINTF(json_stats_fp, "%s\"%s\":\"%s\"", comma, RT_STATS_KEY_NAMES[k], v); \
			comma = ","; \
		}
	#define EUI64_JSON_OUT(k, v) \
		if (entry->stat_present & (1 << k)) { \
			LPRINTF(json_stats_fp, "%s\"%s\":\"" SFPTPD_FORMAT_EUI64 "\"", \
					comma, RT_STATS_KEY_NAMES[k], \
					v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); \
			comma = ","; \
		}

	FLOAT_JSON_OUT(STATS_KEY_OFFSET, entry->offset);
	FLOAT_JSON_OUT(STATS_KEY_FREQ_ADJ, entry->freq_adj);
	FLOAT_JSON_OUT(STATS_KEY_OWD, entry->one_way_delay);
	EUI64_JSON_OUT(STATS_KEY_PARENT_ID, entry->parent_id);
	EUI64_JSON_OUT(STATS_KEY_GM_ID, entry->gm_id);
	STRING_JSON_OUT(STATS_KEY_ACTIVE_INTF, sfptpd_interface_get_name(entry->active_intf));
	STRING_JSON_OUT(STATS_KEY_BOND_NAME, entry->bond_name);
	FLOAT_JSON_OUT(STATS_KEY_PPS_OFFSET, entry->pps_offset);
	INT_JSON_OUT(STATS_KEY_BAD_PERIOD, entry->bad_period_count);
	INT_JSON_OUT(STATS_KEY_OVERFLOWS, entry->overflow_count);
	FLOAT_JSON_OUT(STATS_KEY_P_TERM, entry->p_term);
	FLOAT_JSON_OUT(STATS_KEY_I_TERM, entry->i_term);

	#undef FLOAT_JSON_OUT
	#undef INT_JSON_OUT
	#undef STRING_JSON_OUT
	#undef EUI64_JSON_OUT

	/* Close json object and flush stream */
	LPRINTF(json_stats_fp, "}}\n");

	#undef LPRINTF

	return len;
}

/* fin */
