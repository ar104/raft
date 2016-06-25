/* Manage the RAFT log cache */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "raft.h"
#include "raft_private.h"
#include "raft_log_cache.h"

// This should be the maximum number of clients
#define CAPACITY 1000

#define REL_POS(_i) ((_i) % (CAPACITY))

log_cache_t* log_cache_new()
{
    log_cache_t* me = (log_cache_t*)calloc(1, sizeof(log_cache_t));
    if (!me)
        return NULL;
    me->leader_term  = 0;
    me->count        = 0;
    me->window_start = 0;
    memset(me->is_valid, 0, CAPACITY);
    me->entries = (replicant_t *)calloc(1, sizeof(replicant_t) * CAPACITY);
    me->is_valid = (char *)calloc(sizeof(char), CAPACITY);
    return (log_cache_t*)me;
}

void log_cache_clear(log_cache_t *me)
{
  int i;
  for(i=0;i<CAPACITY;i++) {
    if(me->is_valid[i]) {
      free(me->entries[i].ety.data.buf);
      me->is_valid[i] = 0;
    }
  }
}

void log_cache_set_head(log_cache_t *me,
			int leader_term,
			int idx)
{
  if(leader_term != me->leader_term) {
    log_cache_clear(me);
    me->leader_term = leader_term;
  }

  if(idx < me->window_start ||
     idx >= (me->window_start + CAPACITY)) {
    log_cache_clear(me);
    me->leader_term = leader_term;
    me->window_start = idx;
  }

  while(me->window_start != idx) {
    int pos = REL_POS(me->window_start);
    if(me->is_valid[pos]) {
      free(me->entries[pos].ety.data.buf);
      me->is_valid[pos] = 0;
    }
    me->window_start++;
  }
}

/* Note: caller should first set head */
replicant_t* log_cache_get_next(log_cache_t *me,
				int prev_idx,
				int prev_term)
{
  int pos = REL_POS(me->window_start);
  if(!me->is_valid[pos]) {
    return NULL;
  }
  replicant_t *e = &me->entries[pos];
  if(e->prev_idx != prev_idx || e->prev_term != prev_term) {
    return NULL;
  }
  me->window_start++;
  me->is_valid[pos] = 0;
  return e;
}

/* Note: caller should first set head 
 * and check log_cache_contains 
 */
void log_cache_add(log_cache_t* me,
		   replicant_t *rep)
{
  me->entries[REL_POS(rep->prev_idx + 1)]  = *rep;
  me->is_valid[REL_POS(rep->prev_idx + 1)] = 1;
}

int log_cache_contains(log_cache_t *me, int idx)
{
  return (!(me->window_start > idx)) &&
    (!((me->window_start + CAPACITY) <= idx));
}
