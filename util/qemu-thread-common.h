/*
 * Common qemu-thread implementation header file.
 *
 * Copyright Red Hat, Inc. 2018
 *
 * Authors:
 *  Peter Xu <peterx@redhat.com>,
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_THREAD_COMMON_H
#define QEMU_THREAD_COMMON_H

#include "qemu/thread.h"
#include "trace.h"
#include "qemu/timer.h"

static inline void qemu_mutex_post_init(QemuMutex *mutex)
{
#ifdef CONFIG_DEBUG_MUTEX
    mutex->file = NULL;
    mutex->line = 0;
#endif
    mutex->initialized = true;
}

static inline void qemu_mutex_pre_lock(QemuMutex *mutex,
                                       const char *file, int line)
{
    trace_qemu_mutex_lock(mutex, file, line);
}

static inline void qemu_mutex_post_lock(QemuMutex *mutex,
                                        const char *file, int line)
{
#ifdef CONFIG_DEBUG_MUTEX
    mutex->file = file;
    mutex->line = line;
#endif
    trace_qemu_mutex_locked(mutex, file, line);
}

static inline void qemu_mutex_pre_unlock(QemuMutex *mutex,
                                         const char *file, int line)
{
#ifdef CONFIG_DEBUG_MUTEX
    mutex->file = NULL;
    mutex->line = 0;
#endif
    trace_qemu_mutex_unlock(mutex, file, line);
}

static inline void qemu_mutex_pre_lock_timing(QemuMutex *mutex,
                                       const char *file, int line)
{
    trace_qemu_mutex_lock_timing(qemu_get_thread_id(), mutex, file, line, get_clock());
}

static inline void qemu_mutex_post_lock_timing(QemuMutex *mutex,
                                        const char *file, int line, uint64_t start_time)
{
#ifdef CONFIG_DEBUG_MUTEX
    mutex->file = file;
    mutex->line = line;
#endif
    uint64_t current_time = get_clock();
    
    trace_qemu_mutex_locked_timing(qemu_get_thread_id(), mutex, file, line, current_time - start_time);
    mutex->obtain_time = current_time;
}

static inline void qemu_mutex_pre_unlock_timing(QemuMutex *mutex,
                                         const char *file, int line)
{
#ifdef CONFIG_DEBUG_MUTEX
    mutex->file = NULL;
    mutex->line = 0;
#endif
    trace_qemu_mutex_unlock_timing(qemu_get_thread_id(), mutex, file, line, get_clock() - mutex->obtain_time);
}
#endif
