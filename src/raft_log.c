/**
 * Copyright (c) 2013, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * @file
 * @brief ADT for managing Raft log entries (aka entries)
 * @author Willem Thiart himself@willemthiart.com
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "raft.h"
#include "raft_private.h"
#include "raft_log.h"

#define INITIAL_CAPACITY 10
#define in(x) ((log_private_t*)x)

typedef struct
{
    /* size of array */
    int size;

    /* the amount of elements in the array */
    int count;

    /* position of the queue */
    int front, back; // Absolute idx

    raft_entry_t* entries;

    /* callbacks */
    raft_cbs_t *cb;
    void* raft;
} log_private_t;

#define REL_POS(_i, _s) ((_i) % (_s))

static void __ensurecapacity(log_private_t * me)
{
    int i;
    raft_entry_t *temp;

    if (me->count < me->size)
        return;

    temp = (raft_entry_t*)calloc(1, sizeof(raft_entry_t) * me->size * 2);

    for (i = me->front; i < me->back; i++)
    {
      memcpy(&temp[REL_POS(i, 2*me->size)], 
	     &me->entries[REL_POS(i, me->size)], 
	     sizeof(raft_entry_t));
    }

    /* clean up old entries */
    free(me->entries);

    me->size *= 2;
    me->entries = temp;
}

void log_load_from_checkpoint(log_t *me_, int idx)
{
  log_private_t* me = (log_private_t*)me_;
  me->front = me->back = idx;
}

log_t* log_new()
{
    log_private_t* me = (log_private_t*)calloc(1, sizeof(log_private_t));
    if (!me)
        return NULL;
    me->size = INITIAL_CAPACITY;
    me->count = 0;
    me->back = in(me)->front = 0;
    me->entries = (raft_entry_t*)calloc(1, sizeof(raft_entry_t) * me->size);
    return (log_t*)me;
}

void log_set_callbacks(log_t* me_, raft_cbs_t* funcs, void* raft)
{
    log_private_t* me = (log_private_t*)me_;

    me->raft = raft;
    me->cb = funcs;
}

void log_clear(log_t* me_)
{
    log_private_t* me = (log_private_t*)me_;
    me->count = 0;
    me->back = 0;
    me->front = 0;
}

int log_append_entry(log_t* me_, raft_entry_t* c)
{
    log_private_t* me = (log_private_t*)me_;

    int retval = 0;

    if (0 == c->id)
        return -1;

    __ensurecapacity(me);

    if (me->cb && me->cb->log_offer)
        retval = me->cb->log_offer(me->raft, raft_get_udata(me->raft), c, me->back);


    memcpy(&me->entries[REL_POS(me->back, me->size)], c, sizeof(raft_entry_t));
    me->count++;
    me->back++;
    return retval;
}

raft_entry_t* log_get_from_idx(log_t* me_, int idx, int *n_etys)
{
    log_private_t* me = (log_private_t*)me_;
    int i, back;

    /* idx starts at 1 */
    assert(0 <= idx - 1);
    idx -= 1;

    if (me->front > idx || idx >= me->back)
    {
        *n_etys = 0;
        return NULL;
    }


    i = REL_POS(idx, me->size);
    back = REL_POS(me->back, me->size);

    int logs_till_end_of_log;

    if (i < back)
        logs_till_end_of_log = back - i;
    else
        logs_till_end_of_log = me->size - i;

    *n_etys = logs_till_end_of_log;
    return &me->entries[i];
}

raft_entry_t* log_get_at_idx(log_t* me_, int idx)
{
    log_private_t* me = (log_private_t*)me_;
    int i;

    /* idx starts at 1 */
    assert(0 <= idx - 1);
    idx -= 1;

    if (me->front > idx || idx >= me->back)
    {
      return NULL;
    }


    i = REL_POS(idx, me->size);
    return &me->entries[i];

}

int log_count(log_t* me_)
{
    return ((log_private_t*)me_)->count;
}

void log_delete(log_t* me_, int idx)
{
    log_private_t* me = (log_private_t*)me_;
    
    /* idx starts at 1 */
    idx -= 1;

    while(me->back != idx) {
      me->back--;
      if (me->cb && me->cb->log_pop)
	me->cb->log_pop(me->raft, raft_get_udata(me->raft),
			&me->entries[REL_POS(me->back, me->size)], 
			me->back);
      me->count--;
    }
}

void *log_poll(log_t * me_)
{
    log_private_t* me = (log_private_t*)me_;

    if (0 == log_count(me_))
        return NULL;

    void *elem = &me->entries[REL_POS(me->front, me->size)];
    if (me->cb && me->cb->log_poll)
        me->cb->log_poll(me->raft, 
			 raft_get_udata(me->raft),
			 elem,
			 me->front);
    me->front++;
    me->count--;
    return elem;
}

raft_entry_t *log_peektail(log_t * me_)
{
    log_private_t* me = (log_private_t*)me_;

    if (0 == log_count(me_))
        return NULL;

    return &me->entries[REL_POS(me->back - 1, me->size)];
}

void log_empty(log_t * me_)
{
    log_private_t* me = (log_private_t*)me_;

    me->front = 0;
    me->back = 0;
    me->count = 0;
}

void log_free(log_t * me_)
{
    log_private_t* me = (log_private_t*)me_;

    free(me->entries);
    free(me);
}

int log_get_current_idx(log_t* me_)
{
    log_private_t* me = (log_private_t*)me_;
    return me->back;
}
