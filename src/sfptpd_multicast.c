/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2023 Advanced Micro Devices, Inc. */

/**
 * @file   sfptpd_multicast.c
 * @brief  Routines for multicasting messages
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>

#include <sys/queue.h>

#include "sfptpd_logging.h"
#include "sfptpd_thread.h"
#include "sfptpd_message.h"
#include "sfptpd_multicast.h"


/****************************************************************************
 * Types
 ****************************************************************************/

struct multicast_user {

	/* Magic word */
	uint64_t magic;

	/* Thread of subscriber or publisher */
	struct sfptpd_thread *thread;

	/* Internal state for container implementation */
	SLIST_ENTRY(multicast_user) users;
};

SLIST_HEAD(users_list, multicast_user);

struct multicast_group {

	/* Magic word */
	uint64_t magic;

	/* Message id */
	uint32_t msg_id;

	/* List of subscribers to multicast group */
	struct users_list subscribers;

	/* List of publishers to multicast group */
	struct users_list publishers;

	/* Internal state for container implementation */
	SLIST_ENTRY(multicast_group) groups;
};

SLIST_HEAD(groups_list, multicast_group);

struct sfptpd_multicast {

	/* Magic word */
	uint64_t magic;

	/* Protect container integrity */
	pthread_mutex_t lock;

	/* List of multicast groups */
	struct groups_list groups;
};


/****************************************************************************
 * Constants
 ****************************************************************************/

#define MULTICAST_MAGIC		0x30171CA570000000ULL
#define MULTICAST_GROUP_MAGIC	0x30171CA570064009ULL
#define MULTICAST_USER_MAGIC	0x30171CA5700005E8ULL
#define MULTICAST_DELETED_MAGIC	0x30171CA57000DEADULL

#define MODULE			"multicast"
#define PREFIX			MODULE ": "

/****************************************************************************
 * Global variables
 ****************************************************************************/

static struct sfptpd_multicast *sfptpd_multicast = NULL;


/****************************************************************************
 * Private functions
 ****************************************************************************/

void multicast_dump_group(struct sfptpd_multicast *module, struct multicast_group *group, int sev)
{
	struct multicast_user *user;

	assert(group);
	assert(group->magic == MULTICAST_GROUP_MAGIC);

	pthread_mutex_lock(&module->lock);

	TRACE_LX(sev, PREFIX "- group\n");
	TRACE_LX(sev, PREFIX "   id: %x\n", group->msg_id);
	TRACE_LX(sev, PREFIX "   publishers:\n");
	SLIST_FOREACH(user, &group->publishers, users) {
		assert(user->magic == MULTICAST_USER_MAGIC);
		TRACE_LX(sev, PREFIX "    - %p %s\n", user->thread,
			 sfptpd_thread_get_name(user->thread));
	}
	TRACE_LX(sev, PREFIX "   subscribers:\n");
	SLIST_FOREACH(user, &group->subscribers, users) {
		assert(user->magic == MULTICAST_USER_MAGIC);
		TRACE_LX(sev, PREFIX "    - %p %s\n", user->thread,
			 sfptpd_thread_get_name(user->thread));
	}

	pthread_mutex_unlock(&module->lock);
}

void multicast_dump_groups(struct sfptpd_multicast *module, int sev)
{
	struct multicast_group *group;

	assert(module);
	assert(module->magic == MULTICAST_MAGIC);

	pthread_mutex_lock(&module->lock);

	TRACE_LX(sev, PREFIX "groups:\n");

	SLIST_FOREACH(group, &module->groups, groups)
		multicast_dump_group(module, group, sev);

	pthread_mutex_unlock(&module->lock);
}

int multicast_add_user(struct sfptpd_multicast *module,
		       struct sfptpd_thread *thread,
		       uint32_t msg_id, bool publisher)
{
	struct multicast_group *group;
	struct multicast_user *user;
	const char *action = publisher ? "publish" : "subscribe";
	int rc = 0;

	assert(module);
	assert(module->magic == MULTICAST_MAGIC);

	TRACE_L4(PREFIX "%s(%s, %x)\n", action, sfptpd_thread_get_name(thread), msg_id);

	user = calloc(1, sizeof *user);
	if (user == NULL)
		return errno;

	pthread_mutex_lock(&sfptpd_multicast->lock);

	SLIST_FOREACH(group, &sfptpd_multicast->groups, groups)
		if (group->msg_id == msg_id)
			break;

	if (group == NULL) {
		group = calloc(1, sizeof *group);
		if (group == NULL) {
			rc = errno;
			goto fail;
		}

		group->magic = MULTICAST_GROUP_MAGIC;
		group->msg_id = msg_id;
		SLIST_INSERT_HEAD(&sfptpd_multicast->groups, group, groups);
	}

	user->magic = MULTICAST_USER_MAGIC;
	user->thread = thread;
	SLIST_INSERT_HEAD(publisher ? &group->publishers : &group->subscribers,
			  user, users);

fail:
	multicast_dump_groups(sfptpd_multicast, 4);
	pthread_mutex_unlock(&sfptpd_multicast->lock);
	if (rc != 0) {
		user->magic = MULTICAST_DELETED_MAGIC;
		free(user);
	}
	return rc;
}

int multicast_remove_user(struct sfptpd_multicast *module,
			  struct sfptpd_thread *thread,
			  uint32_t msg_id, bool publisher)
{
	struct multicast_group *group;
	struct multicast_user *user;
	const char *action = publisher ? "unpublish" : "unsubscribe";

	int rc = 0;

	assert(module);
	assert(module->magic == MULTICAST_MAGIC);

	TRACE_L4(PREFIX "%s(%x, %p)\n", action, msg_id, thread);

	pthread_mutex_lock(&sfptpd_multicast->lock);

	SLIST_FOREACH(group, &sfptpd_multicast->groups, groups)
		if (group->msg_id == msg_id)
			break;

	if (group == NULL) {
		rc = ENOENT;
		goto fail;
	}

	assert(group->magic == MULTICAST_GROUP_MAGIC);

	SLIST_FOREACH(user, publisher ? &group->publishers : &group->subscribers, users) {
		assert(user->magic == MULTICAST_USER_MAGIC);
		if (user->thread == thread)
			break;
	}

	if (user == NULL) {
		rc = ENOENT;
		goto fail;
	}

	SLIST_REMOVE(publisher ? &group->publishers : &group->subscribers, user, multicast_user, users);
	user->magic = MULTICAST_DELETED_MAGIC;
	free(user);

fail:
	multicast_dump_groups(sfptpd_multicast, 4);

	if (group &&
	    SLIST_EMPTY(&group->publishers) &&
	    SLIST_EMPTY(&group->subscribers)) {
		TRACE_L4(PREFIX "removing unused group %x\n", group->msg_id);
		SLIST_REMOVE(&module->groups, group, multicast_group, groups);
		group->magic = MULTICAST_DELETED_MAGIC;
		free(group);
	}

	pthread_mutex_unlock(&sfptpd_multicast->lock);
	return rc;
}


/****************************************************************************
 * Public functions
 ****************************************************************************/

int sfptpd_multicast_init(void)
{
	pthread_mutexattr_t mattr;

	sfptpd_multicast = calloc(1, sizeof *sfptpd_multicast);
	if (sfptpd_multicast == NULL)
		return errno;

	sfptpd_multicast->magic = MULTICAST_MAGIC;
	SLIST_INIT(&sfptpd_multicast->groups);

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sfptpd_multicast->lock, &mattr);

	return 0;
}

void sfptpd_multicast_destroy(void)
{
	if (!sfptpd_multicast)
		return;

	assert(sfptpd_multicast->magic == MULTICAST_MAGIC);

	if (!SLIST_EMPTY(&sfptpd_multicast->groups)) {
		WARNING("multicast: not all multicast groups freed on exit\n");
		multicast_dump_groups(sfptpd_multicast, 3);
	} else {
		pthread_mutex_destroy(&sfptpd_multicast->lock);
		sfptpd_multicast->magic = MULTICAST_DELETED_MAGIC;
		free(sfptpd_multicast);
		sfptpd_multicast = NULL;
	}
}

void sfptpd_multicast_dump_state(void)
{
	if (!sfptpd_multicast)
		return;

	assert(sfptpd_multicast->magic == MULTICAST_MAGIC);

	multicast_dump_groups(sfptpd_multicast, 0);
}

int sfptpd_multicast_subscribe(uint32_t msg_id)
{
	return multicast_add_user(sfptpd_multicast, sfptpd_thread_self(), msg_id, false);
}

int sfptpd_multicast_publish(uint32_t msg_id)
{
	return multicast_add_user(sfptpd_multicast, sfptpd_thread_self(), msg_id, true);
}

int sfptpd_multicast_unsubscribe(uint32_t msg_id)
{
	return multicast_remove_user(sfptpd_multicast, sfptpd_thread_self(), msg_id, false);
}

int sfptpd_multicast_unpublish(uint32_t msg_id)
{
	return multicast_remove_user(sfptpd_multicast, sfptpd_thread_self(), msg_id, true);
}

int sfptpd_multicast_send(sfptpd_msg_hdr_t *hdr,
			  uint32_t msg_id,
			  enum sfptpd_msg_pool_id pool)
{
	struct multicast_group *group;
	struct multicast_user *user;
	struct {
		struct sfptpd_thread *thread;
		sfptpd_msg_hdr_t *msg;
	} *dests = NULL;
	int count = 0;
	int rc = 0;
	int i;

	assert(sfptpd_multicast);
	assert(sfptpd_multicast->magic == MULTICAST_MAGIC);

	SLIST_FOREACH(group, &sfptpd_multicast->groups, groups)
		if (group->msg_id == msg_id)
			break;

	if (group == NULL) {
		rc = ECONNREFUSED;
		goto fail;
	}
	assert(group->magic == MULTICAST_GROUP_MAGIC);

	pthread_mutex_lock(&sfptpd_multicast->lock);

	/* Count destinations */
	SLIST_FOREACH(user, &group->subscribers, users)
		count++;

	/* Allocate destinations */
	dests = calloc(count, sizeof *dests);
	i = 0;
	SLIST_FOREACH(user, &group->subscribers, users) {
		assert(user->magic == MULTICAST_USER_MAGIC);
		dests[i].thread = user->thread;
		dests[i].msg = sfptpd_msg_alloc(pool, true);
		if (dests[i++].msg == NULL) {
			rc = errno;
			break;
		}
	}
	assert(rc != 0 || i == count);
	if (rc != 0) {
		ERROR(PREFIX "failure to allocate replicated multicast message %d/%d, %s\n",
		      i, count, strerror(rc));
		while (i--)
			if (dests[i].msg != NULL)
				sfptpd_msg_free(dests[i].msg);
	}

	pthread_mutex_unlock(&sfptpd_multicast->lock);

	if (rc != 0)
		goto fail;

	/* Replicate message */
	for (i = 0; i < count; i++) {
		int ret;

		assert(sfptpd_msg_get_capacity(dests[i].msg) >= sfptpd_msg_get_capacity(hdr));
		memcpy(sfptpd_msg_get_payload(dests[i].msg),
		       sfptpd_msg_get_payload(hdr),
		       sfptpd_msg_get_capacity(hdr));
		TRACE_L6(PREFIX "sending group %x message to %s\n",
			 msg_id, sfptpd_thread_get_name(dests[i].thread));
		ret = sfptpd_msg_send(dests[i].msg, dests[i].thread,
				      msg_id, false);
		if (ret != 0 && rc == 0)
			rc = ret;
	}

fail:
	if (dests != NULL)
		free(dests);
	sfptpd_msg_free(hdr);
	return rc;
}

/* fin */
