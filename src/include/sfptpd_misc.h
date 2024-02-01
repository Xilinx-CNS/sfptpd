/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

#ifndef _SFPTPD_MISC_H
#define _SFPTPD_MISC_H

#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <linux/taskstats.h>

#include "sfptpd_time.h"

#define SFPTPD_HT_MAX_TABLE_SIZE 	(0x100)
#define SFPTPD_HT_MAX_TABLE_ENTRIES 	(0x10000)

#define SFPTPD_INTERPOLATORS_END -1

/****************************************************************************
 * Static assert
 ****************************************************************************/

#ifndef STATIC_ASSERT
#define __STATIC_ASSERT_NAME(_x) __STATIC_ASSERT_ILOATHECPP(_x)
#define __STATIC_ASSERT_ILOATHECPP(_x)  __STATIC_ASSERT_AT_LINE_ ## _x
#define STATIC_ASSERT(e)\
 typedef char  __STATIC_ASSERT_NAME(__LINE__)[(e)?1:-1]
#endif


/****************************************************************************
 * Structures and Types
 ****************************************************************************/

typedef size_t (*sfptpd_interpolator_t)(char *buffer, size_t space, int id, void *context, char opt);

struct sfptpd_interpolation {
	int id;
	char specifier;
	bool has_opt;
	sfptpd_interpolator_t writer;
};

struct sfptpd_hash_table;


/** struct sfptpd_ht_entry
 * Hash table entry
 * @magic: Magic number used to validate entry
 * @next: Pointer to next hash_entry item in linked list. NULL if at end of list
 * @user: Pointer to struct containing set specific info about entry
 */
typedef struct sfptpd_ht_entry
{
	uint32_t magic;
	struct sfptpd_ht_entry *next;
	void *user;
} sfptpd_ht_entry_t;


 /** struct sfptpd_ht_ops
  * Operations for classes derived from the hash table base class
  * @alloc: Allocate a new user entry
  * @copy: Copy the contents of the second user entry into the first entry
  * @free: Free the user entry
  * @get_key: From a user entry, get a pointer to the key and the length
  */
typedef struct sfptpd_ht_ops
{
	void *(*alloc)(void);
	void (*copy)(void *, void *);
	void (*free)(void *);
	void (*get_key)(void *, void **, unsigned int *);
} sfptpd_ht_ops;


/** struct sfptpd_hash_table_iter
 * Hash table iterator
 * @table Pointer to hash table being iterated through
 * @index Current index in hash table
 * @entry Current entry in hash table
 */
typedef struct sfptpd_ht_iter
{
	struct sfptpd_hash_table *table;
	unsigned int index;
	struct sfptpd_ht_entry *entry;
} sfptpd_ht_iter_t;


/** struct sfptpd_prog
 * Running program to search for
 * @pattern The pattern to match
 * @matches Number of matching processes running
 * @a_program The name of a matching executable
 * @a_pid The PID of a running process if applicable
 */
struct sfptpd_prog {
	char *pattern;
	char a_program[TS_COMM_LEN + 1];
	int matches;
	pid_t a_pid;
};


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

/** Format a string with application-specific interpolations.
 * @param interpolators table of interpolation definitions
 * @param context context for interpolators (the object whose properties
 *		  are to be interpolated)
 * @param buffer as per snprintf
 * @param space as per snprintf
 * @param format the application-specific %-format to be interpolated
 * @return The number of bytes written or that would be written as per snprintf
 */
size_t sfptpd_format(const struct sfptpd_interpolation *interpolators, void *context,
		     char *buffer, size_t space, const char *format);

/** Safe version of strncpy. Places 0 termination at last character
 * @param dest Destination buffer
 * @param src Source string
 * @param n Maximum number of bytes to copy
 * @return A pointer to the destination buffer
 */
char *sfptpd_strncpy(char *dest, const char *src, size_t n);


/** Check if the named program is running
 * @param program_name Name of program
 * @return A boolean indicating if program is running
 */
bool sfptpd_is_program_running(const char *program_name);


/** Combines a call to localtime and strftime with error
 * checking for convenience and safety
 * @param s Buffer for output
 * @param max Size of buffer
 * @param format Format string as per strftime(3)
 * @param time_p The UTC time to format
 */
void sfptpd_local_strftime(char *s, size_t max, const char *format, const sfptpd_secs_t *timep);


/** Create a hash table
 * @param hash_size Size of the hash values to be used
 * @param table_size Size of the hash table to allocate
 * @return A pointer to the hash table or null if memory allocation failed
 */
struct sfptpd_hash_table *sfptpd_ht_alloc(const struct sfptpd_ht_ops *ops,
					  unsigned int table_size,
					  unsigned int max_num_entries);

/** Free a hash table and all the contents
 * @param table Pointer to hash table
 */
void sfptpd_ht_free(struct sfptpd_hash_table *table);

/** Add an entry to the hash table
 * @param table Pointer to hash table
 * @param new_entry Entry to be added
 * @return 0 on success or an errno on failure
 */
int sfptpd_ht_add(struct sfptpd_hash_table *table,
		  void *user, bool update);

/** Iterate through the hash table and find the first entry.
 * Gets lock for the table and releases the lock if the table
 * is empty, otherwise holds onto the lock.
 * @param table Pointer to hash table
 * @param iter Iterator to initialise
 * @return A pointer to the first item in the hash table or null if empty
 */
void *sfptpd_ht_first(struct sfptpd_hash_table *table,
		      struct sfptpd_ht_iter *iter);

/** Iterates through the hash table and finds the next entry.
 * Releases the lock if there are no further entries.
 * @param table Pointer to hash table
 * @param iter Iterator previously initialised with sfptpd_hash_table_first()
 * @return A pointer to the next item in the hash table or null if the end of
 * the table has been reached
 */
void *sfptpd_ht_next(struct sfptpd_ht_iter *iter);


/** Clears a hash table of entries
 * @param table Pointer to hash table
 */
void sfptpd_ht_clear_entries(struct sfptpd_hash_table *table);


/** Get the max num entries of a table
 * @param table Pointer to hash table
 * @return Max num entries of table
 */
int sfptpd_ht_get_max_num_entries(struct sfptpd_hash_table *table);


/** Get the number of entries in a table
 * @param table Pointer to hash table
 * @return Returns number of entries in table
 */
int sfptpd_ht_get_num_entries(struct sfptpd_hash_table *table);


/** Find running processes with matching names
 * @param others the programs to look for
 * @return updates the 'others' input
 */
int sfptpd_find_running_programs(struct sfptpd_prog *others);

#endif /* _SFPTPD_MISC_H */
