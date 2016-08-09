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

static void __ensurecapacity_batch(log_private_t * me, int count)
{
    int i;
    raft_entry_t *temp;
   
    if ((me->count + count) <= me->size)
        return;

    while((me->count + count) > me->size) {
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
    raft_entry_t *prev;

    __ensurecapacity(me);

    if (me->cb && me->cb->log_offer) {
      if(me->back > 0) {
	prev = &me->entries[REL_POS(me->back - 1, me->size)];
      }
      else {
	prev = NULL;
      }
      
      retval = me->cb->log_offer(me->raft,
				 raft_get_udata(me->raft),
				 c,
				 prev,
				 me->back);
    }

    memcpy(&me->entries[REL_POS(me->back, me->size)], c, sizeof(raft_entry_t));
    me->count++;
    me->back++;
    return retval;
}

int log_append_batch(log_t* me_, raft_entry_t* c, int count)
{
    log_private_t* me = (log_private_t*)me_;
    int retval = 0;
    raft_entry_t *prev;
        
    __ensurecapacity_batch(me, count);

    if (me->cb && me->cb->log_offer_batch) {
      if(me->back > 0) {
	prev = &me->entries[REL_POS(me->back - 1, me->size)];
      }
      else {
	prev = NULL;
      }
      retval = me->cb->log_offer_batch(me->raft,
				       raft_get_udata(me->raft), c, prev,
				       me->back,
				       count);
    }

    if(REL_POS(me->back + count - 1, me->size) >= REL_POS(me->back, me->size)) {
      memcpy(&me->entries[REL_POS(me->back, me->size)], c, count*sizeof(raft_entry_t));
    }
    else {
      int part1 = me->size - REL_POS(me->back, me->size);
      memcpy(&me->entries[REL_POS(me->back, me->size)], c, part1*sizeof(raft_entry_t));
      memcpy(&me->entries[0], c + part1, (count - part1)*sizeof(raft_entry_t));
    }

    me->count += count;
    me->back  += count;
    return retval;
}

raft_entry_t* log_get_from_idx(log_t* me_, int idx, int *n_etys)
{
    log_private_t* me = (log_private_t*)me_;
    int i, back;

    /* idx starts at 1 */
    if(idx == 0) {
      *n_etys = 0;
      return NULL;
    }
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
    if(idx == 0) {
      return NULL;
    }
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

void log_poll_batch(log_t *me_, int cnt)
{
  log_private_t* me = (log_private_t*)me_;
  if(cnt > log_count(me_))
    cnt = log_count(me_);
  
  if (me->cb && me->cb->log_poll_batch) {
    if(REL_POS(me->front + cnt - 1, me->size) >= REL_POS(me->front, me->size)) {
      me->cb->log_poll_batch(me->raft, 
			     raft_get_udata(me->raft),
			     &me->entries[REL_POS(me->front, me->size)],
			     me->front,
			     cnt);
    }
    else {
      int part1 = me->size - REL_POS(me->front, me->size);
      me->cb->log_poll_batch(me->raft, 
			     raft_get_udata(me->raft),
			     &me->entries[REL_POS(me->front, me->size)],
			     me->front,
			     part1);
      me->cb->log_poll_batch(me->raft, 
			     raft_get_udata(me->raft),
			     &me->entries[0],
			     me->front + part1,
			     cnt - part1);
    }
  }
  me->front += cnt;
  me->count -= cnt;
}

int log_poll(log_t * me_)
{
    log_private_t* me = (log_private_t*)me_;
    int e = 0;

    if (0 == log_count(me_))
        return 0;

    void *elem = &me->entries[REL_POS(me->front, me->size)];

    if (me->cb && me->cb->log_poll)
      e = me->cb->log_poll(me->raft, 
			   raft_get_udata(me->raft),
			   elem,
			   me->front);

    me->front++;
    me->count--;
    return e;
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
