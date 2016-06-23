#ifndef RAFT_LOG_H_
#define RAFT_LOG_H_

typedef void* log_t;

log_t* log_new();

void log_set_callbacks(log_t* me_, raft_cbs_t* funcs, void* raft);

void log_free(log_t* me_);

void log_clear(log_t* me_);

void log_load_from_checkpoint(log_t* me_, int idx, raft_entry_t *entry);

/**
 * Add entry to log.
 * Don't add entry if we've already added this entry (based off ID)
 * Don't add entries with ID=0 
 * @return 0 if unsucessful; 1 otherwise */
int log_append_entry(log_t* me_, raft_entry_t* c, replicant_t *rep);

/* And the batch version */
int log_append_batch(log_t* me_, raft_entry_t* c, int count, replicant_t *rep);

/**
 * @return number of entries held within log */
int log_count(log_t* me_);

/**
 * Delete all logs from this log onwards */
void log_delete(log_t* me_, int idx);

/**
 * Empty the queue. */
void log_empty(log_t * me_);

/**
 * Remove oldest entry
 * @return oldest entry */
void *log_poll(log_t * me_);

raft_entry_t* log_get_from_idx(log_t* me_, int idx, int *n_etys);

raft_entry_t* log_get_at_idx(log_t* me_, int idx);

/**
 * @return youngest entry */
raft_entry_t *log_peektail(log_t * me_);

void log_delete(log_t* me_, int idx);

int log_get_current_idx(log_t* me_);

#endif /* RAFT_LOG_H_ */
