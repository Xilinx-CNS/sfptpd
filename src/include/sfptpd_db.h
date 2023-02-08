/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_DB_H
#define _SFPTPD_DB_H

#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>


/****************************************************************************
 * sfptpd database module
 *
 * Provides a generic in-memory database via independent 'table' objects.
 *
 * - A user-supplied descriptor defines the table structure.
 * - The records are fixed size.
 * - Any number of keys can be defined to enable searching and sorting.
 * - One form of store is available - a linked list.
 * - There are no indexes. Filtering and sorting are performed at query time.
 *
 ****************************************************************************/


/****************************************************************************
 * Constants
 ****************************************************************************/

#define SFPTPD_DB_SEL_END -1
#define SFPTPD_DB_SEL_ORDER_BY -2
#define SFPTPD_DB_SEL_NOT -3

enum sfptpd_db_store_type {
	STORE_LINKED_LIST,
	STORE_ARRAY,
	STORE_DEFAULT = STORE_ARRAY,
};

/* Macros for building a 'sort' function out of a 'search' function. An
 * expression is supplied for dereferencing the key from a record. */
#define SFPTPD_DB_SORT_FN_NAME(search_fn) search_fn ## _sort

#define SFPTPD_DB_SORT_FN(search_fn, rec_type, rec, expr)		\
	static int SFPTPD_DB_SORT_FN_NAME(search_fn)(const void *raw_a, const void *raw_b) { \
		const rec_type *rec = (rec_type *) raw_a;			\
		return search_fn(expr, raw_b);				\
	}

/* Macro for building a key field definition */
#define SFPTPD_DB_FIELD(name, enumeration, search_fn, print_fn) [enumeration] = { name, search_fn, SFPTPD_DB_SORT_FN_NAME(search_fn), print_fn },

/****************************************************************************
 * Structures and Types
 ****************************************************************************/

struct sfptpd_db_table;
struct sfptpd_db_query_result;

/* Defines a field in a table */
struct sfptpd_db_field {
	/* A name used for diagnostic purposes, e.g. diagnostic dumps of the table */
	char *name;

	/* A function to compare a key against a record; used for filtering */
	int (*compare_key)(const void *key_value, const void *record);

	/* A function to compare two records; used for sorting */
	int (*compare_record)(const void *rec_a, const void *rec_b);

	/* An optional function used to print a field value for diagnostic purposes */
	int (*snprint)(char *str, size_t size, int width, const void *record);
};

/* Defines the structure of a table. */
struct sfptpd_db_table_def {
	/* The number of defined fields */
	int num_fields;

	/* An array of defined fields. 'Defined fields' are parts of a record
	   that can be used by this module to perform filtering and sorting.
	   They do not need to be exhaustive of the fields in your data
	   structure and can be virtual as they are implemented by callbacks */
	struct sfptpd_db_field *fields;

	size_t record_size;
};

/* A reference to a record in a table. This can be used for updating
 * and deleting records. The record content can be dereferenced via a
 * function call. */
struct sfptpd_db_record_ref {
	struct sfptpd_db_table *table;
	void *store_element;
	bool valid;
};

/* The result of a query including a list of pointers to matching records
 * in the chosen order. The object should be freed by the caller using
 * the function pointer provided. */
struct sfptpd_db_query_result {
	int num_records;
	void **record_ptrs;
	void (*free)(struct sfptpd_db_query_result *result);
};

/* The result of a query including a list of references for matching
 * references in the chosen order. The object should be freed by the
 * caller using the function pointer provided. */
struct sfptpd_db_query_result_refs {
	int num_records;
	struct sfptpd_db_record_ref *record_refs;
	void (*free)(struct sfptpd_db_query_result_refs *result);
};


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Create a database table
 * @param def The definition of the table structure
 * @param store_type The type of data structure to use to store records
 * @return The table object
 */
struct sfptpd_db_table *sfptpd_db_table_new(struct sfptpd_db_table_def *def,
					    enum sfptpd_db_store_type store_type);

/** Free a database table
 * @param table The table
 */
void sfptpd_db_table_free(struct sfptpd_db_table *table);

/** Insert a record into a database table
 * @param table The table
 * @param record A pointer to the record to be copied into the table
 * @return a record reference for the added record. Can be ignored or
 * used to perform a later update without looking up the record by keys.
 */
struct sfptpd_db_record_ref sfptpd_db_table_insert(struct sfptpd_db_table *table,
						   const void *record);

/** Delete records in a database table matching a set of user-supplied set of key-value pairs.
 * @param table The table
 * @param ... key-value pairs terminated by a key of SFPTPD_DB_SEL_END.
 */
void sfptpd_db_table_delete_impl(struct sfptpd_db_table *table, ...);

/* Safe version of the above with automatic parameter termination */
#define sfptpd_db_table_delete(...) sfptpd_db_table_delete_impl(__VA_ARGS__, SFPTPD_DB_SEL_END)


/** Find a record in a database table by a set of user-supplied set of key-value pairs.
 * @param table The table
 * @param ... key-value pairs terminated by a key of SFPTPD_DB_SEL_END.
 * @return a record reference for the added record.
 */
struct sfptpd_db_record_ref sfptpd_db_table_find_impl(struct sfptpd_db_table *table, ...);

/* Safe version of the above with automatic parameter termination */
#define sfptpd_db_table_find(...) sfptpd_db_table_find_impl(__VA_ARGS__, SFPTPD_DB_SEL_END)

/* Convenience version of the above retrieving the record data as an object */
#define sfptpd_db_table_get(dest, length, ...) record_get_data(table_find(__VA_ARGS__, SFPTPD_DB_SEL_END), dest, length)

/** Count the number of records in a database table matgching a
 * by a set of user-supplied set of key-value pairs.
 * @param table The table
 * @param ... key-value pairs terminated by a key of SFPTPD_DB_SEL_END.
 * @return the count of matching records.
 */
int sfptpd_db_table_count(struct sfptpd_db_table *table, ...);

/* Safe version of the above with automatic parameter termination */
#define sfptpd_db_table_count(...) sfptpd_db_table_count_impl(__VA_ARGS__, SFPTPD_DB_SEL_END)

/** Query a database table with given search and sort criteria.
 * @param table The table
 * @param ... key-value pairs optionally followed by a key value of SFPTPD_DB_SEL_ORDER_BY
 * and a list of sort keys and finally terminated by a key value of SFPTPD_DB_SEL_END.
 * @return a result structure including a an array of pointers to the resulting records.
 */
struct sfptpd_db_query_result sfptpd_db_table_query_impl(struct sfptpd_db_table *table, ...);

/* Safe version of the above with automatic parameter termination */
#define sfptpd_db_table_query(...) sfptpd_db_table_query_impl(__VA_ARGS__, SFPTPD_DB_SEL_END)


/** Execute a function for each record in a database table.
 * @param table The table
 * @param fn The function to call for each record
 * @param context private data for the callback
 * @param ... key-value pairs optionally followed by a key value of SFPTPD_DB_SEL_ORDER_BY
 * and a list of sort keys and finally terminated by a key value of SFPTPD_DB_SEL_END.
 */
void sfptpd_db_table_foreach_impl(struct sfptpd_db_table *table,
				  void (*fn)(void *record, void *context),
				  void *context, ...);

/* Safe version of the above with automatic parameter termination */
#define sfptpd_db_table_foreach(...) sfptpd_db_table_foreach_impl(__VA_ARGS__, SFPTPD_DB_SEL_END)


/** Diagnostic function to dump the known fields in a record.
 * Not to be used for user output.
 * @param trace_level The trace level to use
 * @param title A name for the table
 * @param with_payload Whether to show the raw record data
 * @param table The table
 * @param ... search and sort criteria as per query
 */
void sfptpd_db_table_dump_impl(int trace_level,
			       const char *title,
			       bool with_payload,
			       struct sfptpd_db_table *table, ...);

/* Safe version of the above with automatic paramater termination */
#define sfptpd_db_table_dump(...) sfptpd_db_table_dump_impl(__VA_ARGS__, SFPTPD_DB_SEL_END)


/** Copy the record identified in a record reference
 * @param record_ref The record reference
 * @param dest Pointer to where to copy the record
 * @param length The amount of space at 'dest' for sanity checking.
 */
void sfptpd_db_record_get_data(struct sfptpd_db_record_ref *record_ref,
			       void *dest, size_t length);

/* Checks whether a record reference points to record
 * @param record_ref The record reference
 * @return true if valid, else false.
 */
bool sfptpd_db_record_exists(struct sfptpd_db_record_ref *record_ref);

/* Update a record identified by a record reference.
 * @param record_ref The record reference
 * @param updated_values The updated record values
 */
void sfptpd_db_record_update(struct sfptpd_db_record_ref *record_ref,
			     const void *updated_values);

#endif /* _SFPTPD_DB_H */
