#ifndef CLOSED_LOOP_H
#define CLOSED_LOOP_H

#include <stdint.h>
#include <stddef.h>

#define CLOSED_LOOP_MAX_EDGES 64

struct closed_loop_edge_evidence {
  uintptr_t target_addr;
  uintptr_t candidate_addr;
  size_t t2c_votes;
  size_t c2t_votes;
  size_t c2c_votes;
  size_t rounds;
};

struct closed_loop_collector_state {
  int pass;
  char boot_id[64];
  char bucket_sha256[65];
  size_t same_edges;
  size_t rounds;
  struct closed_loop_edge_evidence edges[CLOSED_LOOP_MAX_EDGES];
};

int closed_loop_collect_bucket(void);
const struct closed_loop_collector_state *closed_loop_collector_state(void);
int closed_loop_path_is_private(const char *path);

#endif
