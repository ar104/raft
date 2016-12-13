/**
 * Copyright (c) 2013, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * @file
 * @brief Implementation of a Raft server
 * @author Willem Thiart himself@willemthiart.com
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* for varags */
#include <stdarg.h>

#include "raft.h"
#include "raft_log.h"
#include "raft_private.h"

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) ((a) < (b) ? (b) : (a))
#endif

static void __log(raft_server_t *me_, raft_node_t* node, const char *fmt, ...)
{
  raft_server_private_t* me = (raft_server_private_t*)me_;

  if(me->cb.log == NULL)
    return;
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsprintf(buf, fmt, args);

    me->cb.log(me_, node, me->udata, buf);
}

static void __log_election(raft_server_t *me_, raft_node_t* node, const char *fmt, ...)
{
  raft_server_private_t* me = (raft_server_private_t*)me_;
  if(me->cb.log_election == NULL)
    return;
  
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsprintf(buf, fmt, args);

    me->cb.log_election(me_, node, me->udata, buf);
}

raft_server_t* raft_new()
{
    raft_server_private_t* me =
        (raft_server_private_t*)calloc(1, sizeof(raft_server_private_t));
    if (!me)
        return NULL;
    me->current_term = 0;
    me->voted_for = -1;
    me->timeout_elapsed = 0;
    me->request_timeout = 200;
    me->log_target      = 10000;
    me->log_base        = 0;
    me->election_timeout = 1000;
    me->log = log_new();
    me->voting_cfg_change_log_idx = -1;
    raft_set_state((raft_server_t*)me, RAFT_STATE_FOLLOWER);
    me->current_leader = NULL;
    me->last_compacted_idx = me->last_applied_idx;
    me->multi_inflight = 0;
    me->preferred_leader = 0;
    return (raft_server_t*)me;
}

void raft_set_multi_inflight(raft_server_t *me_)
{
  raft_server_private_t* me = (raft_server_private_t*)me_;
  me->multi_inflight = 1;
}

void raft_unset_multi_inflight(raft_server_t *me_)
{
  raft_server_private_t* me = (raft_server_private_t*)me_;
  me->multi_inflight = 0;
}

void raft_set_callbacks(raft_server_t* me_, raft_cbs_t* funcs, void* udata)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    memcpy(&me->cb, funcs, sizeof(raft_cbs_t));
    me->udata = udata;
    log_set_callbacks(me->log, &me->cb, me_);
}

void raft_free(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    log_free(me->log);
    free(me_);
}

void raft_clear(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    me->current_term = 0;
    me->voted_for = -1;
    me->timeout_elapsed = 0;
    me->voting_cfg_change_log_idx = -1;
    raft_set_state((raft_server_t*)me, RAFT_STATE_FOLLOWER);
    me->current_leader = NULL;
    me->commit_idx = 0;
    me->log_base   = 0;
    me->last_applied_idx = 0;
    me->num_nodes = 0;
    me->node = NULL;
    me->preferred_leader = 0;
    log_clear(me->log);
}

void raft_set_preferred_leader(raft_server_t *me_)
{
  raft_server_private_t *me = (raft_server_private_t*)me_;
  me->preferred_leader = 1;
}

void raft_unset_preferred_leader(raft_server_t *me_)
{
  raft_server_private_t *me = (raft_server_private_t*)me_;
  me->preferred_leader = 0;
}


void raft_election_start(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    __log_election(me_, NULL, "election starting: %d %d, term: %d ci: %d",
		   me->election_timeout, me->timeout_elapsed, me->current_term,
		   raft_get_current_idx(me_));

    raft_become_candidate(me_);
}

void raft_become_leader(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    int i;

    __log_election(me_, NULL, "becoming leader term:%d", raft_get_current_term(me_));

    raft_set_state(me_, RAFT_STATE_LEADER);
    for (i = 0; i < me->num_nodes; i++)
    {
        if (me->node == me->nodes[i] || !raft_node_is_voting(me->nodes[i]))
            continue;

        raft_node_t* node = me->nodes[i];
        raft_node_set_next_idx(node, raft_get_current_idx(me_) + 1);
        raft_node_set_match_idx(node, 0);
        raft_send_appendentries(me_, node);
    }
}

void raft_become_candidate(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    int i;

    __log_election(me_, NULL, "becoming candidate");

    raft_set_current_term(me_, raft_get_current_term(me_) + 1);
    for (i = 0; i < me->num_nodes; i++)
        raft_node_vote_for_me(me->nodes[i], 0);
    raft_vote(me_, me->node);
    me->current_leader = NULL;
    raft_set_state(me_, RAFT_STATE_CANDIDATE);

    /* we need a random factor here to prevent simultaneous candidates */
    /* TODO: this should probably be lower */
    me->timeout_elapsed = rand() % me->election_timeout;

    for (i = 0; i < me->num_nodes; i++)
        if (me->node != me->nodes[i] && raft_node_is_voting(me->nodes[i]))
            raft_send_requestvote(me_, me->nodes[i]);
}

void raft_become_follower(raft_server_t* me_)
{
    __log_election(me_, NULL, "becoming follower");
    raft_set_state(me_, RAFT_STATE_FOLLOWER);
}

static void compact_log(raft_server_t *me_)
{
  raft_server_private_t* me = (raft_server_private_t*)me_;
  int target = me->log_base;
  if(target <= (me->last_compacted_idx + 2*me->log_target))
    return; // Not enough log entries
  // Adjust target to leave enough log entries for lagging nodes
  target = me->last_compacted_idx + me->log_target;
  log_poll_batch(me->log, target - me->last_compacted_idx);
  me->last_compacted_idx = target;
}

void raft_checkpoint(raft_server_t *me_, int log_index)
{
  raft_server_private_t* me = (raft_server_private_t*)me_;
  log_index = log_index + 1; // internal numbering
  if(log_index > me->last_applied_idx)
    me->log_base = me->last_applied_idx;
  else
    me->log_base = log_index;
}

int raft_periodic(raft_server_t* me_, int msec_since_last_period)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    int i;

    me->timeout_elapsed += msec_since_last_period;

    /* Only one voting node means it's safe for us to become the leader */
    if (1 == raft_get_num_voting_nodes(me_) &&
        raft_node_is_voting(raft_get_my_node((void*)me)) &&
        !raft_is_leader(me_))
        raft_become_leader(me_);

    if (me->state == RAFT_STATE_LEADER)
    {
      me->timeout_elapsed = 0;
      for (i = 0; i < me->num_nodes; i++)
        if (me->node != me->nodes[i]) {
	  raft_node_set_elapsed(me->nodes[i],
				raft_node_get_elapsed(me->nodes[i]) +
				msec_since_last_period);
	  if(raft_node_get_elapsed(me->nodes[i]) >= me->request_timeout) {
	    raft_send_appendentries(me_, me->nodes[i]);
	  }
	  else if(raft_node_get_next_idx(me->nodes[i]) <= raft_get_current_idx(me_)) {
	    raft_send_appendentries(me_, me->nodes[i]);
	  }
	}
      
    }
    // Dont become the leader if we don't have voting rights
    else if (me->election_timeout <= me->timeout_elapsed && 
	     raft_node_is_voting(me->node))
    {
        if (1 < me->num_nodes)
            raft_election_start(me_);
    }

    if (me->last_applied_idx < me->commit_idx) {
      while(me->last_applied_idx < me->commit_idx) {
	if (-1 == raft_apply_entry(me_))
	  return -1;
      }
    }
    compact_log(me_);
    return 0;
}

raft_entry_t* raft_get_entry_from_idx(raft_server_t* me_, int etyidx)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    return log_get_at_idx(me->log, etyidx);
}


int raft_recv_appendentries_response(raft_server_t* me_,
                                     raft_node_t* node,
                                     msg_appendentries_response_t* r)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    __log(me_, node,
          "received appendentries response %s ci:%d rci:%d 1stidx:%d",
          r->success == 1 ? "SUCCESS" : "fail",
          raft_get_current_idx(me_),
          r->current_idx,
          r->first_idx);
    if (!node)
        return -1;

    /* Stale response -- ignore */
    if (r->current_idx <= raft_node_get_match_idx(node) && 
	raft_node_get_match_idx(node) != 0)
        return 0;

    if (!raft_is_leader(me_))
        return -1;

    /* If response contains term T > currentTerm: set currentTerm = T
       and convert to follower (§5.3) */
    if (me->current_term < r->term)
    {
        raft_set_current_term(me_, r->term);
        raft_become_follower(me_);
        return 0;
    }
    else if (me->current_term != r->term)
        return 0;

    /* stop processing, this is a node we don't have in our configuration */
    if (!node)
        return 0;

    if (0 == r->success)
    {
        /* If AppendEntries fails because of log inconsistency:
           decrement nextIndex and retry (§5.3) */
        /* We use the provided current index */
        raft_node_set_next_idx(node, min(r->current_idx + 1, raft_get_current_idx(me_)));
        assert(raft_node_get_match_idx(node) < raft_node_get_next_idx(node));
	/* retry */
        raft_send_appendentries(me_, node);
        return 0;
    }

    assert(r->current_idx <= raft_get_current_idx(me_));

    // For a positive ack do not roll back next idx
    raft_node_set_next_idx(node, max(r->current_idx + 1, raft_node_get_next_idx(node)));
    raft_node_set_match_idx(node, r->current_idx);
    
    if (!raft_node_is_voting(node) &&
        -1 == me->voting_cfg_change_log_idx &&
        raft_get_current_idx(me_) <= r->current_idx + 1 &&
        me->cb.node_has_sufficient_logs &&
        0 == raft_node_has_sufficient_logs(node)
        )
    {
      if(me->cb.node_has_sufficient_logs(me_, me->udata, node))
        raft_node_set_has_sufficient_logs(node);

    }

    /* Update commit idx */
    int votes = 1; /* include me */
    int point = r->current_idx;
    int i;
    for (i = 0; i < me->num_nodes; i++)
    {
        if (me->node == me->nodes[i] || !raft_node_is_voting(me->nodes[i]))
            continue;

	int match_idx = raft_node_get_match_idx(me->nodes[i]);

        if (0 < match_idx)
        {
            raft_entry_t* ety = raft_get_entry_from_idx(me_, match_idx);
            if (ety && ety->term == me->current_term && point <= match_idx)
                votes++;
	    if(node == me->nodes[i] && me->cb.setmatch != NULL) {
	      me->cb.setmatch(me_, me->udata, i, match_idx - 1);
	    }
	}
    }
    
    if (raft_get_num_voting_nodes(me_) / 2 < votes && raft_get_commit_idx(me_) < point)
        raft_set_commit_idx(me_, point);

    /* Aggressively send remaining entries */
    if (raft_get_entry_from_idx(me_, raft_node_get_next_idx(node)))
        raft_send_appendentries(me_, node);

    if (me->last_applied_idx < me->commit_idx) {
      while(me->last_applied_idx < me->commit_idx) {
	if (-1 == raft_apply_entry(me_))
	  break;
      }
    }
    return 0;
}

int raft_recv_appendentries(
    raft_server_t* me_,
    raft_node_t* node,
    msg_appendentries_t* ae,
    msg_appendentries_response_t *r
    )
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    int i, xxx;

    if(!me->preferred_leader || ae->n_entries > 0) {
      me->timeout_elapsed = 0;
    }

    if (0 < ae->n_entries)
        __log(me_, node, "recvd appendentries from: %lx, t:%d ci:%d lc:%d pli:%d plt:%d #%d",
              node,
              ae->term,
              raft_get_current_idx(me_),
              ae->leader_commit,
              ae->prev_log_idx,
              ae->prev_log_term,
              ae->n_entries);

    if(ae->prev_log_term == -1) {
      fprintf(stderr, "Node log is too far behind ... shutting down.\n");
      fflush(stderr);
      exit(-1);
    }

    r->term = me->current_term;

    if (raft_is_candidate(me_) && me->current_term == ae->term)
    {
        raft_become_follower(me_);
    }
    else if (me->current_term < ae->term)
    {
        raft_set_current_term(me_, ae->term);
        r->term = ae->term;
        raft_become_follower(me_);
	if(!raft_node_is_voting(me->node)) {
	  // Delete everything -- wait for cfg change entry
	  log_delete(me_, 1);
	}
    }
    else if (ae->term < me->current_term)
    {
        /* 1. Reply false if term < currentTerm (§5.1) */
        __log(me_, node, "AE term %d is less than current term %d",
              ae->term, me->current_term);
        goto fail_with_current_idx;
    }

    /* Not the first appendentries we've received */
    /* NOTE: the log starts at 1 */
    if (0 < ae->prev_log_idx && ae->prev_log_idx >= me->log_base)
    {
        raft_entry_t* e = raft_get_entry_from_idx(me_, ae->prev_log_idx);

        if (!e)
        {
            __log(me_, node, "AE no log at prev_idx %d", ae->prev_log_idx);
            goto fail_with_current_idx;
        }

        /* 2. Reply false if log doesn't contain an entry at prevLogIndex
           whose term matches prevLogTerm (§5.3) */
        if (raft_get_current_idx(me_) < ae->prev_log_idx)
            goto fail_with_current_idx;

        if (e->term != ae->prev_log_term)
        {
            __log(me_, node, "AE term doesn't match prev_term (ie. %d vs %d) ci:%d pli:%d",
                  e->term, ae->prev_log_term, raft_get_current_idx(me_), ae->prev_log_idx);
            assert(me->commit_idx < ae->prev_log_idx);
            /* Delete all the following log entries because they don't match */
	    if(me->voting_cfg_change_log_idx != -1 && 
	       me->voting_cfg_change_log_idx >= ae->prev_log_idx) {
	      me->voting_cfg_change_log_idx = -1;
	    }
            log_delete(me->log, ae->prev_log_idx);
            r->current_idx = ae->prev_log_idx - 1;
            goto fail;
        }
    }

    /* 3. If an existing entry conflicts with a new one (same index
       but different terms), delete the existing entry and all that
       follow it (§5.3) */
    if (ae->n_entries == 0 && 0 < ae->prev_log_idx && ae->prev_log_idx + 1 < raft_get_current_idx(me_))
    {
        assert(me->commit_idx < ae->prev_log_idx + 1);
	if(me->voting_cfg_change_log_idx != -1 && 
	   me->voting_cfg_change_log_idx >= ae->prev_log_idx + 1) {
	  me->voting_cfg_change_log_idx = -1;
	}
        log_delete(me->log, ae->prev_log_idx + 1);
    }

    r->current_idx = ae->prev_log_idx;

    for (i = 0; i < ae->n_entries; i++)
    {
        msg_entry_t* ety = &ae->entries[i];
        int ety_index = ae->prev_log_idx + 1 + i;
        raft_entry_t* existing_ety = raft_get_entry_from_idx(me_, ety_index);
        r->current_idx = ety_index;
        if (existing_ety && existing_ety->term != ety->term)
        {
            assert(me->commit_idx < ety_index);
	    if(me->voting_cfg_change_log_idx != -1 && 
	       me->voting_cfg_change_log_idx >= ety_index) {
	      me->voting_cfg_change_log_idx = -1;
	    }
            log_delete(me->log, ety_index);
            break;
        }
        else if (!existing_ety)
            break;
    }

 accept_entries:
    /* Pick up remainder in case of mismatch or missing entry */
    if(i < ae->n_entries) {
      raft_append_entry_batch(me_, 
			      &ae->entries[i],
			      ae->n_entries - i);
    }

    /* Release resources for remaining entries */
    if(i > 0) {
      for(xxx=0;xxx < i; xxx++) {
	if(me->cb.log_poll) {
	  me->cb.log_poll(NULL, NULL, &ae->entries[xxx], -1);
	}
      }
    }
    
    for (; i < ae->n_entries; i++)
    {
      if(raft_entry_is_voting_cfg_change(&ae->entries[i])) {
	me->voting_cfg_change_log_idx = ae->prev_log_idx + 1 + i;
      }
      r->current_idx = ae->prev_log_idx + 1 + i;
    }


    /* 4. If leaderCommit > commitIndex, set commitIndex =
        min(leaderCommit, index of most recent entry) */
    if (raft_get_commit_idx(me_) < ae->leader_commit)
    {
        int last_log_idx = max(raft_get_current_idx(me_), 1);
        raft_set_commit_idx(me_, min(last_log_idx, ae->leader_commit));
    }

    /* update current leader because we accepted appendentries from it */
    me->current_leader = node;

    r->success = 1;
    r->first_idx = ae->prev_log_idx + 1;

    return 0;

fail_with_current_idx:
    r->current_idx = raft_get_current_idx(me_);
fail:
    r->success = 0;
    r->first_idx = 0;
    return -1;
}

int raft_already_voted(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    return -1 != me->voted_for;
}

static int __should_grant_vote(raft_server_private_t* me, msg_requestvote_t* vr)
{
    if (vr->term < raft_get_current_term((void*)me))
        return 0;

    /* TODO: if voted for is candiate return 1 (if below checks pass) */
    if (raft_already_voted((void*)me))
        return 0;

    /* Below we check if log is more up-to-date... */

    int current_idx = raft_get_current_idx((void*)me);

    /* Our log is definitely not more up-to-date if it's empty! */
    if (0 == current_idx)
        return 1;

    raft_entry_t* e = raft_get_entry_from_idx((void*)me, current_idx);
    if (e->term < vr->last_log_term)
        return 1;

    if (vr->last_log_term == e->term && current_idx <= vr->last_log_idx)
        return 1;

    return 0;
}

int raft_recv_requestvote(raft_server_t* me_,
                          raft_node_t* node,
                          msg_requestvote_t* vr,
                          msg_requestvote_response_t *r)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    if (raft_get_current_term(me_) < vr->term)
    {
        raft_set_current_term(me_, vr->term);
        raft_become_follower(me_);
    }

    if (__should_grant_vote(me, vr))
    {
        /* It shouldn't be possible for a leader or candidate to grant a vote
         * Both states would have voted for themselves */
        assert(!(raft_is_leader(me_) || raft_is_candidate(me_)));

        raft_vote_for_nodeid(me_, vr->candidate_id);
        r->vote_granted = 1;

        /* there must be in an election. */
        me->current_leader = NULL;

        me->timeout_elapsed = 0;
    }
    else
        r->vote_granted = 0;

    __log_election(me_, node, "node requested vote: %d replying: %s",
		   node, r->vote_granted == 1 ? "granted" : "not granted");

    r->term = raft_get_current_term(me_);
    return 0;
}

int raft_votes_is_majority(const int num_nodes, const int nvotes)
{
    if (num_nodes < nvotes)
        return 0;
    int half = num_nodes / 2;
    return half + 1 <= nvotes;
}

int raft_recv_requestvote_response(raft_server_t* me_,
                                   raft_node_t* node,
                                   msg_requestvote_response_t* r)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    __log_election(me_, node, "node responded to requestvote status: %s",
		   r->vote_granted == 1 ? "granted" : "not granted");

    if (!raft_is_candidate(me_))
    {
        return 0;
    }
    else if (raft_get_current_term(me_) < r->term)
    {
        raft_set_current_term(me_, r->term);
        raft_become_follower(me_);
        return 0;
    }
    else if (raft_get_current_term(me_) != r->term)
    {
        /* The node who voted for us would have obtained our term.
         * Therefore this is an old message we should ignore.
         * This happens if the network is pretty choppy. */
        return 0;
    }

    __log_election(me_, node, "node responded to requestvote: %d status: %s ct:%d rt:%d",
          node, r->vote_granted == 1 ? "granted" : "not granted",
          me->current_term,
          r->term);

    if (1 == r->vote_granted)
    {
        if (node)
            raft_node_vote_for_me(node, 1);
        int votes = raft_get_nvotes_for_me(me_);
        if (raft_votes_is_majority(me->num_nodes, votes))
            raft_become_leader(me_);
    }

    return 0;
}

int raft_recv_entry(raft_server_t* me_,
                    msg_entry_t* e,
		    msg_entry_response_t *r)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    int i;
        
    /* Only one voting cfg change at a time */
    if (raft_entry_is_voting_cfg_change(e))
        if (-1 != me->voting_cfg_change_log_idx)
            return RAFT_ERR_ONE_VOTING_CHANGE_ONLY;

    if (!raft_is_leader(me_))
        return RAFT_ERR_NOT_LEADER;

    __log(me_, NULL, "received entry t:%d id: %d idx: %d",
          me->current_term, e->id, raft_get_current_idx(me_) + 1);

    raft_entry_t ety;
    ety.term = me->current_term;
    ety.id = e->id;
    ety.type = e->type;
    memcpy(&ety.data, &e->data, sizeof(raft_entry_data_t));

        
    raft_append_entry(me_, &ety);
    
    int start = rand()%me->num_nodes;
    i = start;
    do {
      if (me->node == me->nodes[i] || 
	  !me->nodes[i] ||
	  !raft_node_is_voting(me->nodes[i])) {
	i = (i + 1)%me->num_nodes;
	continue;
      }
      // Aggressively send appendentries if we think node is up to date
      raft_send_appendentries(me_, me->nodes[i]);
      i = (i + 1)%me->num_nodes;
    } while(i != start);
    
    /* if we're the only node, we can consider the entry committed */
    if (1 == me->num_nodes)
        me->commit_idx = raft_get_current_idx(me_);
    if(r != NULL) {
      r->id = e->id;
      r->idx = raft_get_current_idx(me_);
      r->term = me->current_term;
    }
    if (raft_entry_is_voting_cfg_change(e))
        me->voting_cfg_change_log_idx = raft_get_current_idx(me_);

    return 0;
}

/* Note -- not cfg change entries can be batched */
int raft_recv_entry_batch(raft_server_t* me_,
			  msg_entry_t* e,
			  msg_entry_response_t *r,
			  int count)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    int i;
    
    if (!raft_is_leader(me_))
        return RAFT_ERR_NOT_LEADER;

    int detected_voting_cfg_change = me->voting_cfg_change_log_idx;
    for(i=0;i<count;i++) {
      __log(me_, NULL, "received entry t:%d id: %d idx: %d",
      	    me->current_term, (e + i)->id, raft_get_current_idx(me_) + i + 1);
      (e + i)->term = me->current_term;
      if (raft_entry_is_voting_cfg_change(e + i)) {
	if(detected_voting_cfg_change != -1)
	  return RAFT_ERR_ONE_VOTING_CHANGE_ONLY;
	detected_voting_cfg_change = raft_get_current_idx(me_) + 1 + i;
      }
    }
    me->voting_cfg_change_log_idx = detected_voting_cfg_change;
    
    raft_append_entry_batch(me_, e, count);
    
    int start = rand()%me->num_nodes;
    i = start;
    do {
      if (me->node == me->nodes[i] || 
	  !me->nodes[i] ||
	  !raft_node_is_voting(me->nodes[i])) {
	i = (i + 1)%me->num_nodes;
	continue;
      }
      raft_send_appendentries(me_, me->nodes[i]);
      i = (i + 1)%me->num_nodes;
    } while(i != start);
    
    /* if we're the only node, we can consider the entry committed */
    if (1 == me->num_nodes)
        me->commit_idx = raft_get_current_idx(me_);
    if(r != NULL) {
      for(i=0;i<count;i++,r++) {
	r->id = e->id;
	r->idx = raft_get_current_idx(me_);
	r->term = me->current_term;
      } 
    }
    return 0;
}

int raft_send_requestvote(raft_server_t* me_, raft_node_t* node)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    msg_requestvote_t rv;

    assert(node);
    assert(node != me->node);

    __log(me_, node, "sending requestvote to: %d", node);

    rv.term = me->current_term;
    rv.last_log_idx = raft_get_current_idx(me_);
    rv.last_log_term = raft_get_last_log_term(me_);
    rv.candidate_id = raft_get_nodeid(me_);
    if (me->cb.send_requestvote)
        me->cb.send_requestvote(me_, me->udata, node, &rv);
    return 0;
}

int raft_append_entry(raft_server_t* me_, raft_entry_t* ety)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    return log_append_entry(me->log, ety);
}

int raft_append_entry_batch(raft_server_t* me_, raft_entry_t* ety, int count)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    return log_append_batch(me->log, ety, count);
}

int raft_apply_entry(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    /* Don't apply after the commit_idx */
    if (me->last_applied_idx == me->commit_idx)
        return -1;

    int log_idx = me->last_applied_idx + 1;

    raft_entry_t* ety = raft_get_entry_from_idx(me_, log_idx);
    if (!ety)
        return -1;

    __log(me_, NULL, "applying log: %d, id: %d size: %d",
          me->last_applied_idx, ety->id, ety->data.len);

    me->last_applied_idx++;
    if (me->cb.applylog)
    {
      int e = me->cb.applylog(me_, me->udata, ety, log_idx - 1);
        if (RAFT_ERR_SHUTDOWN == e)
            return RAFT_ERR_SHUTDOWN;
    }

    /* voting cfg change is now complete */
    if (log_idx == me->voting_cfg_change_log_idx)
        me->voting_cfg_change_log_idx = -1;

    return 0;
}

raft_entry_t* raft_get_entries_from_idx(raft_server_t* me_, int idx, int* n_etys)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    return log_get_from_idx(me->log, idx, n_etys);
}

int raft_send_appendentries(raft_server_t* me_, raft_node_t* node)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    assert(node);
    assert(node != me->node);

    if (!(me->cb.send_appendentries))
        return 0;

    msg_appendentries_t ae = {};
    ae.term = me->current_term;
    ae.leader_commit = raft_get_commit_idx(me_);
    ae.prev_log_idx = 0;
    ae.prev_log_term = 0;

    int next_idx = raft_node_get_next_idx(node);

    ae.entries = raft_get_entries_from_idx(me_, next_idx, &ae.n_entries);
    /* previous log is the log just before the new logs */
    if (1 < next_idx)
    {
        raft_entry_t* prev_ety = raft_get_entry_from_idx(me_, next_idx - 1);
	if(prev_ety == NULL) {
	  // We've already compacted this entry...
	  ae.prev_log_term = -1;
	}
	else {
	  ae.prev_log_term = prev_ety->term;
	}
        ae.prev_log_idx = next_idx - 1;
    }

    __log(me_, node, "sending appendentries node: ci:%d t:%d lc:%d pli:%d plt:%d",
          raft_get_current_idx(me_),
          ae.term,
          ae.leader_commit,
          ae.prev_log_idx,
          ae.prev_log_term);

    int sent = me->cb.send_appendentries(me_, me->udata, node, &ae);
    if(me->multi_inflight) {
      raft_node_set_next_idx(node, raft_node_get_next_idx(node) + sent);
      if(!sent)
	raft_node_set_elapsed(node, 0);
    }
    else {
      raft_node_set_elapsed(node, 0);
    }

    return sent;
}


void raft_send_appendentries_all(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    int i;

    me->timeout_elapsed = 0;
    for (i = 0; i < me->num_nodes; i++)
        if (me->node != me->nodes[i])
            raft_send_appendentries(me_, me->nodes[i]);
}

raft_node_t* raft_add_node(raft_server_t* me_, void* udata, int id, int is_self)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    /* set to voting if node already exists */
    raft_node_t* node = raft_get_node(me_, id);
    if (node)
    {
        raft_node_set_voting(node, 1);
        return node;
    }

    me->num_nodes++;
    me->nodes = (raft_node_t*)realloc(me->nodes, sizeof(raft_node_t*) * me->num_nodes);
    me->nodes[me->num_nodes - 1] = raft_node_new(udata, id);
    assert(me->nodes[me->num_nodes - 1]);
    if (is_self)
        me->node = me->nodes[me->num_nodes - 1];

    return me->nodes[me->num_nodes - 1];
}

raft_node_t* raft_add_non_voting_node(raft_server_t* me_, 
				      void* udata, 
				      int id, 
				      int is_self)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    raft_node_t* node = raft_add_node(me_, udata, id, is_self);
    raft_node_set_voting(node, 0);
    // Note: following line sets the next idx to the cfg change log entry
    raft_node_set_next_idx(node, me->voting_cfg_change_log_idx);
    raft_node_set_match_idx(node, 0);
    return node;
}

void raft_remove_node(raft_server_t* me_, raft_node_t* node)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    raft_node_t* new_array, *new_node;
    new_array = (raft_node_t*)calloc((me->num_nodes - 1), sizeof(raft_node_t*));
    new_node = new_array;

    int i;
    for (i = 0; i<me->num_nodes; i++)
    {
        if (me->nodes[i] == node)
            continue;
        *new_node = me->nodes[i];
        new_node++;
    }

    me->num_nodes--;
    free(me->nodes);
    me->nodes = new_array;

    free(node);
}

int raft_get_nvotes_for_me(raft_server_t* me_)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;
    int i, votes;

    for (i = 0, votes = 0; i < me->num_nodes; i++)
        if (me->node != me->nodes[i] && raft_node_is_voting(me->nodes[i]))
            if (raft_node_has_vote_for_me(me->nodes[i]))
                votes += 1;

    if (me->voted_for == raft_get_nodeid(me_))
        votes += 1;

    return votes;
}

void raft_vote(raft_server_t* me_, raft_node_t* node)
{
    raft_vote_for_nodeid(me_, node ? raft_node_get_id(node) : -1);
}

void raft_vote_for_nodeid(raft_server_t* me_, const int nodeid)
{
    raft_server_private_t* me = (raft_server_private_t*)me_;

    me->voted_for = nodeid;
    if (me->cb.persist_vote)
        me->cb.persist_vote(me_, me->udata, nodeid);
}

int raft_msg_entry_response_committed(raft_server_t* me_,
                                      const msg_entry_response_t* r)
{
    raft_entry_t* ety = raft_get_entry_from_idx(me_, r->idx);
    if (!ety)
        return 0;

    /* entry from another leader has invalidated this entry message */
    if (r->term != ety->term)
        return -1;
    return r->idx <= raft_get_commit_idx(me_);
}

int raft_apply_all(raft_server_t* me_)
{
    while (raft_get_last_applied_idx(me_) < raft_get_commit_idx(me_))
    {
        int e = raft_apply_entry(me_);
        if (RAFT_ERR_SHUTDOWN == e)
            return RAFT_ERR_SHUTDOWN;
    }

    return 0;
}

int raft_entry_is_voting_cfg_change(raft_entry_t* ety)
{
    return RAFT_LOGTYPE_ADD_NONVOTING_NODE == ety->type ||
           RAFT_LOGTYPE_REMOVE_NODE == ety->type;
}

int raft_entry_is_cfg_change(raft_entry_t* ety)
{
    return (
        RAFT_LOGTYPE_ADD_NODE == ety->type ||
        RAFT_LOGTYPE_ADD_NONVOTING_NODE == ety->type ||
        RAFT_LOGTYPE_REMOVE_NODE == ety->type);
}

raft_entry_t *raft_last_applied_ety(raft_server_t *me_)
{
  raft_server_private_t* me = (raft_server_private_t*)me_;
  if(raft_get_last_applied_idx(me_) == 0)
    return NULL;
  return log_get_at_idx(me->log, raft_get_last_applied_idx(me_));
}
