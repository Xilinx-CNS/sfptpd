/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2023 Xilinx, Inc. */

#ifndef _SFPTPD_LOGGING_H
#define _SFPTPD_LOGGING_H

#include "glibc_compat.h"

#include <stdio.h>
#include <syslog.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>

#include "sfptpd_misc.h"

/****************************************************************************
 * Structures, Types, Defines
 ****************************************************************************/

/** Macros used to log messages of different severities */
#define EMERGENCY(x, ...) sfptpd_log_message(LOG_EMERG, x, ##__VA_ARGS__)
#define ALERT(x, ...)     sfptpd_log_message(LOG_ALERT, x, ##__VA_ARGS__)
#define CRITICAL(x, ...)  sfptpd_log_message(LOG_CRIT, x, ##__VA_ARGS__)
#define ERROR(x, ...)     sfptpd_log_message(LOG_ERR, x, ##__VA_ARGS__)
#define WARNING(x, ...)   sfptpd_log_message(LOG_WARNING, x, ##__VA_ARGS__)
#define NOTICE(x, ...)    sfptpd_log_message(LOG_NOTICE, x, ##__VA_ARGS__)
#define INFO(x, ...)      sfptpd_log_message(LOG_INFO, x, ##__VA_ARGS__)

/** Component IDs used to categorize trace debugging messages */
typedef enum sfptpd_component_id {
	SFPTPD_COMPONENT_ID_SFPTPD,
	SFPTPD_COMPONENT_ID_PTPD2,
	SFPTPD_COMPONENT_ID_THREADING,
	SFPTPD_COMPONENT_ID_BIC,
	SFPTPD_COMPONENT_ID_NETLINK,
	SFPTPD_COMPONENT_ID_NTP,
	SFPTPD_COMPONENT_ID_SERVO,
	SFPTPD_COMPONENT_ID_CLOCKS,
	SFPTPD_COMPONENT_ID_MAX
} sfptpd_component_id_e;

/** For debugging a trace macro is defined. The higher the level, the more verbose
 * the trace information. Valid values for the level are >= 1. */
#define TRACE(c, l, x, ...)  sfptpd_log_trace(c, l, x, ##__VA_ARGS__)

/** Trace macros by increasing verbosity */
#define TRACE_L1(x, ...)  TRACE(SFPTPD_COMPONENT_ID_SFPTPD, 1, x, ##__VA_ARGS__)
#define TRACE_L2(x, ...)  TRACE(SFPTPD_COMPONENT_ID_SFPTPD, 2, x, ##__VA_ARGS__)
#define TRACE_L3(x, ...)  TRACE(SFPTPD_COMPONENT_ID_SFPTPD, 3, x, ##__VA_ARGS__)
#define TRACE_L4(x, ...)  TRACE(SFPTPD_COMPONENT_ID_SFPTPD, 4, x, ##__VA_ARGS__)
#define TRACE_L5(x, ...)  TRACE(SFPTPD_COMPONENT_ID_SFPTPD, 5, x, ##__VA_ARGS__)
#define TRACE_L6(x, ...)  TRACE(SFPTPD_COMPONENT_ID_SFPTPD, 6, x, ##__VA_ARGS__)
#define TRACE_LX(level, x, ...)  TRACE(SFPTPD_COMPONENT_ID_SFPTPD, level, x, ##__VA_ARGS__)

/** Macros defining string formats for common data types */
#define TEXT_RED                          "\033[31m"
#define TEXT_DEFAULT                      "\033[0m"
#define SFPTPD_FORMAT_EUI48               "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx"
#define SFPTPD_FORMAT_EUI64               "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx"
#define SFPTPD_FORMAT_EUI64_SEP           "%02hhx%02hhx%c%02hhx%02hhx%c%02hhx%02hhx%c%02hhx%02hhx"
#define SFPTPD_FORMAT_EUI64_NOSEP         "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx"
#define SFPTPD_FORMAT_FLOAT               "%0.3Lf"
#define SFPTPD_FORMAT_FLOAT_RED           TEXT_RED SFPTPD_FORMAT_FLOAT TEXT_DEFAULT
#define SFPTPD_FORMAT_TOPOLOGY_FLOAT      SFPTPD_FORMAT_FLOAT " ns"
#define SFPTPD_FORMAT_TIMESPEC            "%ld.%09ld"
#define SFPTPD_FORMAT_STIMESPEC           "%s%ld.%09ld"
#define SFPTPD_FMT_SFTIMESPEC             "%" PRIu64 ".%09" PRIu32 "%03" PRIu32
#define SFPTPD_FMT_SSFTIMESPEC            "%s%" PRId64 ".%012" PRIu64
#define SFPTPD_FMT_SFTIMESPEC_FIXED       "%22" PRIu64 ".%09" PRIu32 "%03" PRIu32
#define SFPTPD_FMT_SSFTIMESPEC_NS         "%s%" PRId64 ".%09" PRIu32

/** Macros defining arguments for compound time formats */
#define SFPTPD_ARGS_TIMESPEC(ts) \
	(ts).tv_sec, \
	(ts).tv_nsec
#define SFPTPD_ARGS_STIMESPEC(ts) \
	((ts).tv_sec == -1 && ((ts).tv_nsec != 0) ? "-" : ""), \
	((ts).tv_sec >= 0 || (ts).tv_nsec == 0 ? ((ts).tv_sec) : (ts).tv_sec + 1), \
	((ts).tv_sec >= 0 || (ts).tv_nsec == 0 ? (ts).tv_nsec : 1000000000 - (ts).tv_nsec)
#define _SFPTPD_NSEC_FRAC_TO_DEC(ts) \
	(uint32_t) (((uint64_t)((ts).nsec_frac) * 1000 + 0x80000000UL) >> 32)
#define SFPTPD_ARGS_SFTIMESPEC(ts) \
	(ts).sec, \
	(ts).nsec, \
	_SFPTPD_NSEC_FRAC_TO_DEC((ts))
#define SFPTPD_ARGS_SSFTIMESPEC_S(ts) \
	((ts).sec == -1 && ((ts).nsec != 0) ? "-" : ""), \
	((ts).sec >= 0 || (ts).nsec == 0 ? ((ts).sec) : (ts).sec + 1)
#define SFPTPD_ARGS_SSFTIMESPEC_NS(ts) \
	SFPTPD_ARGS_SSFTIMESPEC_S(ts), \
	((ts).sec >= 0 || (ts).nsec == 0 ? (ts).nsec : 1000000000 - (ts).nsec)
#define SFPTPD_ARGS_SSFTIMESPEC(ts) \
	SFPTPD_ARGS_SSFTIMESPEC_S(ts), \
	((ts).sec >= 0 || (ts).nsec == 0 ? \
		1000 * ((uint64_t)((ts).nsec)) + _SFPTPD_NSEC_FRAC_TO_DEC((ts)) : \
		1000000000000 - 1000 * ((uint64_t)((ts).nsec)) - _SFPTPD_NSEC_FRAC_TO_DEC((ts)))

/** Forward structure declarations */
struct sfptpd_config;
struct sfptpd_clock;
struct sfptpd_stats_range;

/** Opaque public structure declaration for structure used to store
 *  state of an open snapshot-type log file.
 */
struct sfptpd_log;

/** Structure used to hold date and time for printing */
struct sfptpd_log_time {
#define SFPTPD_LOG_TIME_STR_MAX (32)
	char time[SFPTPD_LOG_TIME_STR_MAX];
};


/** Buildtime checks. Turn all functions that do printf-type operations into
 * printf to get compiler checks on the format string and parameter list */
#ifdef SFPTPD_BUILDTIME_CHECKS
#define sfptpd_log_message(p, f, ...) printf(f, ##__VA_ARGS__)
#define sfptpd_log_trace(c, l, f, ...) printf(f, ##__VA_ARGS__)
#define sfptpd_log_stats(f, ...) printf(f, ##__VA_ARGS__)
#define sfptpd_log_write_state(c, i, f, ...) printf(f, ##__VA_ARGS__)
#define sfptpd_log_topology_write_field(s, n, f, ...) printf(f, ##__VA_ARGS__)
#endif


/** For printing out tables for logging */
#define SFPTPD_LOGGING_NODE_STRING_LENGTH 	    (60)
#define SFPTPD_LOGGING_INTERFACE_STRING_LENGTH	    (53)


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Set up message and statistics logs according to configuration
 * @param config Pointer to configuration
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_log_open(struct sfptpd_config *config);

/** Close message and statistics logs and free resources
 */
void sfptpd_log_close(void);

/** Set trace level. Can be used to modify the trace level at runtime
 * @param component Component for which level is being set
 * @param level Trace level - 0 is off
 */
void sfptpd_log_set_trace_level(sfptpd_component_id_e component, int level);

/** Rotate statistics log file. If statistics logging is enabled and directed
 * to a file, closes the existing log and opens a new log file.
 * @param config Pointer to configuration
 * @return 0 on success or an errno otherwise.
 */
int sfptpd_log_rotate(struct sfptpd_config *config);

/** Check whether stats logging to a typewriter
 * @return A boolean indicating whether stats logging is to a typewriter.
 */
bool sfptpd_log_isatty(void);

#ifndef SFPTPD_BUILDTIME_CHECKS
/** Log a message.
 * @param priority Priority of the message (linux syslog priorities)
 * @param format Format string followed by a variable list of parameters
 */
void sfptpd_log_message(int priority, const char *format, ...);

/** Log a trace message. Messages (TRACE() macro) are only logged if lower
 * than the current trace level for the component
 * @param component Component that is logging
 * @param level Trace level of the message
 * @param format Format string followed by a variable list of parameters
 */
void sfptpd_log_trace(sfptpd_component_id_e component, unsigned int level,
		      const char *format, ...);

/** Log statistics. If statistics logging is enabled, writes stats to either
 * stdout or to a file.
 * @param format Format string followed by a variable list of parameters
 */
void sfptpd_log_stats(const char *format, ...);

/** Write state information for a clock. This creates a file in the
 * location SFPTPD_STATE_PATH with the name of the specified clock and the
 * instance name.
 * @param clock Instance of clock to which the state applies
 * @param sync_instance_name Name of the sync module instance that produced the
 * state or NULL if the state has been produced by a local clock
 * synchronization process.
 * @param format Format string followed by a variable list of parameters
 */
void sfptpd_log_write_state(struct sfptpd_clock *clock,
			    const char *sync_instance_name,
			    const char *format, ...);
#endif /* SFPTPD_BUILDTIME_CHECKS */

/** Write frequency correction for a clock. This creates a file in the
 * location STPTPD_STATE_PATH with the name of the specified clock containing
 * the specified frequency correction.
 * @param clock Instance of clock
 * @param freq_adj_ppb The frequency adjustment in parts-per-billion.
 * @return 0 on success or an errno otherwise
 */
int sfptpd_log_write_freq_correction(struct sfptpd_clock *clock, long double freq_adj_ppb);

/** Read frequency correction for a clock. This reads a file in the location
 * SFPTPD_STATE_PATH based on the name of the specified clock and returns
 * the saved frequency correction value.
 * @param clock Instance of clock
 * @param freq_adj_ppb Returned frequency adjustment in parts-per-billion.
 * @return 0 on success or an errno otherwise
 */
int sfptpd_log_read_freq_correction(struct sfptpd_clock *clock, long double *freq_adj_ppb);

/** Delete the frequency correction for a clock.
 * @param clock Instance of clock
 */
void sfptpd_log_delete_freq_correction(struct sfptpd_clock *clock);

/** Open topology file for writing. It is the responsibility of the caller to
 * close the file once the information has been written using fclose().
 * @return A logging object on success or NULL on error
 */
struct sfptpd_log *sfptpd_log_open_topology(void);

#ifndef SFPTPD_BUILDTIME_CHECKS
/** Write a field to the topology file with central justification
 * @param stream Stream to write to
 * @param new_line Write a new line after the field
 * @param format Formatting string for field to be written
 * @param ... Variable length argument list
 */
void sfptpd_log_topology_write_field(FILE *stream, bool new_line,
				     const char *format, ...);
#endif /* SFPTPD_BUILDTIME_CHECKS */

/** Write a 1-to-1 connector between nodes in the topology
 * @param stream Stream to write to
 * @param arrow_top Draw arrow at top of connector
 * @param arrow_bottom Draw arrow at bottom of connector
 * @param label Formatting string for connector label or NULL
 * @param ... Variable length argument list
 */
void sfptpd_log_topology_write_1to1_connector(FILE *stream, bool arrow_top,
					      bool arrow_bottom,
					      const char *label, ...);

/** Write beginning of a 1-to-n connector between nodes in the topology
 * @param stream Stream to write to
 * @param num_nodes Number of destination nodes for the connector
 * @param arrow Draw arrow at top of connector
 */
void sfptpd_log_topology_write_1ton_connector_start(FILE *stream, int num_modes,
						    bool arrow);

/** Write end of a 1-to-n connector between nodes in the topology
 * @param stream Stream to write to
 * @param num_nodes Number of destination nodes for the connector
 * @param arrow Draw arrows at bottom of connector
 */
void sfptpd_log_topology_write_1ton_connector_end(FILE *stream, int num_nodes,
						  bool arrow);

/** Open interfaces file for writing. It is the responsibility of the caller
 * to close the file once the information has been written using
 * sfptpd_log_file_close().
 * @return A file handle on success or NULL on error
 */
struct sfptpd_log *sfptpd_log_open_interfaces(void);

/** Open ptp-nodes file for writing. It is the responsibility of the caller
 * to close the file once the information has been written using
 * sfptpd_log_file_close().
 * @return A file handle on success or NULL on error
 */
struct sfptpd_log *sfptpd_log_open_ptp_nodes(void);

/** Open sync-instances file for writing. It is the responsibility of the
 * caller to close the file once the information has been written using
 * sfptpd_log_file_close().
 * @return A file handle on success or NULL on error
 */
struct sfptpd_log *sfptpd_log_open_sync_instances(void);

/** Appends a new row to a table. Used to create interfaces and ptp-nodes files
 * @param stream Stream to write to
 * @param draw_line Output a horizontal line after this row
 * @param format Format string to print with
 */
void sfptpd_log_table_row(FILE *stream, bool draw_line, const char *format, ...);

/** Open statistics file for writing. It is the responsibility of the caller to
 * close the file once the information has been written using
 * sfptpd_log_file_close().
 * @param clock Instance of clock to which stats apply
 * @param sync_instance_name Name of the sync module instance that produced the
 * stats or NULL if the stats have been produced by a local clock
 * synchronization process.
 * @return A file handle on success or NULL on error
 */
struct sfptpd_log *sfptpd_log_open_statistics(struct sfptpd_clock *clock,
					      const char *sync_instance_name);

/** Open machine-readable statistics file for writing. It is the responsibility
 * of the caller to close the file once the information has been written using
 * sfptpd_log_file_close().
 * @param clock Instance of clock to which stats apply
 * @param entity_name Name of the sync module instance that produced the
 * stats or NULL if the stats have been produced by a local clock
 * synchronization process.
 * @return A file handle on success or NULL on error
 */
struct sfptpd_log *sfptpd_log_open_statistics_json(struct sfptpd_clock *clock,
					      const char *entity_name);

/** Open remote monitoring file for writing. It is the responsibility of the
 * caller to close the file once the information has been written using
 * sfptpd_log_file_close().
 * @return A logging object on success or NULL on error
 */
struct sfptpd_log *sfptpd_log_open_remote_monitor(void);

/** Open remote monitoring file for writing streaming stats in JSON Lines
 * format. It is the responsibility of the caller to close the file once
 * the information has been written using sfptpd_log_file_close().
 * @return A logging object on success or NULL on error
 */
struct sfptpd_log *sfptpd_log_open_remote_monitor_json(void);

/** Get the stream for a log file.
 * @param log The opaque log file state.
 * return The stream or NULL on error
 */
FILE *sfptpd_log_file_get_stream(struct sfptpd_log *log);

/** Close a log file
 * @param log The opaque log file state.
 * return 0 on success or an errno otherwise.
 */
int sfptpd_log_file_close(struct sfptpd_log *log);

/** Get the time as a string
 * @param time  Pointer to returned time string
 */
void sfptpd_log_get_time(struct sfptpd_log_time *time);

/** Gets the output stream for realtime stats. Don't open/close this.
 * @return Stream pointer. May be NULL if realtime stats are disabled.
 */
FILE* sfptpd_log_get_rt_stats_out_stream(void);

/** Records number of characters of RT stats written.
 * @param chars The number of characters written.
 * @param flush Whether to flush the buffer regardless.
 * @return Whether the buffer was flushed.
 */
bool sfptpd_log_rt_stats_written(size_t chars, bool flush);

/** Gets the output stream for remote monitoring. Don't open/close this.
 * @return Stream pointer. May be NULL if remote monitoring is disabled.
 */
FILE* sfptpd_log_get_remote_monitor_out_stream(void);

/** Log a line of lexed configuration */
void sfptpd_log_lexed_config(const char *format, ...);

/** Abandon config logging in case of error */
void sfptpd_log_config_abandon(void);

/** Get specification for path interpolation
 * @return format specifiers.
 */
const struct sfptpd_interpolation *sfptpd_log_get_format_specifiers(void);

#endif /* _SFPTPD_LOGGING_H */
