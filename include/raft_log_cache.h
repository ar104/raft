#ifndef RAFT_LOG_CACHE_H_
#define RAFT_LOG_CACHE_H_
#include "raft.h"

typedef struct
{
  /* Current term of the leader */
  int leader_term;

  /* the amount of elements in the array */
  int count;
  
  /* window */
  int window_start;

  char *is_valid;
  replicant_t * entries;
  
} log_cache_t;

log_cache_t* log_cache_new();
void log_cache_clear(log_cache_t* me);
void log_cache_set_head(log_cache_t *me,
			int leader_term,
			int idx);
replicant_t* log_cache_get_next(log_cache_t *me,
				int prev_idx,
				int prev_term);


void log_cache_add(log_cache_t* me,
		   replicant_t *replicant);
int log_cache_contains(log_cache_t *me, int idx);

#endif
