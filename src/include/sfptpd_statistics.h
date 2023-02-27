/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_STATISTICS_H
#define _SFPTPD_STATISTICS_H

#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include "sfptpd_clock.h"
#include "sfptpd_misc.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/
 
/** Forward declarations */
struct sfptpd_hash_table;

/** Default minimum period required for convergence and maximum offset */
#define SFPTPD_STATS_CONVERGENCE_MIN_PERIOD_DEFAULT 60
#define SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_DEFAULT 1000.0

/** Alternative maximum offset, used when operating PTP in software
 * timestamping mode */
#define SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_SW_TS 100000.0

/** Maximum offset used for NTP sync module */
#define SFPTPD_STATS_CONVERGENCE_MAX_OFFSET_NTP 10000000.0

/** Maximum size of a stats collection */
#define SFPTPD_STATS_COLLECTION_MAX_SIZE (256)

/** Size of hash table for all uses (as expecting maximums of 50 slaves,
 * 1 master or 3 domains) */
#define SFPTPD_STATS_SET_SIZE (20)

/** Max number of entries for hash table */
#define SFPTPD_HT_STATS_SET_MAX (1024)

/** Max length of transport address to add to nodes table */
#define SFPTPD_NODES_MAX_ADDRESS_LEN (40)

struct sfptpd_clock_hw_id;

/** struct sfptpd_stats_convergence.
 * Convergence measure, used to decide whether a slave clock is synchronized
 * to its master
 * @min_period: Minimum time period to declare convergence
 * @max_offset: Maximum offset allowed for convergence
 * @num_samples: Count of number of samples since first sample in convergence
 * range
 * @start_time: Time at which first sample was within convergence range
 * @latest_time: Time of last sample
 */
typedef struct sfptpd_stats_convergence
{
	time_t min_period;
	long double max_offset;
	unsigned int num_samples;
	time_t start_time;
	time_t latest_time;
} sfptpd_stats_convergence_t;


/** Structure sfptpd_stats_std_dev
 * Structure used to calculate the standard deviation of a set of data
 * @num_samples: Number of data samples
 * @sum_data: Sum of the data samples
 * @sum_data_squared: Sum of the square of the data samples
 */
typedef struct sfptpd_stats_std_dev {
	unsigned int num_samples;
	long double sum_data;
	long double sum_data_squared;
} sfptpd_stats_std_dev_t;


/** struct sfptpd_stats_pps
 * Raw PPS statistics as reported by the NIC
 * @overflow_count: Count of number of PPS pulse queue overflows
 * @bad_period_count: Count of number of bad PPS period measurements
 * @offset.last: Last PPS offset
 * @offset.mean: Mean PPS offset since stats were last cleared
 * @offset.min: Minimum PPS offset since stats were last cleared
 * @offset.max: Maximum PPS offset since stats were last cleared
 * @period.last: Last PPS period
 * @period.mean: Mean PPS period since stats were last cleared
 * @period.min: Minimum PPS period since stats were last cleared
 * @period.max: Maximum PPS period since stats were last cleared
 */
typedef struct sfptpd_stats_pps
{
	int overflow_count;
	int bad_period_count;
	struct {
		int last;
		int mean;
		int min;
		int max;
	} offset;
	struct {
		int last;
		int mean;
		int min;
		int max;
	} period;
} sfptpd_stats_pps_t;


/** struct sfptpd_stats_range
 * Statistical measure of the range of a value over a period of time
 * @valid: Indicates that the structure has been initialised
 * @num_samples: Total samples taken during the period
 * @total: Sum total of all samples taken during the period
 * @total: Sum total of the squares of samples taken during the period
 * @min: Minimum value recorded during the period
 * @max: Maximum value recorded during the period
 * @qualified: A flag that indicates that a qualification criterion
 * for the data applied continuously across the window.
 * @min_time: The time when the minimum value occurred.
 * @max_time: The time when the maximum value occurred.
 */
typedef struct sfptpd_stats_range
{
	bool valid;
	unsigned long num_samples;
	long double total;
	long double total_squares;
	long double min;
	long double max;
	bool qualified;
	struct timespec min_time;
	struct timespec max_time;
} sfptpd_stats_range_t;


/** struct sfptpd_stats_count
 * Statistical count of a value over a period of time
 * @valid: Indicates that the structure has been initialised
 * @num_samples: Total samples taken during the period
 * @total: Count total for the period
 */
typedef struct sfptpd_stats_count
{
	bool valid;
	unsigned long num_samples;
	unsigned long total;
} sfptpd_stats_count_t;


/** enum sfptpd_stats_type
 * Enum of different statistics measures used to identify members of
 * a statistics collection
 */
enum sfptpd_stats_type
{
	SFPTPD_STATS_TYPE_RANGE,
	SFPTPD_STATS_TYPE_COUNT,
	SFPTPD_STATS_TYPE_MAX
};


/** enum sfptpd_stats_time_period
 * Enum of different time periods over which historical statistics data
 * can be collected
 */
enum sfptpd_stats_time_period
{
	SFPTPD_STATS_PERIOD_MINUTE,
	SFPTPD_STATS_PERIOD_TEN_MINUTES,
	SFPTPD_STATS_PERIOD_HOUR,
	SFPTPD_STATS_PERIOD_DAY,
	SFPTPD_STATS_PERIOD_WEEK,
	SFPTPD_STATS_PERIOD_MAX
};


/** enum sfptpd_stats_history_index
 * Number of recorded data sets for each time period
 */
enum sfptpd_stats_history_index
{
	SFPTPD_STATS_HISTORY_CURRENT,
	SFPTPD_STATS_HISTORY_1,
	SFPTPD_STATS_HISTORY_2,
	SFPTPD_STATS_HISTORY_3,
	SFPTPD_STATS_HISTORY_MAX
};


/** struct sfptpd_stats_item_ops
 * Abstracted interface to allow statistical measures to be handled in a
 * generic way.
 * @free: Free the resources associated with the statistics item
 * @update: Update the statistic. The argument list contains the new sample. The
 * format is specific to the statistics type being updated
 * @end_period: The end of the current stats period
 * @write_headings: Write the statistic column headings to the stream provided
 * @write_data: Write a statistics entry row to the stream provided
 * @write_json_opening: Write the JSON opening to the stream provided
 * @write_json_data: Write a JSON-formatted entry to the stream provided
 * @write_json_closing: Write the JSON closing to the stream provided
 * @get: Get a historical statistic entry
 */
struct sfptpd_stats_item;
typedef struct sfptpd_stats_item_ops
{
	void (*free)(struct sfptpd_stats_item *item);
	void (*update)(struct sfptpd_stats_item *item, va_list args);
	void (*end_period)(struct sfptpd_stats_item *item,
			   enum sfptpd_stats_time_period period);
	void (*write_headings)(struct sfptpd_stats_item *item, FILE *stream);
	void (*write_data)(struct sfptpd_stats_item *item, FILE *stream,
			   const char *name, const char *start, const char *end,
			   enum sfptpd_stats_time_period period,
			   enum sfptpd_stats_history_index index);
	void (*write_json_opening)(struct sfptpd_stats_item *item, FILE *stream);
	void (*write_json_data)(struct sfptpd_stats_item *item, FILE *stream,
			   enum sfptpd_stats_time_period period,
			   enum sfptpd_stats_history_index index,
			   const char *period_name, int period_secs, int seq_num,
			   const char *start, const char *end);
	void (*write_json_closing)(struct sfptpd_stats_item *item, FILE *stream);
	int (*get)(struct sfptpd_stats_item *item,
		   enum sfptpd_stats_time_period period,
		   enum sfptpd_stats_history_index index,
		   va_list args);
} sfptpd_stats_item_ops_t;


/** struct sfptpd_stats_item
 * Base class for a statistics classes of different measures
 * @type: Type of statistical measure
 * @naym: Name of data item
 * @units: Units of data item or NULL if none
 * @decimal_places: Number of decimal places that should be displayed
 * @ops: Structure of function pointers providing interface to derived classes
 */
typedef struct sfptpd_stats_item
{
	enum sfptpd_stats_type type;
	const char *name;
	const char *units;
	unsigned int decimal_places;
	const struct sfptpd_stats_item_ops *ops;
} sfptpd_stats_item_t;


/** struct sfptpd_stats_time_interval
 * Structure describing a specific time interval
 * @seq: A sequence number for the window
 * @start_valid: Indicates that the start time is valid
 * @end_valid: Indicates that the start time is valid
 * @start_time: Start time of time interval
 * @end_time: End time of time interval
 */
typedef struct sfptpd_stats_time_interval
{
	unsigned long seq_num;
	bool start_valid;
	bool end_valid;
	struct timespec start_time;
	struct timespec end_time;
} sfptpd_stats_time_interval_t;


/** struct sfptpd_stats_collection
 * Container for a collection of statistics
 * @naym: Textual name of the stats collection
 * @elapsed: Elapsed seconds for each stats period
 * @intervals: Two dimensional array storing a set of time intervals for each
 * period and instance. At the end of each time period the first element in
 * the array stores the end time for this time period. The subsequent entries
 * store completed historical time interval e.g. the first entry in the array
 * contains the interval for the current "minute" time period. Each time that
 * the end of the period is reached, this data is copied to the historical
 * time interval data for "minute" with all the existing historical "minute"
 * moved to an older position. The oldest historical data for the time period
 * is discarded.
 * @capacity: Current size of the stats items array
 * @items: Array of pointers to stats items
 */
typedef struct sfptpd_stats_collection
{
	const char *name;
	unsigned int elapsed[SFPTPD_STATS_PERIOD_MAX];
	sfptpd_stats_time_interval_t intervals[SFPTPD_STATS_PERIOD_MAX][SFPTPD_STATS_HISTORY_MAX];	
	unsigned int capacity;
	struct sfptpd_stats_item **items;
} sfptpd_stats_collection_t;


/** struct sfptpd_stats_collection_defn
 * Structure used to define a stats items when creating a collection
 * @id: Index that identifies stats item
 * @type: Type of statistical measure
 * @naym: Name of data item
 * @units: Units of data item or NULL if no units
 * @decimal_places: Number of decimal places to display
 */
typedef struct sfptpd_stats_collection_defn
{
	unsigned int id;
	enum sfptpd_stats_type type;
	const char *name;
	const char *units;
	unsigned int decimal_places;
} sfptpd_stats_collection_defn_t;


/** struct sfptpd_stats_ptp_node
 * Structure used to define ptp-node specific aspects for use in a hash table
 * @clock_id: Clock ID of the node
 * @clock_id_string: String representation of clock ID
 * @state: Whether node is a master or slave
 * @port_number: Port number of the node
 * @domain_number: Domain number of the node
 */
typedef struct sfptpd_stats_ptp_node {
	sfptpd_clock_id_t clock_id;
	char clock_id_string[SFPTPD_CLOCK_HW_ID_STRING_SIZE];
	const char *state;
	unsigned int port_number;
	unsigned int domain_number;
	char transport_address[SFPTPD_NODES_MAX_ADDRESS_LEN];
} sfptpd_stats_ptp_node_t;


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Initialise a convergence measure and set the default convergence criteria.
 * @param conv  Pointer to convergence structure
 */
void sfptpd_stats_convergence_init(struct sfptpd_stats_convergence *conv);

/** Set the minimum period during which the offset must be below the maximum
 * valid offset before convergence is declared.
 * @param conv  Pointer to convergence structure
 * @param min_period  Minimum period before convergence will be declared
 */
void sfptpd_stats_convergence_set_min_period(struct sfptpd_stats_convergence *conv,
					     time_t min_period);

/** Set the maximum offset for the convergence criteria. 
 * @param conv  Pointer to convergence structure
 * @param max_offset_ns  Maximum offset allowed to maintain convergence
 */
void sfptpd_stats_convergence_set_max_offset(struct sfptpd_stats_convergence *conv,
					     long double max_offset_ns);

/** Invalidate the state of convergence measure to not converged
 * @param conv  Pointer to convergence structure
 */
void sfptpd_stats_convergence_reset(struct sfptpd_stats_convergence *conv);

/** Process a new sample.
 * @param conv  Pointer to convergence structure
 * @param time  Current time
 * @param offset_ns  Current offset
 * @return A boolean indicating whether convergence criteria are currently met
 */
bool sfptpd_stats_convergence_update(struct sfptpd_stats_convergence *conv,
				     time_t time, long double offset_ns);


/** Initialise a standard deviation measure.
 * @param std_dev Pointer to standard deviation stats structure
 */
void sfptpd_stats_std_dev_init(struct sfptpd_stats_std_dev *std_dev);

/** Add a sample to a standard deviation measure.
 * @param std_dev Pointer to standard deviation structure
 * @param sample Sample to add
 */
void sfptpd_stats_std_dev_add_sample(struct sfptpd_stats_std_dev *std_dev,
				     long double sample);

/** Remove a sample from a standard deviation measure.
 * @param std_dev Pointer to standard deviation structure
 * @param sample Sample to remove
 */
void sfptpd_stats_std_dev_remove_sample(struct sfptpd_stats_std_dev *std_dev,
					long double sample);

/** Get the standard deviation and mean of the data set.
 * @param std_dev Pointer to standard deviation structure
 * @param mean Pointer to returned mean. Can be null if mean not required
 * @return Standard deviation
 */
long double sfptpd_stats_std_dev_get(struct sfptpd_stats_std_dev *std_dev,
				     long double *mean);


/** Get the PPS statistics
 * @param interface Interface for which PPS stats are required
 * @param pps_stats Pointer to structure where PPS stats should be written
 * @return 0 on success or an errno otherwise
 */
int sfptpd_stats_get_pps_statistics(struct sfptpd_interface *interface,
				    struct sfptpd_stats_pps *pps_stats);

/** Reset the PPS statistics
 * @param interface Interface for which PPS stats are to be reset
 */
void sfptpd_stats_reset_pps_statistics(struct sfptpd_interface *interface);


/** Initialise statistics range measure
 * @param range Pointer to object to be initialised
 */
void sfptpd_stats_range_init(struct sfptpd_stats_range *range);

/** Update statistics range measure
 * @param range Pointer to measure
 * @param sample Data sample
 * @param time The time for the sample
 * @param qualified Whether an underlying qualification condition
 * was true when the sample was collected
 */
void sfptpd_stats_range_update(struct sfptpd_stats_range *range,
			       long double sample,
			       struct timespec time,
			       bool qualified);

/** Add one set of range statistics to another. Used to accummulate
 * statistics from short periods into stats for a longer period.
 * @param dst Pointer to destination measure
 * @param src Pointer to source measure
 */
void sfptpd_stats_range_add(struct sfptpd_stats_range *dst,
			    struct sfptpd_stats_range *src);


/** Initialise statistics count measure
 * @param count Pointer to object to be initialised
 */
void sfptpd_stats_count_init(struct sfptpd_stats_count *count);

/** Update statistics count measure
 * @param count Pointer to measure
 * @param sample Data sample. Value 0 will increase the number of
 * samples and can be used to indicate that an event did not occur.
 */
void sfptpd_stats_count_update(struct sfptpd_stats_count *count,
			       unsigned long sample,
			       unsigned long num_samples);

/** Add one set of range statistics to another. Used to accummulate
 * statistics from short periods into stats for a longer period.
 * @param dst Pointer to destination measure
 * @param src Pointer to source measure
 */
void sfptpd_stats_count_add(struct sfptpd_stats_count *dst,
			    struct sfptpd_stats_count *src);


/** Allocate a statistics collection
 * @param stats Pointer to collection object to be initialised
 * @param name Name for the stats collection
 * @return 0 on success or an errno on failure
 */
int sfptpd_stats_collection_alloc(struct sfptpd_stats_collection *stats,
				  const char *name);

/** Create a statistics collection. Allocate a collection and add the
 * statistics specified by the supplied array of definitions.
 * @param stats Pointer to collection
 * @param name Name for the stats collection
 * @param num_items Number of stats items to add to the collection
 * @param definitions Array of stats definitions to add to the collection
 * @return 0 on success or an errno on failure
 */
int sfptpd_stats_collection_create(struct sfptpd_stats_collection *stats,
				   const char *name,
				   unsigned int num_items,
				   const struct sfptpd_stats_collection_defn *definitions);

/** Free a statistics collection and all the statistics it contains
 * @param stats Pointer to collection object to be initialised
 */
void sfptpd_stats_collection_free(struct sfptpd_stats_collection *stats);

/** Add a new statistics entry to a collection. All statistics entries
 * are indentified by an index into the collection. When creating entries
 * best practice is to use sequential numbers beginning with 0.
 * @param stats Pointer to collection
 * @param id Index that identifies statistics item
 * @param type Type of statistical measure required
 * @param name Textual name for the entry
 * @param units Units of the statistic
 * @param decimal_places Number of decimal places to display
 * @return 0 on success or an errno on failure
 */
int sfptpd_stats_collection_add(struct sfptpd_stats_collection *stats,
				unsigned int id,
				enum sfptpd_stats_type type,
				const char *name,
				const char *units,
				unsigned int decimal_places);

/** Update a range statistical measure with a new data sample
 * @param stats Pointer to collection
 * @param index Index of statistics entry to update
 * @param sample New data sample in type appropriate for the statistics entry
 * @param time A timestamp for this sample
 * @param qualified Whether this sample is qualified
 * @return 0 on success or an errno on failure
 */
int sfptpd_stats_collection_update_range(struct sfptpd_stats_collection *stats,
					 unsigned int index,
					 long double sample,
					 struct timespec time,
					 bool qualified);

/** Update a count statistical measure with a new data sample
 * @param stats Pointer to collection
 * @param index Index of statistics entry to update
 * @param sample New data sample in type appropriate for the statistics entry
 * @return 0 on success or an errno on failure
 */
int sfptpd_stats_collection_update_count(struct sfptpd_stats_collection *stats,
					 unsigned int index,
					 unsigned long sample);

/** Update a count statistical measure with a new data sample
 * @param stats Pointer to collection
 * @param index Index of statistics entry to update
 * @param sample New data sample in type appropriate for the statistics entry
 * @param num_samples Total number of new samples in type appropriate for the statistics entry
 * @return 0 on success or an errno on failure
 */
int sfptpd_stats_collection_update_count_samples(struct sfptpd_stats_collection *stats,
					 unsigned int index,
					 unsigned long sample,
					 unsigned long num_samples);

/** Indicate the end of a statistics period and update the history of each
 * stats item accordingly.
 * @param stats Pointer to collection
 * @param end_time Realtime clock time at the end of the stats period
 */
void sfptpd_stats_collection_end_period(struct sfptpd_stats_collection *stats,
				        struct timespec *end_time);

/** Write all the statistics to the appropriate log file.
 * @param stats Pointer to collection
 * @param clock Handle of instance clock, used as a backup for log file name
 * @param sync_instance_name Used for log file name, may be null
 */
void sfptpd_stats_collection_dump(struct sfptpd_stats_collection *stats,
								  struct sfptpd_clock *clock,
								  const char *sync_instance_name);

/** Read a historical statistic
 * @param stats Pointer to collection
 * @param index Index of statistical item of interest
 * @param period Stats period of interest
 * @param index Stats history instance of interest
 * @param mean Pointer to returned mean value for the statistic over this
 * period or NULL if not required
 * @param min Pointer to returned min value for the statistic over this
 * period or NULL if not required
 * @param max Pointer to returned max value for the statistic over this
 * period or NULL if not required
 * @param qualified Pointer to returned qualified value for the statistic over this
 * period or NULL if not required
 * @param min_time Pointer to returned min_time value for the statistic over this
 * period or NULL if not required
 * @param max_time Pointer to returned max_time value for the statistic over this
 * period or NULL if not required
 * @return 0 on success or ENOENT if no valid entry for specified period
 * and instance
 */
int sfptpd_stats_collection_get_range(struct sfptpd_stats_collection *stats,
				      unsigned int index,
				      enum sfptpd_stats_time_period period,
				      enum sfptpd_stats_history_index instance,
				      long double *mean, long double *min,
				      long double *max, int *qualified,
				      struct timespec *min_time,
				      struct timespec *max_time);

/** Read a statistic history
 * @param stats Pointer to collection
 * @param index Index of statistical item of interest
 * @param period Stats period of interest
 * @param instance Stats history index of interest
 * @param count Pointer to returned count value for the statistic over this
 * period
 * @return 0 on success or ENOENT if no valid entry for specified period
 * and instance
 */
int sfptpd_stats_collection_get_count(struct sfptpd_stats_collection *stats,
				      unsigned int index,
				      enum sfptpd_stats_time_period period,
				      enum sfptpd_stats_history_index instance,
				      unsigned long *count);


/** Read metadata for a statistic
 * @param stats Pointer to collection
 * @param period Stats period of interest
 * @param instance Stats history index of interest
 * @param interval Pointer to returned interval metadata for this period
 * @return 0 on success or ENOENT if no valid entry for specified period
 * and instance
 */
int sfptpd_stats_collection_get_interval(struct sfptpd_stats_collection *stats,
					 enum sfptpd_stats_time_period period,
					 enum sfptpd_stats_history_index instance,
					 struct sfptpd_stats_time_interval *interval);


/** Creates a ptp-node hash table
 * @return Returns pointer to hash table on success or NULL on failure
 */
struct sfptpd_hash_table *sfptpd_stats_create_set(void);


/** Gets the first ptp-node in a ptp-node hash table
 * @param table Pointer to ptp-node hash table
 * @param iter Pointer to iterator to be initialised
 * @return Pointer to ptp-node on success, NULL if table empty
 */
struct sfptpd_stats_ptp_node *sfptpd_stats_node_ht_get_first(struct sfptpd_hash_table *table,
							     struct sfptpd_ht_iter *iter);


/** Gets the next ptp-node in a ptp-node hash table
 * @param iter Pointer to iterator holding current position in table
 * @return Pointer to ptp-node on success, NULL if at end of table
 */
struct sfptpd_stats_ptp_node *sfptpd_stats_node_ht_get_next(struct sfptpd_ht_iter *iter);


/** Add a ptp-node to a ptp-node hash table
 * @param table Pointer to table to add node to
 * @param clock_id Clock ID of node
 * @param state True if node is master, false if slave
 * @param port_number Port number of node
 * @param domain_number Domain number of node
 * @param transport_address Numerical transport address of node
 * @return Ptp node created, NULL if unsuccessful
 */
int sfptpd_stats_add_node(struct sfptpd_hash_table *table,
			   unsigned char *clock_id,
			   bool state,
			   uint16_t port_number,
			   uint16_t domain_number,
			   const char *transport_address);


#endif /* _SFPTPD_STATISTICS_H */
