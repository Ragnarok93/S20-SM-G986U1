#include "common.h"

int main(void) {
  const char *gate = getenv("KS_H1A_S2_TEST");
  if (!gate || strcmp(gate, "1") != 0) {
    return 3;
  }

  disable_rseq_for_thread();
  set_unbuffer();
  set_limit();
  init_ashmem_path();
  int ret = slide_h1a_s2_test();
  fflush(stdout);
  return ret ? 2 : 0;
}
