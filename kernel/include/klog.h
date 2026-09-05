#pragma once
#include <kernel.h>

#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3

#ifndef CONFIG_LOG_LEVEL
#define CONFIG_LOG_LEVEL LOG_INFO
#endif

/* Each source file may define KLOG_SUBSYS before including this header to set
 * the prefix printed in front of its messages. */
#ifndef KLOG_SUBSYS
#define KLOG_SUBSYS "kernel"
#endif

extern int klog_runtime_level;

void klog_set_level(int level);
void klog_print(int level, const char *subsys, const char *fmt, ...) __printf(3, 4);

#define klog(level, fmt, ...)                                              \
    do {                                                                   \
        if ((level) >= CONFIG_LOG_LEVEL && (level) >= klog_runtime_level)  \
            klog_print((level), KLOG_SUBSYS, fmt, ##__VA_ARGS__);          \
    } while (0)

#define klog_debug(fmt, ...) klog(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define klog_info(fmt, ...)  klog(LOG_INFO, fmt, ##__VA_ARGS__)
#define klog_warn(fmt, ...)  klog(LOG_WARN, fmt, ##__VA_ARGS__)
#define klog_error(fmt, ...) klog(LOG_ERROR, fmt, ##__VA_ARGS__)

/* The log ring behind /dev/klog (M26 tools). */
#include <stddef.h>
#include <stdint.h>
void klog_ring_init(void);          /* after cpu_init_boot */
void klog_ring_append(const char *text, size_t n);
void klog_ring_drain(void);
size_t klog_ring_read(uint64_t *pos, char *buf, size_t n);
uint64_t klog_ring_head(void);
struct poll_source;
struct poll_source *klog_poll_source(void);
