#ifndef BINDING_RENDERER_H
#define BINDING_RENDERER_H

#include <stddef.h>
#include <stdint.h>

int binding_render_mm_answer_buffer(uintptr_t mm, char *buf, size_t cap,
                                    size_t *len);
int binding_render_companion_buffer(const char *boot_id, char *buf,
                                    size_t cap, size_t *len);
int binding_parse_companion_buffer(const void *data, size_t len,
                                   const char *current_boot, uint64_t btime,
                                   int64_t mm_mtime, int64_t companion_mtime,
                                   char *reason, size_t reason_size);
int binding_render_run22_gate_buffer(const char *boot_id, uintptr_t mm,
                                     int t2c_pass, int c2t_pass,
                                     int c2c_pass, char *buf, size_t cap,
                                     size_t *len);
int binding_parse_run22_gate_buffer(const void *data, size_t len,
                                    const char *current_boot, uintptr_t mm,
                                    uint64_t btime, int64_t mm_mtime,
                                    int64_t gate_mtime, char *reason,
                                    size_t reason_size);
int binding_write_atomic(const char *path, const void *data, size_t len);
int binding_render_mm_answer(uintptr_t mm, const char *path);
int binding_render_companion(const char *boot_id, const char *path);
int binding_render_run22_gate(const char *boot_id, uintptr_t mm,
                              int t2c_pass, int c2t_pass, int c2c_pass,
                              const char *path);

#endif
