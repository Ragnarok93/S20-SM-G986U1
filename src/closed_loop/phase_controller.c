#include "closed_loop.h"
#include "binding_renderer.h"
#include "bucket_answer.h"
#include "common.h"
#include "rootchain_bridge.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CLOSED_LOOP_WAIT_SECONDS (15 * 60)
#define CLOSED_LOOP_HEARTBEAT_SECONDS 5
#define CLOSED_LOOP_WATCHDOG_SECONDS (CLOSED_LOOP_HEARTBEAT_SECONDS * 3)
#define CLOSED_LOOP_PRIVATE_PREFIX "/data/data/org.mozilla.firefox/files/"
#define CLOSED_LOOP_FORBIDDEN_PREFIX "/data/local/tmp/"
#define CLOSED_LOOP_BOOT_LOCK "/data/local/tmp/rootchain_bridge_boot.lock"

enum closed_loop_phase {
  CLOSED_LOOP_SPAWN = 0,
  CLOSED_LOOP_COLLECT,
  CLOSED_LOOP_WAIT_ANSWER,
  CLOSED_LOOP_VALIDATE,
  CLOSED_LOOP_ROOTCHAIN
};

struct closed_loop_binding {
  pid_t pid;
  pid_t tgid;
  unsigned long long starttime;
  char stat_line[4096];
  char boot_id[64];
  char bucket_sha256[65];
};

static struct closed_loop_binding g_spawn_binding;
static int g_controller_entered;
static volatile sig_atomic_t g_watchdog_fired;

static const char *phase_name(enum closed_loop_phase phase) {
  switch (phase) {
    case CLOSED_LOOP_SPAWN: return "SPAWN";
    case CLOSED_LOOP_COLLECT: return "COLLECT";
    case CLOSED_LOOP_WAIT_ANSWER: return "WAIT_ANSWER";
    case CLOSED_LOOP_VALIDATE: return "VALIDATE";
    case CLOSED_LOOP_ROOTCHAIN: return "ROOTCHAIN";
  }
  return "UNKNOWN";
}

static void mmloop_fail(enum closed_loop_phase phase, const char *reason) {
  pr_error("MMLOOP phase=%s FAIL reason=%s same_boot_retry=0\n",
           phase_name(phase), reason);
  pr_info("MMLOOP verdict=FAIL phase=%s reason=%s\n",
          phase_name(phase), reason);
}

static void watchdog_handler(int signo) {
  (void)signo;
  g_watchdog_fired = 1;
  {
    static const char message[] =
        "MMLOOP heartbeat watchdog=FAIL missed=2 same_boot_retry=0\n";
    ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
  }
  _exit(70);
}

static void install_watchdog(void) {
  struct sigaction action;

  memset(&action, 0, sizeof(action));
  action.sa_handler = watchdog_handler;
  sigaction(SIGALRM, &action, NULL);
  alarm(CLOSED_LOOP_WATCHDOG_SECONDS);
}

static void heartbeat(enum closed_loop_phase phase) {
  static unsigned int tick;
  pr_info("MMLOOP heartbeat phase=%s tick=%u\n", phase_name(phase), ++tick);
  install_watchdog();
}

static int read_file_head(const char *path, char *buf, size_t len) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  ssize_t got;

  if (fd < 0) {
    return 0;
  }
  memset(buf, 0, len);
  got = read(fd, buf, len - 1);
  close(fd);
  return got > 0;
}

static int read_canonical_boot_id(char *out, size_t size) {
  static const unsigned char hyphens[] = {8, 13, 18, 23};
  size_t i;

  if (!read_file_head("/proc/sys/kernel/random/boot_id", out, size)) {
    return 0;
  }
  out[strcspn(out, "\r\n")] = '\0';
  if (strlen(out) != 36) {
    return 0;
  }
  for (i = 0; i < sizeof(hyphens); ++i) {
    if (out[hyphens[i]] != '-') {
      return 0;
    }
  }
  for (i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      continue;
    }
    if (!((out[i] >= '0' && out[i] <= '9') ||
          (out[i] >= 'a' && out[i] <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

static void sha256_to_hex(const uint8_t digest[32], char out[65]) {
  static const char digits[] = "0123456789abcdef";
  size_t i;

  for (i = 0; i < 32; ++i) {
    out[i * 2] = digits[digest[i] >> 4];
    out[i * 2 + 1] = digits[digest[i] & 15];
  }
  out[64] = '\0';
}

static int rehash_bucket(char out[65]) {
  uint8_t digest[32];

  if (bucket_answer_sha256_file(getenv("KS_BUCKET_FILE"), digest) != 0) {
    return 0;
  }
  sha256_to_hex(digest, out);
  return 1;
}

static int capture_binding(struct closed_loop_binding *binding) {
  char stat_text[4096];
  char *close_paren;
  char *fields;
  char *end = NULL;
  unsigned long long starttime = 0;
  int field = 1;

  if (!binding || !read_canonical_boot_id(binding->boot_id,
                                          sizeof(binding->boot_id))) {
    return 0;
  }
  binding->pid = getpid();
  binding->tgid = gettid();
  if (!read_file_head("/proc/self/stat", stat_text, sizeof(stat_text))) {
    return 0;
  }
  snprintf(binding->stat_line, sizeof(binding->stat_line), "%s", stat_text);
  close_paren = strrchr(stat_text, ')');
  if (!close_paren) {
    return 0;
  }
  fields = close_paren + 2;
  end = fields;
  while (*end) {
    char *token = end;
    while (*end && *end != ' ') {
      ++end;
    }
    if (*end) {
      *end++ = '\0';
    }
    ++field;
    if (field == 22) {
      starttime = strtoull(token, NULL, 10);
      break;
    }
  }
  binding->starttime = starttime;
  return starttime != 0;
}

static int binding_matches(const struct closed_loop_binding *expected) {
  struct closed_loop_binding current;

  if (!capture_binding(&current)) {
    return 0;
  }
  return current.pid == expected->pid &&
         current.tgid == expected->tgid &&
         current.starttime == expected->starttime &&
         strcmp(current.stat_line, expected->stat_line) == 0 &&
         strcmp(current.boot_id, expected->boot_id) == 0;
}

static int private_path(const char *path) {
  return closed_loop_path_is_private(path);
}

static int preflight_paths(void) {
  static const char *env_names[] = {
    "KS_BUCKET_FILE",
    "KS_BUCKET_ANSWER",
    "ROOTCHAIN_MM_PATH",
    "ROOTCHAIN_MM_BOOT_ID_PATH",
    "ROOTCHAIN_RUN22_GATE_PATH",
    "ROOTCHAIN_MANIFEST_PATH"
  };
  size_t i;

  for (i = 0; i < sizeof(env_names) / sizeof(env_names[0]); ++i) {
    const char *value = getenv(env_names[i]);
    if (!private_path(value)) {
      pr_error("MMLOOP preflight path env=%s private=FAIL\n", env_names[i]);
      return 0;
    }
  }
  return 1;
}

static int boot_lock_absent(void) {
  struct stat st;

  if (stat(CLOSED_LOOP_BOOT_LOCK, &st) == 0) {
    pr_error("MMLOOP preflight boot_lock=PRESENT same_boot_retry=0\n");
    return 0;
  }
  if (errno != ENOENT) {
    pr_error("MMLOOP preflight boot_lock=ERROR errno=%d\n", errno);
    return 0;
  }
  return 1;
}

static size_t count_dir_entries(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  size_t count = 0;

  if (!dir) {
    return (size_t)-1;
  }
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      ++count;
    }
  }
  closedir(dir);
  return count;
}

static int collector_clean(size_t fd_baseline, size_t task_baseline) {
  size_t fds = count_dir_entries("/proc/self/fd");
  size_t tasks = count_dir_entries("/proc/self/task");

  if (fds == (size_t)-1 || tasks == (size_t)-1) {
    pr_error("MMLOOP residue inspect=FAIL fds=%zu tasks=%zu\n", fds, tasks);
    return 0;
  }
  if (fds > fd_baseline || tasks != task_baseline) {
    pr_error("MMLOOP residue fds=%zu/%zu tasks=%zu/%zu FAIL\n", fds,
             fd_baseline, tasks, task_baseline);
    return 0;
  }
  pr_info("MMLOOP residue fds=%zu/%zu tasks=%zu/%zu PASS\n", fds,
          fd_baseline, tasks, task_baseline);
  return 1;
}

static int choose_edge(const struct closed_loop_collector_state *state,
                       const struct closed_loop_edge_evidence **edge) {
  size_t i;

  if (!state || !state->pass || state->same_edges == 0) {
    return 0;
  }
  for (i = 0; i < state->same_edges; ++i) {
    if (state->edges[i].t2c_votes == state->edges[i].rounds &&
        state->edges[i].c2t_votes == state->edges[i].rounds &&
        state->edges[i].c2c_votes == state->edges[i].rounds) {
      *edge = &state->edges[i];
      return 1;
    }
  }
  return 0;
}

static int render_bindings(const struct closed_loop_binding *binding,
                           uintptr_t mm,
                           const struct closed_loop_edge_evidence *edge) {
  int64_t now = (int64_t)time(NULL);
  uint64_t btime = (uint64_t)now;

  if (binding_render_mm_answer(mm, getenv("ROOTCHAIN_MM_PATH")) != 0 ||
      binding_render_companion(binding->boot_id,
                               getenv("ROOTCHAIN_MM_BOOT_ID_PATH")) != 0 ||
      binding_render_run22_gate(binding->boot_id, mm, 1, 1, 1,
                                getenv("ROOTCHAIN_RUN22_GATE_PATH")) != 0) {
    return 0;
  }
  (void)edge;
  (void)btime;
  return 1;
}

int closed_loop_main(void) {
  enum closed_loop_phase phase = CLOSED_LOOP_SPAWN;
  const char *mode = getenv("KS_MMLOOP_MODE");
  const char *answer_path = getenv("KS_BUCKET_ANSWER");
  struct closed_loop_binding validate_binding;
  const struct closed_loop_collector_state *collector;
  const struct closed_loop_edge_evidence *edge = NULL;
  size_t task_baseline;
  size_t fd_baseline;
  uintptr_t mm = 0;
  char reason[128];
  int elapsed_seconds = 0;
  int collector_rc;

  if (g_controller_entered) {
    pr_error("MMLOOP smoke phase=REENTRY FAIL same_boot_retry=0\n");
    return 3;
  }
  g_controller_entered = 1;

  if (mode && strcmp(mode, "smoke") == 0) {
    pr_info("MMLOOP smoke marker=ready phase=SPAWN\n");
    pr_info("MMLOOP smoke marker=complete phase=SPAWN\n");
    return 0;
  }

  if (!mode || strcmp(mode, "closed-loop") != 0 || getenv("KS_MM_TEST") ||
      !preflight_paths() || !boot_lock_absent() ||
      !capture_binding(&g_spawn_binding)) {
    phase = CLOSED_LOOP_SPAWN;
    mmloop_fail(phase, getenv("KS_MM_TEST") ? "forbidden-mm-test" :
                    (!mode || strcmp(mode, "closed-loop") != 0) ? "mode" :
                    !preflight_paths() ? "private-path" :
                    !boot_lock_absent() ? "boot-lock-present" :
                    "spawn-binding");
    return 2;
  }
  fd_baseline = (int)count_dir_entries("/proc/self/fd");
  task_baseline = count_dir_entries("/proc/self/task");
  if (fd_baseline == (size_t)-1 || task_baseline == (size_t)-1) {
    mmloop_fail(phase, "baseline");
    return 2;
  }
  pr_info("MMLOOP phase=%s PASS pid=%d tgid=%d starttime=%llu\n",
          phase_name(phase), g_spawn_binding.pid, g_spawn_binding.tgid,
          g_spawn_binding.starttime);

  phase = CLOSED_LOOP_COLLECT;
  heartbeat(phase);
  collector_rc = closed_loop_collect_bucket();
  alarm(0);
  collector = closed_loop_collector_state();
  if (collector_rc != 0 || !collector->pass || !binding_matches(&g_spawn_binding) ||
      !collector_clean(fd_baseline, task_baseline) ||
      !rehash_bucket(g_spawn_binding.bucket_sha256) ||
      strcmp(g_spawn_binding.bucket_sha256, collector->bucket_sha256) != 0) {
    mmloop_fail(phase, !collector_rc || !collector->pass ?
                "collector" : "residue-or-binding");
    return 2;
  }
  pr_info("MMLOOP phase=%s PASS bucket_sha256=%s same_edges=%zu\n",
          phase_name(phase), g_spawn_binding.bucket_sha256,
          collector->same_edges);

  phase = CLOSED_LOOP_WAIT_ANSWER;
  heartbeat(phase);
  while (elapsed_seconds < CLOSED_LOOP_WAIT_SECONDS) {
    if (g_watchdog_fired) {
      mmloop_fail(phase, "watchdog");
      return 2;
    }
    if (access(answer_path, F_OK) == 0) {
      break;
    }
    sleep(CLOSED_LOOP_HEARTBEAT_SECONDS);
    elapsed_seconds += CLOSED_LOOP_HEARTBEAT_SECONDS;
    heartbeat(phase);
  }
  if (elapsed_seconds >= CLOSED_LOOP_WAIT_SECONDS) {
    mmloop_fail(phase, "answer-timeout");
    return 2;
  }
  pr_info("MMLOOP phase=%s PASS elapsed=%ds\n", phase_name(phase),
          elapsed_seconds);

  phase = CLOSED_LOOP_VALIDATE;
  validate_binding = g_spawn_binding;
  if (!binding_matches(&validate_binding) || !boot_lock_absent() ||
      !rehash_bucket(validate_binding.bucket_sha256) ||
      strcmp(validate_binding.bucket_sha256,
             g_spawn_binding.bucket_sha256) != 0 ||
      !bucket_answer_load(&mm, reason, sizeof(reason))) {
    mmloop_fail(phase, reason);
    return 2;
  }
  if (!choose_edge(collector, &edge)) {
    mmloop_fail(phase, "missing-live-vote-edge");
    return 2;
  }
  pr_info("MMLOOP phase=%s PASS answer=%016llx t2c=%zu/%zu c2t=%zu/%zu "
          "c2c=%zu/%zu\n", phase_name(phase),
          (unsigned long long)mm, edge->t2c_votes, edge->rounds,
          edge->c2t_votes, edge->rounds, edge->c2c_votes, edge->rounds);

  if (!render_bindings(&validate_binding, mm, edge)) {
    mmloop_fail(phase, "renderer");
    return 2;
  }
  pr_info("MMLOOP binding render=PASS mm=%016llx companion=45 gate=94\n",
          (unsigned long long)mm);

  phase = CLOSED_LOOP_ROOTCHAIN;
  if (!binding_matches(&validate_binding) ||
      !boot_lock_absent() ||
      !rehash_bucket(validate_binding.bucket_sha256) ||
      strcmp(validate_binding.bucket_sha256,
             g_spawn_binding.bucket_sha256) != 0) {
    mmloop_fail(phase, "phase-binding");
    return 2;
  }
  heartbeat(phase);
  {
    int rootchain_rc = rootchain_bridge_run();
    if (rootchain_rc != 0) {
      mmloop_fail(phase, "rootchain-bridge");
      return rootchain_rc == 0 ? 0 : 2;
    }
  }
  pr_info("MMLOOP phase=%s PASS\n", phase_name(phase));
  pr_info("MMLOOP verdict=PASS phase=%s\n", phase_name(phase));
  return 0;
}

int main(void) {
  return closed_loop_main();
}
