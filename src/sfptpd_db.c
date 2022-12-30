/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_db.c
 * @brief  Database
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <limits.h>

#include "sfptpd_sync_module.h"
#include "sfptpd_ptp_module.h"
#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_clock.h"
#include "sfptpd_statistics.h"
#include "sfptpd_engine.h"
#include "sfptpd_interface.h"
#include "sfptpd_misc.h"
#include "sfptpd_thread.h"
#include "sfptpd_pps_module.h"
#include "sfptpd_db.h"

#include "ptpd_lib.h"


/****************************************************************************
 * Constants
 ****************************************************************************/

#define MAX_FIELDS 10
#define ARRAY_INITIAL_SIZE_BYTES 4096

const static uint32_t MAGIC_TABLE = 0xf74931e2;
const static uint32_t MAGIC_LL_HDR = 0x40e84c00;
const static uint32_t MAGIC_AR_HDR = 0x40e84c01;


/****************************************************************************
 * Types
 ****************************************************************************/

struct store;
struct linked_list_header;

struct selection {
	/* Parameters for filtering records (WHERE) */
	int filter_count;
	int filter_fields [MAX_FIELDS];
	void *filter_values [MAX_FIELDS];

	/* Parameters for sorting result (ORDER BY) */
	int sort_count;
	int sort_fields[MAX_FIELDS];
};

struct store_ops {
	struct sfptpd_db_record_ref (*insert)(struct sfptpd_db_table *table, const void *record);
	void (*delete)(struct sfptpd_db_record_ref *record_ref);
	void (*free)(struct store *store);
	struct sfptpd_db_record_ref (*find)(struct sfptpd_db_table *table, struct selection *selection);
	void *(*get_data)(struct sfptpd_db_record_ref *ref);
	void (*foreach)(struct sfptpd_db_table *table,
			void (*fn)(struct sfptpd_db_record_ref record_ref, void *context),
			void *context);
};

struct store {
	struct store_ops *ops;
	size_t record_size;
};

struct sfptpd_db_table {
	uint32_t magic;
	struct sfptpd_db_table_def *def;
	struct store *store;
};

struct linked_list_header {
	uint32_t magic;
	struct linked_list_header *next;
};

struct linked_list {
	struct store base;
	struct linked_list_header *head;
};

struct array_header {
	uint32_t magic;
	bool populated;
	uintptr_t next_freed;
};

struct array {
	struct store base;        /*!< Must be first field in struct */
	size_t capacity;          /*!< Number of elements allocated */
	size_t hwm;               /*!< High Water Mark of elements used */
	size_t count;             /*!< Number of elements currently populated */
	uintptr_t first_freed;    /*!< Index of first freed element to reuse */
	size_t stride;            /*!< Bytes between start of adjacent elements */
	void *data;               /*!< Array of capacity*stride bytes */
};


/****************************************************************************
 * Function prototypes
 ****************************************************************************/


/* The linked-list store is a singly-linked list of headers which are
 * immediately followed by the client-supplied record */

static struct sfptpd_db_record_ref linked_list_insert(struct sfptpd_db_table *table,
						      const void *record);

static void linked_list_delete(struct sfptpd_db_record_ref *record_ref);

static void linked_list_free(struct store *store);

static struct sfptpd_db_record_ref linked_list_find(struct sfptpd_db_table *table,
						    struct selection *selection);

static void *linked_list_get_data(struct sfptpd_db_record_ref *ref);

static void linked_list_foreach(struct sfptpd_db_table *table,
				void (*fn)(struct sfptpd_db_record_ref record, void *context),
				void *context);


/* The array store is an array with the following properties:
 *  - each element is a header followed by the client-supplied record
 *  - capacity doubles when exceeded
 *  - flag indicates if entry is populated
 *  - indexes are used to refer to elements
 *  - there is a linked list of freed elements
 *  - counts indicate if linked list is empty so there is no terminator
 */

static struct sfptpd_db_record_ref array_insert(struct sfptpd_db_table *table,
						const void *record);

static void array_delete(struct sfptpd_db_record_ref *record_ref);

static void array_free(struct store *store);

static struct sfptpd_db_record_ref array_find(struct sfptpd_db_table *table,
					      struct selection *selection);

static void *array_get_data(struct sfptpd_db_record_ref *ref);

static void array_foreach(struct sfptpd_db_table *table,
			  void (*fn)(struct sfptpd_db_record_ref record, void *context),
			  void *context);


/****************************************************************************
 * Constants
 ****************************************************************************/

struct store_ops linked_list_ops = {
	.insert = linked_list_insert,
	.delete = linked_list_delete,
	.free = linked_list_free,
	.find = linked_list_find,
	.get_data = linked_list_get_data,
	.foreach = linked_list_foreach,
};


struct store_ops array_ops = {
	.insert = array_insert,
	.delete = array_delete,
	.free = array_free,
	.find = array_find,
	.get_data = array_get_data,
	.foreach = array_foreach,
};


/****************************************************************************
 * Internal Functions
 ****************************************************************************/


static bool check_selection_matches(struct sfptpd_db_table *table,
				    struct selection *selection,
				    void *record)
{
	int key;

	assert(table != NULL);
	assert(selection != NULL);
	assert(record != NULL);
	assert(table->magic == MAGIC_TABLE);

	/* The value of every key field must match for success */
	for (key = 0; key < selection->filter_count; key ++) {
		int filter_field = selection->filter_fields[key];
		void *filter_value = selection->filter_values[key];

		if (table->def->fields[filter_field].compare_key(filter_value, record) != 0)
			break;
	}
	return key == selection->filter_count;
}


static void build_selection_params(struct selection *selection, va_list ap)
{
	int key;

	assert(selection != NULL);

	selection->filter_count = 0;
	selection->sort_count = 0;
	while (true) {
		key = va_arg(ap, int);
		if (key < 0)
			break;
		assert(selection->filter_count < MAX_FIELDS);
		selection->filter_fields[selection->filter_count] = key;
		selection->filter_values[selection->filter_count] = va_arg(ap, void *);
		selection->filter_count ++;
	}

	if (key == SFPTPD_DB_SEL_ORDER_BY) {
		while (true) {
			key = va_arg(ap, int);
			if (key == SFPTPD_DB_SEL_END)
				break;
			assert(selection->sort_count < MAX_FIELDS);
			selection->sort_fields[selection->sort_count] = key;
			selection->sort_count ++;
		}
	}
}


static void query_result_free(struct sfptpd_db_query_result *query_result)
{
	assert(query_result != NULL);

	free(query_result->record_ptrs);
}


static void query_result_refs_free(struct sfptpd_db_query_result_refs *query_result)
{
	assert(query_result != NULL);

	free(query_result->record_refs);
}


/* Implementation of linked-list store */

static struct store *linked_list_create(void)
{
	struct linked_list *new = calloc(1, sizeof *new);
	new->base.ops = &linked_list_ops;
	return &new->base;
}


static struct sfptpd_db_record_ref linked_list_insert(struct sfptpd_db_table *table,
						      const void *record)
{
	struct linked_list *ll = (struct linked_list *) table->store;
	struct linked_list_header *header;

	assert(table != NULL);
	assert(record != NULL);
	assert(table->magic == MAGIC_TABLE);
	assert(ll->head == NULL || ll->head->magic == MAGIC_LL_HDR);

	header = malloc(sizeof *header + ll->base.record_size);
	header->magic = MAGIC_LL_HDR;
	header->next = ll->head;
	memcpy(header + 1, record, ll->base.record_size);
	ll->head = header;

	return (struct sfptpd_db_record_ref) { .table = table, .store_element = header, .valid = true };
}


static void linked_list_delete(struct sfptpd_db_record_ref *record)
{
	struct linked_list *ll;
	struct linked_list_header **ptr;

	assert(record != NULL);
	assert(record->valid);
	assert(record->table != NULL);
	assert(record->table->magic == MAGIC_TABLE);

	ll = (struct linked_list *) record->table->store;

	record->valid = false;

	/* This is typically O(n) for a single record but will turn out to be O(n)
	   for a whole table if we delete it from head to tail. */

	/* Find the link to the record */
	for (ptr = &ll->head; *ptr != NULL; ptr = &(*ptr)->next) {
		assert((*ptr)->magic == MAGIC_LL_HDR);

		if (*ptr == record->store_element)
			break;
	}

	/* Delete the record */
	assert(*ptr != NULL);
	*ptr = (*ptr)->next;
	free(record->store_element);
}


static void linked_list_free(struct store *store)
{
	struct linked_list *ll = (struct linked_list *) store;

	assert(ll != NULL);
	assert(ll->head == NULL);
	free(ll);
}


static void *linked_list_get_data(struct sfptpd_db_record_ref *ref)
{
	assert(ref != NULL);
	assert(ref->valid);

	/* Return a pointer to the payload that follows the next pointer */
	return ((struct linked_list_header *) ref->store_element) + 1;
}


static void linked_list_foreach(struct sfptpd_db_table *table,
				void (*fn)(struct sfptpd_db_record_ref record_ref, void *context),
				void *context)
{
	struct linked_list *ll;
	struct linked_list_header *ptr;
	struct sfptpd_db_record_ref ref = {
		.table = table,
		.valid = true
	};

	assert(table != NULL);
	assert(fn != NULL);
	assert(table->magic == MAGIC_TABLE);

	ll = (struct linked_list *) table->store;

	/* Iterate through linked list */
	for (ptr = ll->head; ptr != NULL; ptr = ptr->next) {
		assert(ptr->magic == MAGIC_LL_HDR);
		ref.store_element = ptr;
		fn(ref, context);
	}
}


static struct sfptpd_db_record_ref linked_list_find(struct sfptpd_db_table *table,
						    struct selection *selection)
{
	struct linked_list *ll;
	struct linked_list_header *ptr;
	struct sfptpd_db_record_ref ref = {
		.table = table
	};

	assert(table != NULL);
	assert(selection != NULL);
	assert(table->magic == MAGIC_TABLE);

	ll = (struct linked_list *) table->store;

	/* Iterate through linked list */
	for (ptr = ll->head; ptr != NULL; ptr = ptr->next) {
		assert(ptr->magic == MAGIC_LL_HDR);
		if (check_selection_matches(table, selection, ptr + 1))
			break;
	}

	if (ptr == NULL) {
		ref.valid = false;
		ref.store_element = NULL;
	} else {
		ref.valid = true;
		ref.store_element = ptr;
	}

	return ref;
}


/* Implementation of array store */

static struct store *array_create(size_t initial_capacity, size_t record_size)
{
	struct array *new = calloc(1, sizeof *new);
	size_t alignment;
	size_t element_size;

	assert(new);

	alignment = __alignof__(struct array_header);
	element_size = sizeof(struct array_header) + record_size;

	new->base.ops = &array_ops;
	new->capacity = initial_capacity;
	new->stride = alignment * ((element_size + alignment - 1)/alignment);
	new->data = calloc(initial_capacity, new->stride);
	assert(new->data);

	return &new->base;
}


static struct array_header *get_array_header(struct array *array, int index, bool is_new) {
	struct array_header *hdr;

	assert(array);
	assert(array->data);
	assert(index < array->hwm);

	hdr = (struct array_header *) (((uint8_t *) array->data) + index * array->stride);
	if (is_new) {
		hdr->magic = MAGIC_AR_HDR;
	} else {
		assert(hdr->magic == MAGIC_AR_HDR);
	}

	return hdr;
}


static struct sfptpd_db_record_ref array_insert(struct sfptpd_db_table *table,
						const void *record)
{
	struct array *ar = (struct array *) table->store;
	struct array_header *header;
	uintptr_t index;

	assert(table != NULL);
	assert(record != NULL);
	assert(table->magic == MAGIC_TABLE);
	assert(ar->hwm <= ar->capacity);

	if (ar->count == ar->hwm) {
		if (ar->count == ar->capacity) {
			ar->capacity *= 2;
			ar->data = realloc(ar->data, ar->capacity * ar->stride);
			assert(ar->data != NULL);
		}
		index = ar->hwm++;
		header = get_array_header(ar, index, true);
	} else {
		index = ar->first_freed;
		header = get_array_header(ar, index, false);
		assert(!header->populated);
		ar->first_freed = header->next_freed;
	}
	header->populated = true;
	ar->count++;

	memcpy(header + 1, record, ar->base.record_size);

	return (struct sfptpd_db_record_ref) { .table = table, .store_element = (void *) index, .valid = true };
}


static void array_delete(struct sfptpd_db_record_ref *record)
{
	struct array *ar;
	struct array_header *hdr;
	uintptr_t index;

	assert(record != NULL);
	assert(record->valid);
	assert(record->table != NULL);
	assert(record->table->magic == MAGIC_TABLE);

	ar = (struct array *) record->table->store;

	record->valid = false;

	/* Find the record */
	index = (uintptr_t) record->store_element;
	hdr = get_array_header(ar, index, false);

	/* Delete the record */
	assert(hdr != NULL);
	hdr->populated = false;
	ar->count--;

	if (ar->count == 0) {
		ar->hwm = 0;
	} else if (index + 1 == ar->hwm) {
		ar->hwm--;
	} else {
		hdr->next_freed = ar->first_freed;
		ar->first_freed = index;
	}
}


static void array_free(struct store *store)
{
	struct array *ar = (struct array *) store;

	assert(ar != NULL);
	assert(ar->data != NULL);

	free(ar->data);
	free(ar);
}


static void *array_get_data(struct sfptpd_db_record_ref *ref)
{
	assert(ref != NULL);
	assert(ref->valid);

	struct array *ar = (struct array *) ref->table->store;

	/* Return a pointer to the payload that follows the next pointer */
	return get_array_header(ar, (uintptr_t) ref->store_element, false) + 1;
}


static void array_foreach(struct sfptpd_db_table *table,
			  void (*fn)(struct sfptpd_db_record_ref record_ref, void *context),
			  void *context)
{
	struct array *ar;
	struct sfptpd_db_record_ref ref = {
		.table = table,
		.valid = true
	};
	uintptr_t index;
	int count;

	assert(table != NULL);
	assert(fn != NULL);
	assert(table->magic == MAGIC_TABLE);

	ar = (struct array *) table->store;
	assert(ar->hwm <= ar->capacity);

	/* Iterate across array */
	for (index = 0, count = 0; index < ar->hwm; index++) {
		if (get_array_header(ar, index, false)->populated) {
			ref.store_element = (void *) index;
			fn(ref, context);
			count++;
		}
	}

	assert(count == ar->count);
}


static struct sfptpd_db_record_ref array_find(struct sfptpd_db_table *table,
					      struct selection *selection)
{
	struct array *ar;
	struct array_header *hdr = NULL;
	struct sfptpd_db_record_ref ref = {
		.table = table
	};
	uintptr_t index;

	assert(table != NULL);
	assert(selection != NULL);
	assert(table->magic == MAGIC_TABLE);

	ar = (struct array *) table->store;

	assert(ar->hwm <= ar->capacity);

	/* Iterate across array */
	for (index = 0; index < ar->hwm; index++) {
		hdr = get_array_header(ar, index, false);
		if (hdr->populated &&
		    check_selection_matches(table, selection, hdr + 1))
			break;
		hdr = NULL;
	}

	if (hdr == NULL) {
		ref.valid = false;
		ref.store_element = 0;
	} else {
		ref.valid = true;
		ref.store_element = (void *) index;
	}

	return ref;
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct sfptpd_db_table *sfptpd_db_table_new(struct sfptpd_db_table_def *def,
					    enum sfptpd_db_store_type type)
{
	struct sfptpd_db_table *new;

	assert(def != NULL);

	new = calloc(1, sizeof *new);

	new->def = def;
	new->magic = MAGIC_TABLE;

	switch (type) {
	case STORE_LINKED_LIST:
		new->store = linked_list_create();
		break;
	case STORE_ARRAY:
		new->store = array_create(ARRAY_INITIAL_SIZE_BYTES / def->record_size, def->record_size);
		break;
	default:
		assert(0);
	}

	new->store->record_size = def->record_size;

	return new;
}

void sfptpd_db_table_free(struct sfptpd_db_table *table)
{
	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);

	sfptpd_db_table_delete(table);

	table->store->ops->free(table->store);

	free(table);
}

struct sfptpd_db_record_ref sfptpd_db_table_insert(struct sfptpd_db_table *table,
						   const void *record)
{
	assert(table != NULL);
	assert(record != NULL);
	assert(table->magic == MAGIC_TABLE);

	return table->store->ops->insert(table, record);
}


struct sfptpd_db_record_ref sfptpd_db_table_find_impl(struct sfptpd_db_table *table, ...)
{
	va_list ap;
	struct selection selection;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);

	/* Count the number of keys */
	va_start(ap, table);
	build_selection_params(&selection, ap);
	va_end(ap);

	return table->store->ops->find(table, &selection);
}
/* Context for the callback functions used in the count operation */
struct count_fn_context {
	struct sfptpd_db_table *table;
	int count;
	struct selection selection;
};

/* Executed per record, incrementing a count of each record that matches criteria */
static void count_fn(void *record, void *context) {
	struct count_fn_context *counter = context;

	if (check_selection_matches(counter->table,
				    &counter->selection,
				    record))
		counter->count++;
}

int sfptpd_db_table_count_impl(struct sfptpd_db_table *table, ...)
{
	struct count_fn_context counter;
	va_list ap;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);

	/* Build up the key-value list */
	va_start(ap, table);
	build_selection_params(&counter.selection, ap);
	va_end(ap);

	counter.table = table;
	counter.count = 0;

	sfptpd_db_table_foreach(table, count_fn, &counter);

	return counter.count;
}


/* Context for the callback functions used in the query operation */
struct query_fn_context {
	struct sfptpd_db_table *table;
	struct sfptpd_db_query_result result;
	struct sfptpd_db_query_result_refs result_refs;
	struct selection selection;
	int i;
};

/* Executed per record, incrementing a count of each record that matches criteria */
static void query_count_fn(void *record, void *raw_context) {
	struct query_fn_context *context = raw_context;

	if (check_selection_matches(context->table,
				    &context->selection,
				    record))
		context->result.num_records++;
}

/* Executed per record, copying pointers to each record that matches criteria */
static void query_select_fn(void *record, void *raw_context) {
	struct query_fn_context *context = raw_context;

	if (check_selection_matches(context->table,
				    &context->selection,
				    record))
		context->result.record_ptrs[context->i++] = record;
}

/* Record comparison function for qsort_r */
static int query_compare_fn(const void *raw_rec_a, const void *raw_rec_b, void *raw_context) {
	const void *const *rec_a = raw_rec_a, *const *rec_b = raw_rec_b;
	struct query_fn_context *context = raw_context;
	int cmp = 0;

	for (int key = 0; key < context->selection.sort_count; key++) {
		int sort_field_idx = context->selection.sort_fields[key];
		struct sfptpd_db_field *sort_field = &context->table->def->fields[sort_field_idx];

		assert(sort_field->compare_record != NULL);
		cmp = sort_field->compare_record(*rec_a, *rec_b);
		if (cmp != 0)
			break;
	}
	return cmp;
}

/* Internal function to perform a query with filtering and sorting.
 * @param table The table
 * @param selection The selection parameters */
static struct sfptpd_db_query_result table_query(struct sfptpd_db_table *table,
						 struct selection *selection)
{
	struct query_fn_context fn_context;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);

	fn_context.table = table;
	fn_context.result.num_records = 0;
	fn_context.selection = *selection;

	/* Count the number of matching records */
	sfptpd_db_table_foreach(table, query_count_fn, &fn_context);

	/* Allocate space for the result */
	fn_context.i = 0;
	fn_context.result.free = query_result_free;
	fn_context.result.record_ptrs = calloc(fn_context.result.num_records,
					       sizeof *fn_context.result.record_ptrs);

	/* Create a list of matching record pointers */
	sfptpd_db_table_foreach(table, query_select_fn, &fn_context);

	/* Sort the result */
	if (fn_context.selection.sort_count != 0) {
		qsort_r(fn_context.result.record_ptrs,
			fn_context.result.num_records,
			sizeof *fn_context.result.record_ptrs,
			query_compare_fn,
			&fn_context);
	}

	return fn_context.result;
}


struct sfptpd_db_query_result sfptpd_db_table_query_impl(struct sfptpd_db_table *table, ...)
{
	va_list ap;
	struct selection selection;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);

	/* Build up the key-value list */
	va_start(ap, table);
	build_selection_params(&selection, ap);
	va_end(ap);

	return table_query(table, &selection);
}




struct deref_context {
	void (*user_fn)(void *record, void *context);
	void *user_context;
};

/* Trampoline to get record data from a record reference before calling
   a user callback */
static void deref_fn(struct sfptpd_db_record_ref record_ref, void *context)
{
	struct deref_context *deref_context = context;

	assert(record_ref.valid);
	deref_context->user_fn(record_ref.table->store->ops->get_data(&record_ref),
			       deref_context->user_context);
}

void sfptpd_db_table_foreach_impl(struct sfptpd_db_table *table,
				  void (*fn)(void *record, void *context),
				  void *context, ...) {

	va_list ap;
	struct selection selection;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);

	/* Build up the key-value list */
	va_start(ap, context);
	build_selection_params(&selection, ap);
	va_end(ap);

	if (selection.filter_count == 0 && selection.sort_count == 0) {
		/* With no constraints we can just operate on every entry in any order.
		 * We operate on record references hence via deref_fn. */
		struct deref_context deref_context = {
			.user_fn = fn,
			.user_context = context,
		};

		table->store->ops->foreach(table, deref_fn, &deref_context);
	} else {
		/* With search or sort constraints we call the query function first
		 * and operate directly on record pointers */
		struct sfptpd_db_query_result result = table_query(table, &selection);

		for (int i = 0; i < result.num_records; i++)
			fn(result.record_ptrs[i], context);

		result.free(&result);
	}
}


/* Executed per record, copying pointers to each record that matches criteria */
static void query_refs_select_fn(struct sfptpd_db_record_ref ref, void *raw_context) {
	struct query_fn_context *context = raw_context;
	void *record = ref.table->store->ops->get_data(&ref);

	if (check_selection_matches(context->table,
				    &context->selection,
				    record))
		context->result_refs.record_refs[context->i++] = ref;
}


/* Internal function to perform a query with filtering that returns
 * an object with record references.
 * @param table The table
 * @param selection The selection parameters */
static struct sfptpd_db_query_result_refs table_query_refs(struct sfptpd_db_table *table,
							   struct selection *selection)
{
	struct query_fn_context fn_context;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);
	assert(selection != NULL);

	fn_context.table = table;
	fn_context.result.num_records = 0;
	fn_context.selection = *selection;

	/* Count the number of matching records */
	sfptpd_db_table_foreach(table, query_count_fn, &fn_context);
	fn_context.result_refs.num_records = fn_context.result.num_records;

	/* Allocate space for the result */
	fn_context.i = 0;
	fn_context.result_refs.free = query_result_refs_free;
	fn_context.result_refs.record_refs = calloc(fn_context.result_refs.num_records,
						    sizeof *fn_context.result_refs.record_refs);

	/* Create a list of matching record pointers */
	table->store->ops->foreach(table, query_refs_select_fn, &fn_context);

	/* Sort the result */
	if (fn_context.selection.sort_count != 0) {
		qsort_r(fn_context.result.record_ptrs,
			fn_context.result.num_records,
			sizeof *fn_context.result.record_ptrs,
			query_compare_fn,
			&fn_context);
	}

	/* TODO: sorting of the resulting refs not currently supported */
	assert (fn_context.selection.sort_count == 0);

	return fn_context.result_refs;
}


static void table_delete(struct sfptpd_db_table *table,
			 struct selection *selection)
{
	struct sfptpd_db_query_result_refs result;
	int i;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);
	assert(selection != NULL);

	result = table_query_refs(table, selection);

	for (i = 0; i < result.num_records; i++) {
		struct sfptpd_db_record_ref *ref = &result.record_refs[i];

		ref->table->store->ops->delete(ref);
	}

	result.free(&result);
}


void sfptpd_db_table_delete_impl(struct sfptpd_db_table *table, ...)
{
	va_list ap;
	struct selection selection;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);

	/* Build up the key-value list */
	va_start(ap, table);
	build_selection_params(&selection, ap);
	va_end(ap);

	table_delete(table, &selection);
}


void sfptpd_db_table_dump_impl(int trace_level,
			       const char *title,
			       bool with_payload,
			       struct sfptpd_db_table *table, ...)
{
	struct sfptpd_db_query_result result;
	va_list ap;
	struct selection selection;
	int num_cols;
	int row;
	int key;
	const char *unknown_value = "?";
	struct col {
		int key;
		int width;
		const char *title;
	} cols[MAX_FIELDS + 1];
	struct col *col;
	struct col *col_end;
	int width;
	char *str;
	size_t sz;
	int rc;
	int all_cols_width = 0;
	int i;

	assert(table != NULL);
	assert(table->magic == MAGIC_TABLE);

	/* Build up the key-value list */
	va_start(ap, table);
	build_selection_params(&selection, ap);
	va_end(ap);

	sfptpd_log_trace(SFPTPD_COMPONENT_ID_SFPTPD, trace_level, "dump of table %s, %d search keys, %d sort keys\n",
			 title,
			 selection.filter_count,
			 selection.sort_count);

	result = table_query(table, &selection);

	num_cols = table->def->num_fields + 1;

	/* Dimension the output */
	for (key = 0, col = &cols[0]; key < table->def->num_fields; key++, col++) {
		struct sfptpd_db_field *field = &table->def->fields[key];

		col->title = field->name;
		assert(col->title != NULL);
		col->width = strlen(col->title);
		col->key = key;

		if (field->snprint == NULL) {
			if (col->width < strlen(unknown_value))
			    col->width = strlen(unknown_value);
		} else {
			for (row = 0; row < result.num_records; row++) {
				width = field->snprint(NULL, 0, 0, result.record_ptrs[row]);
				if (width > col->width)
					col->width = width;
			}
		}
	}

	/* Add a column for the payload if required */
	if (with_payload) {
		col->title = "record";
		col->width = strlen(col->title);
		col->key = -1;
		width = table->def->record_size * 2;
		if (width > col->width)
			col->width = width;
		col++;
	}

	col_end = col;
	num_cols = col_end - cols;

	for (col = cols; col < col_end; col++)
		all_cols_width += col->width;

	sz = all_cols_width + num_cols * 3 + 1 + 2;
	str = malloc(sz);
	assert(str != NULL);

	/* Print headings */
	i = 0;
	for (col = cols; col < col_end; col++) {
		rc = snprintf(str + i, sz - i, "| %*s ", col->width, col->title);
		assert(rc >= 0 && rc < sz - i);
		i += rc;
	}
	rc = snprintf(str + i, sz - i, "|");
	assert(rc >= 0 && rc < sz - i);
	sfptpd_log_trace(SFPTPD_COMPONENT_ID_SFPTPD, trace_level, "%s\n", str);

	/* Print rule */
	i = 0;
	for (col = cols; col < col_end; col++) {
		str[i++] = (col == cols) ? '|' : '+';
		for (int j = 0; j < col->width + 2; j++)
			str[i++] = '-';
	}
	str[i++] = '|';
	str[i++] = '\0';
	sfptpd_log_trace(SFPTPD_COMPONENT_ID_SFPTPD, trace_level, "%s\n", str);

	/* Print data */
	for (row = 0; row < result.num_records; row++) {
		i = 0;
		for (col = cols; col < col_end; col++) {
			struct sfptpd_db_field *field = &table->def->fields[col->key];
			int j;

			rc = snprintf(str + i, sz - i, "| ");
			assert(rc >= 0 && rc < sz - i);
			i += rc;

			if (col->key >= 0) {
				/* Is a key column */
				if (field->snprint == NULL) {
					rc = snprintf(str + i, sz - i, "%*s",
						      col->width,
						      unknown_value);
					assert(rc >= 0 && rc < sz - i);
					i += rc;
				} else {
					rc = field->snprint(str + i, col->width + 1, col->width, result.record_ptrs[row]);
					assert(rc >= 0 && rc <= col->width);
					i += rc;
					for (j = rc; j < col->width; j++)
						str[i++] = ' ';
				}
			} else {
				/* Is the payload column */
				for (j = 0; j < table->def->record_size; j++) {
					rc = snprintf(str + i, sz - i, "%02x",
						      ((uint8_t *) result.record_ptrs[row])[j]);
					assert(rc >= 0 && rc <= sz - i);
					i += rc;
				}
			}
			str[i++] = ' ';
		}
		rc = snprintf(str + i, sz - i, "|");
		assert(rc >= 0 && rc < sz - i);
		sfptpd_log_trace(SFPTPD_COMPONENT_ID_SFPTPD, trace_level, "%s\n", str);
	}

	free(str);
	query_result_free(&result);
}


bool sfptpd_db_record_exists(struct sfptpd_db_record_ref *ref)
{
	return ref->valid;
}


void sfptpd_db_record_get_data(struct sfptpd_db_record_ref *ref, void *dest, size_t length)
{
	const void *src;

	assert(ref != NULL);
	assert(dest != NULL);
	assert(length == ref->table->def->record_size);

	src = ref->table->store->ops->get_data(ref);
	memcpy(dest, src, length);
}


void sfptpd_db_record_update(struct sfptpd_db_record_ref *record_ref,
			     const void *updated_values)
{
	void *data;

	assert(record_ref->valid);

	data = record_ref->table->store->ops->get_data(record_ref);

	memcpy(data, updated_values, record_ref->table->def->record_size);
}


/* fin */
