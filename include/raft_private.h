#ifndef RAFT_PRIVATE_H_
#define RAFT_PRIVATE_H_

#include "raft_log_cache.h"

/**
 * Copyright (c) 2013, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. 
 *
 * @file
 * @author Willem Thiart himself@willemthiart.com
 */

typedef struct {
    /* Persistent state: */

    /* the server's best guess of what the current term is
     * starts at zero */
    int current_term;

    /* The candidate the server voted for in its current term,
     * or Nil if it hasn't voted for any.  */
    int voted_for;

    /* the log which is replicated */
    void* log;

    /* Volatile state: */
  
    /* Log cache */
    log_cache_t *log_cache;
  
    /* idx of highest log entry known to be committed */
    int commit_idx;

    /* idx of highest log entry applied to state machine */
    int last_applied_idx;

    /* follower/leader/candidate indicator */
    int state;

    /* amount of time left till timeout */
    int timeout_elapsed;

    /* amount of time since last compaction */
    int last_compaction;

    /* Last compacted idx */
    int last_compacted_idx;

    /* Next compaction idx */
    int next_compaction_idx;
  
    raft_node_t* nodes;
    int num_nodes;

    int election_timeout;
    int request_timeout;
    int nack_timeout;
    int log_target;

    /* what this node thinks is the node ID of the current leader, or -1 if
     * there isn't a known current leader. */
    raft_node_t* current_leader;

    /* callbacks */
    raft_cbs_t cb;
    void* udata;

    /* my node ID */
    raft_node_t* node;

    /* the log which has a voting cfg change, otherwise -1 */
    int voting_cfg_change_log_idx;

    /* Image build in progress */
    volatile int img_build_in_progress;

    /* Flag to toggle client assist */ 
    volatile int client_assist;

    /* Flag to turn on multiple inflight requests */
    volatile int multi_inflight;

} raft_server_private_t;

void raft_election_start(raft_server_t* me);

void raft_become_candidate(raft_server_t* me);

void raft_become_follower(raft_server_t* me);

void raft_vote(raft_server_t* me, raft_node_t* node);

void raft_set_current_term(raft_server_t* me,int term);

int raft_get_img_build(raft_server_t * me_);

/**
 * @return 0 on error */
int raft_send_requestvote(raft_server_t* me, raft_node_t* node);

int raft_send_appendentries(raft_server_t* me, raft_node_t* node);

void raft_send_appendentries_all(raft_server_t* me_);

/**
 * Apply entry at lastApplied + 1. Entry becomes 'committed'.
 * @return 1 if entry committed, 0 otherwise */
int raft_apply_entry(raft_server_t* me_);

/**
 * Appends entry using the current term.
 * Note: we make the assumption that current term is up-to-date
 * @return 0 if unsuccessful */
int raft_append_entry(raft_server_t* me_, raft_entry_t* c, replicant_t *rep);

void raft_set_last_applied_idx(raft_server_t* me, int idx);

void raft_set_state(raft_server_t* me_, int state);

int raft_get_state(raft_server_t* me_);

raft_node_t* raft_node_new(void* udata, int id);

void raft_node_set_next_idx(raft_node_t* node, int nextIdx);

void raft_node_set_match_idx(raft_node_t* node, int matchIdx);

int raft_node_get_match_idx(raft_node_t* me_);

void raft_node_vote_for_me(raft_node_t* me_, const int vote);

int raft_node_has_vote_for_me(raft_node_t* me_);

void raft_node_set_has_sufficient_logs(raft_node_t* me_);

int raft_node_has_sufficient_logs(raft_node_t* me_);

int raft_votes_is_majority(const int nnodes, const int nvotes);

#endif /* RAFT_PRIVATE_H_ */
