#include "common.h"

/* Isolated S2 definitions for state referenced by the shared utility code.
 * The full-chain run_exploit() entry and fops/root/pipe state are not linked. */
int memfd_leak = -1;
int kaslr_done;
uint64_t kaslr_base;
