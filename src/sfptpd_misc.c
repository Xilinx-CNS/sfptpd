/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_misc.c
 * @brief  Miscellaneous functions
 */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fts.h>
#include <fnmatch.h>
#include <linux/taskstats.h>
#include <execinfo.h>

#include "sfptpd_misc.h"
#include "sfptpd_logging.h"
#include "sfptpd_clock.h"

#define SFPTPD_HT_MAGIC (0xFACE85BE)


/** struct sfptpd_hash_table
 * Generic hash table
 * @ops: Pointer to set specific functions
 * @table_size: Size of hash table
 * @max_num_entries: Maximum number of entries in hash table
 * @num_entries: Current number of entries in hash table
 * @table_lock: Mutex lock for the table
 * @entries: Array of lists containing hash table contents
 */
typedef struct sfptpd_hash_table
{
	const struct sfptpd_ht_ops *ops;
	unsigned int table_size;
	unsigned int max_num_entries;
	unsigned int num_entries;
	pthread_mutex_t table_lock;
	struct sfptpd_ht_entry *entries[0];
} sfptpd_hash_table_t;


/****************************************************************************
 * Miscellaneous library functions
 ****************************************************************************/

char *sfptpd_strncpy(char *dest, const char *src, size_t n)
{
	char *r;
	assert(dest != NULL);
	assert(src != NULL);
	r = strncpy(dest, src, n);
	dest[n - 1] = '\0';
	return r;
}

int sfptpd_find_running_programs(struct sfptpd_prog *others)
{
	int count = 0;
	int mypid = getpid();
	FTS *fts;
	FTSENT *ftsent;
	struct sfptpd_prog *prog;
	char *proc_roots[] = {
		"/proc",
		NULL
	};

	/* Initialisation */
	for (prog = others; prog->pattern; prog++) {
		prog->matches = 0;
		prog->a_pid = 0;
	}

	/* procfs traversal */
	fts = fts_open(proc_roots, FTS_LOGICAL | FTS_NOSTAT | FTS_NOCHDIR, NULL);
	assert(fts);

	ftsent = fts_read(fts);
	assert(ftsent);

	ftsent = fts_children(fts, FTS_NAMEONLY);
	for (ftsent = fts_children(fts, FTS_NAMEONLY); ftsent; ftsent = ftsent->fts_link) {
		ssize_t res;
		char path[64]; /* Size for procfs paths */
		char exe[64]; /* Size for procfs paths */
		char *command;
		FILE *stream;
		char status;
		int i;

		/* Only look at the PID entries. */
		if ((ftsent->fts_info & FTS_DP) == 0)
			continue;
		for (i = 0; i < ftsent->fts_namelen &&
		            isdigit(ftsent->fts_name[i]); i++);

		if (i != ftsent->fts_namelen)
			continue;

		/* Check if it's actually running */
		res = snprintf(path, sizeof path, "%s/%s/stat",
			       ftsent->fts_path, ftsent->fts_name);
		assert((size_t) res < sizeof path);
		stream = fopen(path, "r");
		if (stream == NULL)
			goto next_process;
		res = fscanf(stream, "%*d %*s %c", &status);
		fclose(stream);
		if (res != 1)
			goto next_process;
		if (strchr("ZXx", status))
			goto next_process;

		/* Check what the exe link points to */
		res = snprintf(path, sizeof path, "%s/%s/exe",
			       ftsent->fts_path, ftsent->fts_name);
		assert((size_t) res < sizeof path);

		res = readlink(path, exe, sizeof exe - 1);
		if (res != -1) {
			exe[res] = '\0';
		} else if (errno == EACCES) {
			res = snprintf(path, sizeof path, "%s/%s/cmdline",
				       ftsent->fts_path, ftsent->fts_name);
			assert(res < (ssize_t) sizeof path);

			stream = fopen(path, "r");
			if (stream == NULL)
				goto next_process;
			res = fgets(exe, sizeof exe, stream) == NULL;
			fclose(stream);
			if (res)
				goto next_process;
		} else {
			goto next_process;
		}

		command = basename(exe);

		for (prog = others; prog->pattern; prog++) {
			if (fnmatch(prog->pattern, command, 0) == 0) {
				pid_t pid = atol(ftsent->fts_name);

				if (pid == mypid)
					goto next_process;

				prog->matches++;
				prog->a_pid = pid;
				snprintf(prog->a_program, sizeof prog->a_program,
					 "%s", command);

		                if (prog->matches == 1)
		                    count++;

				/* Only increment first match */
				goto next_process;
			}
		}

	next_process:
		/* On error, assume the process vanished. */
		continue;
	}

	fts_close(fts);

    return count;
}


void sfptpd_local_strftime(char *s, size_t max, const char *format, const sfptpd_secs_t *timep)
{
	struct tm tm;
	time_t tt = (time_t) *timep;
	struct tm *ltime = localtime_r(&tt, &tm);

	if (ltime != NULL) {
		strftime(s, max, format, ltime);
	} else {
		snprintf(s, max, "(invalid-time)");
	}
}


void sfptpd_debug_backtrace(void)
{
#define BACKTRACE_SIZE 128
	void **buffer = malloc(BACKTRACE_SIZE * sizeof(void *));
	char **strings;
	int len;

	if (!buffer)
		return;

	len = backtrace(buffer, BACKTRACE_SIZE);
	strings = backtrace_symbols(buffer, len);
	if (strings) {
		while(--len >= 1) {
			TRACE_L3("frame %d: %s\n", len, strings[len]);
		}
		free(strings);
	}

	free(buffer);
}


/****************************************************************************
 * Hash table functions
 ****************************************************************************/


static void sfptpd_ht_get_lock(struct sfptpd_hash_table *table)
{
	assert(table != NULL);

	int rc;

	rc = pthread_mutex_lock(&table->table_lock);
	if (rc != 0) {
		ERROR("failed to lock PTP set mutex, %d\n", rc);
	}
}

static void sfptpd_ht_release_lock(struct sfptpd_hash_table *table)
{
	assert(table != NULL);

	(void)pthread_mutex_unlock(&table->table_lock);
}


static unsigned int hash(sfptpd_hash_table_t *table, void *key,
			 unsigned int key_size)
{
	assert(table != NULL);
	assert(key != NULL);

        unsigned int hashval = 0;
        for(unsigned index = 0; index < key_size; index++) {
                hashval = ((unsigned char *)key)[index] + (hashval << 5) - hashval;
        }
        return hashval % table->table_size;
}


static struct sfptpd_ht_entry *sfptpd_ht_find(struct sfptpd_hash_table *table,
		     void *key, unsigned int key_length)
{
	assert(table != NULL);
	assert(key != NULL);

	struct sfptpd_ht_entry *entry;
	void *comp_key;
        unsigned int comp_key_length, hashval = hash(table, key, key_length);

        for(entry = table->entries[hashval]; entry != NULL; entry = entry->next) {
		assert(entry->magic == SFPTPD_HT_MAGIC);
		table->ops->get_key(entry->user, &comp_key, &comp_key_length);
		if (comp_key_length == key_length) {
			if (memcmp(key, comp_key, key_length) == 0) {
				return entry;
			}
                }
        }
        return NULL;
}


struct sfptpd_hash_table *sfptpd_ht_alloc(const struct sfptpd_ht_ops *ops,
					  unsigned int table_size,
					  unsigned int max_num_entries)
{
	assert(ops != NULL);
	assert(table_size < SFPTPD_HT_MAX_TABLE_SIZE);
	assert(max_num_entries < SFPTPD_HT_MAX_TABLE_ENTRIES);

        struct sfptpd_hash_table *new_table;
	int rc;

	new_table = (struct sfptpd_hash_table *)calloc(1, (sizeof(*new_table) + 
						       sizeof(new_table->entries[0]) * table_size));
	if (new_table == NULL) {
		ERROR("Insufficient memory to allocate hash table.");
		return NULL;
	}

	new_table->table_size = table_size;
	new_table->num_entries = 0;
	new_table->max_num_entries = max_num_entries;
	new_table->ops = ops;

	rc = pthread_mutex_init(&(new_table->table_lock), NULL);
	if (rc != 0) {
		CRITICAL("failed to create hash table lock, %s\n", strerror(rc));
		free(new_table);
		return NULL;
	}

        return new_table;
}


void sfptpd_ht_free(struct sfptpd_hash_table *table)
{
        sfptpd_ht_entry_t *entry, *copy;
	const struct sfptpd_ht_ops *ops = table->ops;

        assert(table != NULL);

        for (unsigned ii = 0; ii < table->table_size; ii++) {
                entry = table->entries[ii];
		while(entry != NULL) {
			assert(entry->magic == SFPTPD_HT_MAGIC);
			copy = entry;
			entry = entry->next;
			ops->free(copy->user);
			free(copy);
                }
        }

	(void)pthread_mutex_destroy(&table->table_lock);

        free(table);
}


int sfptpd_ht_add(struct sfptpd_hash_table *table,
		  void *user, bool update)
{
	assert(table != NULL);
	assert(user != NULL);

	const struct sfptpd_ht_ops *ops = table->ops;
	sfptpd_ht_entry_t *new_entry;
	void *key = NULL;
	unsigned int key_length;

	sfptpd_ht_get_lock(table);

	ops->get_key(user, &key, &key_length);
	assert(key != NULL);

        unsigned int hashval = hash(table, key, key_length);

        /* Check whether item already exists */
	new_entry = sfptpd_ht_find(table, key, key_length);

	if (new_entry != NULL) {
		if (update) {
			ops->copy(new_entry->user, user);
			sfptpd_ht_release_lock(table);
			return 0;
		} else {
			sfptpd_ht_release_lock(table);
			return EEXIST;
		}
	}

	if (table->num_entries == table->max_num_entries) {
		TRACE_L3("Maximum number of ptp-nodes reached, discarding new node.");
		sfptpd_ht_release_lock(table);
		return ENOSPC;
	}

	new_entry = (sfptpd_ht_entry_t *)calloc(1, sizeof(*new_entry));

	if (new_entry == NULL) {
		ERROR("Insufficient memory to allocate hash_table entry");
		sfptpd_ht_release_lock(table);
		return ENOMEM;
	}
	
	new_entry->user = ops->alloc();
	ops->copy(new_entry->user, user);

	new_entry->magic = SFPTPD_HT_MAGIC;
	new_entry->next = NULL;

	/* TODO Entries should be added in an ordered fashion */
	if (table->entries[hashval] == NULL) {
		table->entries[hashval] = new_entry;
	} else {
		new_entry->next = table->entries[hashval];
		table->entries[hashval] = new_entry;
	}
	table->num_entries++;

	sfptpd_ht_release_lock(table);
	return 0;
}


void *sfptpd_ht_first(struct sfptpd_hash_table *table,
		      struct sfptpd_ht_iter *iter)
{
	assert(table != NULL);
	assert(iter != NULL);

	iter->table = table;
	iter->entry = NULL;

	sfptpd_ht_get_lock(table);
	for (iter->index = 0; iter->index < table->table_size; iter->index++) {
		iter->entry = table->entries[iter->index];
		if (iter->entry != NULL) {
			assert(iter->entry->magic == SFPTPD_HT_MAGIC);
			assert(iter->entry->user != NULL);
			return iter->entry->user;
		}
	}

	sfptpd_ht_release_lock(table);
	return NULL;
}


void *sfptpd_ht_next(struct sfptpd_ht_iter *iter)
{
	struct sfptpd_hash_table *table = iter->table;

	assert(iter != NULL);
	assert(iter->table != NULL);

	if (iter->entry == NULL) {
		sfptpd_ht_release_lock(table);
		return NULL;
	}

	if (iter->entry->next != NULL) {
		assert (iter->entry->next != iter->entry);
		iter->entry = iter->entry->next;
		assert(iter->entry->user != NULL);
		return iter->entry->user;
	}

	for (iter->index++; iter->index < table->table_size; iter->index++) {
		if (table->entries[iter->index] != NULL) {
			iter->entry = table->entries[iter->index];
			assert(iter->entry->magic == SFPTPD_HT_MAGIC);
			assert(iter->entry->user != NULL);
			return iter->entry->user;
		}
	}

	sfptpd_ht_release_lock(table);
	return NULL;
}


void sfptpd_ht_clear_entries(struct sfptpd_hash_table *table)
{
	assert(table != NULL);

	struct sfptpd_ht_entry *entry, *temp;
	unsigned int index;
	void *user;

	sfptpd_ht_get_lock(table);

	for (index = 0; index < table->table_size; index++) {
		entry = table->entries[index];
		while (entry != NULL) {
			temp = entry;
			entry = temp->next;
			user = temp->user;
			free(temp);
			table->ops->free(user);
		}
		table->entries[index] = NULL;
	}
	table->num_entries = 0;
	sfptpd_ht_release_lock(table);
}


int sfptpd_ht_get_max_num_entries(struct sfptpd_hash_table *table)
{
	assert(table != NULL);
	return table->max_num_entries;
}

int sfptpd_ht_get_num_entries(struct sfptpd_hash_table *table)
{
	assert(table != NULL);
	return table->num_entries;
}


/****************************************************************************
 * String formatting functions
 ****************************************************************************/

ssize_t sfptpd_format(const struct sfptpd_interpolation *interpolators, void *context,
		      char *buffer, const size_t capacity, const char *format)
{
	const struct sfptpd_interpolation *spec;
	enum { IDLE, FMT } state = IDLE;
	const char *f = format;
	const ssize_t space = capacity;
	ssize_t ret = 0;
	ssize_t len = 0;
	char c;

	assert(space >= 0);
	assert(format != NULL);
	assert(buffer != NULL || space == 0);

	errno = EINVAL;
	while ((c = *format++)) {
		char opt;

		switch (state) {
		case IDLE:
			if (c == '%') {
				state = FMT;
			} else {
				if (buffer && len + 1 < space)
					buffer[len] = c;
				len++;
			}
			break;
		case FMT:
			/* Handle quoted '%' */
			if (c == '%') {
				if (buffer && len + 1 < space)
					buffer[len] = c;
				len++;
				state = IDLE;
				break;
			}

			/* Look up interpolator */
			for (spec = interpolators;
			     (spec->id != SFPTPD_INTERPOLATORS_END &&
			     c != spec->specifier);
			     spec++);

			/* Error in invalid specifier */
			if (spec->id == SFPTPD_INTERPOLATORS_END) {
				ERROR("format: unknown interpolator '%c'\n", c);
				ret = -1;
				goto error;
			}
			assert(spec->writer);

			/* Handle option character following specifier */
			opt = '\0';
			if (spec->has_opt) {
				opt = *format++;
				if (opt == '\0') {
					ERROR("format: missing option from "
					      "interpolator at end of format "
					      "string: %s\n", f);
					ret = -1;
					goto error;
				}
			}

			/* Invoke interpolator */
			ret = spec->writer(buffer ? buffer + len : NULL,
					   space != 0 ? space - len : 0,
					   spec->id, context, opt);
			if (ret < 0) {
				goto error;
			}
			len += ret;
			state = IDLE;
			break;
		}
	}

error:
	if (state != IDLE && ret != -1)
		ret = -1;

	if (buffer && space > 0) {
		if (len < space)
			buffer[len] = '\0';
		else
			buffer[space-1] = '\0';
	}

	if (ret >= 0) {
		errno = 0;
		return len;
	} else {
		return -1;
	}
}

int sfptpd_open_dirf(const char *fmt, ...)
{
	va_list ap;
	char *path;
	int rc;
	int e = 0;

	va_start(ap, fmt);
	if ((rc = vasprintf(&path, fmt, ap)) == -1) {
		e = errno;
		CRITICAL("formatting path %s, %s\n", fmt, strerror(e));
		errno = e;
		return rc;
	}
	va_end(ap);

	rc = open(path, O_PATH);
	if (rc == -1) {
		e = errno;
		ERROR("opening directory %s, %s\n",
		      fmt, strerror(e));
	}

	free(path);
	errno = e;
	return rc;
}

/* Read an integer from a file descriptor.
 * This is meant for when the whole file only contains an integer.
 * No assumption can be made about whether any extra bytes have been consumed.
 * Any whitespace character or EOF will be treated as a delimiter.
 */
int sfptpd_read_int_from_fd(int fd, long long *answer)
{
	enum {
		B_DEC,
		B_HEX,
		B_AUTO,
		B_PREFIX,
	} base = B_AUTO;
	long long num = 0, prev;
	bool negative = false;
	char buf[16];
	int sz;

	while((sz = read(fd, buf, sizeof buf)) != 0) {
		if (sz == -1 && errno != EINTR)
			return errno;
		for (int ptr = 0; ptr < sz; ptr++) {
			char c = buf[ptr];
			prev = num;
			if (isspace(c))
				break;
			else if (base == B_AUTO) {
				if (c == '-')
					negative = !negative;
				else if (c == '0')
					base = B_PREFIX;
				else if (isdigit(c))
					base = B_DEC;
				else
					return EINVAL;
			} else if (base == B_PREFIX) {
				if (c == 'x' || c == 'X') {
					base = B_HEX;
					continue;
				} else
					return EINVAL;
			}
			if (base == B_HEX && isdigit(c))
				num = prev * 16 + c - '0';
			else if (base == B_HEX && isxdigit(c))
				num = prev * 16 + c - 'a' + 10;
			else if (base == B_DEC && isdigit(c))
				num = prev * 10 + c - '0';
			else if (base != B_PREFIX)
				return EINVAL;
			if (num < prev)
				return ERANGE;
		}
	}
	*answer = negative ? -num : num;
	return 0;
}

int sfptpd_read_int_from_fileat(int dir_fd, const char *filename, long long *answer)
{
	int fd;
	int rc;

	if ((fd = openat(dir_fd, filename, O_RDONLY)) == -1) {
		rc = errno;
		TRACE_L1("failed to open file %s, %s\n", filename, strerror(rc));
		return rc;
	}

	rc = sfptpd_read_int_from_fd(fd, answer);
	if (rc != 0)
		ERROR("failed to read number from %s, %s\n", filename, strerror(rc));

	close(fd);
	return rc;
}

char *sfptpd_bitset_format(sfptpd_bitset_t set, const char** strings, unsigned int max)
{
	const char *unknown_fmt = "?%d ";
	sfptpd_bitset_t tst = set;
	ssize_t len = 0;
	char *buf, *ptr;
	int rc;

	while (tst) {
		unsigned int key = __builtin_ctz(tst);
		if (key < max && strings[key]) {
			len += strlen(strings[key]) + 1;
		} else {
			if ((rc = snprintf(NULL, 0, unknown_fmt, key)) == -1)
				return NULL;
			len += rc;
		}
		tst &= ~(1UL << key);
	}

	if ((ptr = buf = malloc(len + 1)) == NULL)
		return buf;

	while (set) {
		unsigned int key = __builtin_ctz(set);
		assert(len >= 0);
		if (key < max && strings[key]) {
			rc = snprintf(ptr, len, "%s ", strings[key]);
		} else {
			rc = snprintf(ptr, len, unknown_fmt, key);
		}
		if (rc == -1) goto fail;
		len -= rc;
		ptr += rc;
		set &= ~(1UL << key);
	}

	/* Remove trailing space if bits were set */
	ptr[ptr == buf ? 0 : -1] = '\0';

	return buf;
fail:
	free(buf);
	return NULL;
}

/* fin */
