/*
 * CPU thread main loop - common bits for user and system mode emulation
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "sysemu/cpus.h"
#include "qemu/lockable.h"

static QemuMutex qemu_cpu_list_lock;
static QemuCond exclusive_cond;
static QemuCond exclusive_resume;

/* >= 1 if a thread is inside start_exclusive/end_exclusive.  Written
 * under qemu_cpu_list_lock, read with atomic operations.
 */
static int pending_cpus;

void qemu_init_cpu_list(void)
{
    /* This is needed because qemu_init_cpu_list is also called by the
     * child process in a fork.  */
    pending_cpus = 0;

    qemu_mutex_init(&qemu_cpu_list_lock);
    qemu_cond_init(&exclusive_cond);
    qemu_cond_init(&exclusive_resume);
}

void cpu_list_lock(void)
{
    qemu_mutex_lock(&qemu_cpu_list_lock);
}

void cpu_list_unlock(void)
{
    qemu_mutex_unlock(&qemu_cpu_list_lock);
}

static bool cpu_index_auto_assigned;

static int cpu_get_free_index(void)
{
    CPUState *some_cpu;
    int max_cpu_index = 0;

    cpu_index_auto_assigned = true;
    CPU_FOREACH(some_cpu) {
        if (some_cpu->cpu_index >= max_cpu_index) {
            max_cpu_index = some_cpu->cpu_index + 1;
        }
    }
    return max_cpu_index;
}

void cpu_list_add(CPUState *cpu)
{
    QEMU_LOCK_GUARD(&qemu_cpu_list_lock);
    if (cpu->cpu_index == UNASSIGNED_CPU_INDEX) {
        cpu->cpu_index = cpu_get_free_index();
        assert(cpu->cpu_index != UNASSIGNED_CPU_INDEX);
    } else {
        assert(!cpu_index_auto_assigned);
    }
    QTAILQ_INSERT_TAIL_RCU(&cpus, cpu, node);
}

void cpu_list_remove(CPUState *cpu)
{
    QEMU_LOCK_GUARD(&qemu_cpu_list_lock);
    if (!QTAILQ_IN_USE(cpu, node)) {
        /* there is nothing to undo since cpu_exec_init() hasn't been called */
        return;
    }

    QTAILQ_REMOVE_RCU(&cpus, cpu, node);
    cpu->cpu_index = UNASSIGNED_CPU_INDEX;
}

struct qemu_work_item {
    QSIMPLEQ_ENTRY(qemu_work_item) node;
    run_on_cpu_func func;
    run_on_cpu_data data;
    bool free, exclusive, done;
    bool bql;
};

/* Called with the CPU's lock held */
static void queue_work_on_cpu_locked(CPUState *cpu, struct qemu_work_item *wi)
{
    QSIMPLEQ_INSERT_TAIL(&cpu->work_list, wi, node);
    wi->done = false;

    qemu_cpu_kick(cpu);
}

static void queue_work_on_cpu(CPUState *cpu, struct qemu_work_item *wi)
{
    cpu_mutex_lock(cpu);
    queue_work_on_cpu_locked(cpu, wi);
    cpu_mutex_unlock(cpu);
}

void run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data)
{
    struct qemu_work_item wi;
    bool has_bql = qemu_mutex_iothread_locked();

    g_assert(no_cpu_mutex_locked());

    if (qemu_cpu_is_self(cpu)) {
        if (has_bql) {
            func(cpu, data);
        } else {
            qemu_mutex_lock_iothread();
            func(cpu, data);
            qemu_mutex_unlock_iothread();
        }
        return;
    }

    /* We are going to sleep on the CPU lock, so release the BQL */
    if (has_bql) {
        qemu_mutex_unlock_iothread();
    }

    wi.func = func;
    wi.data = data;
    wi.done = false;
    wi.free = false;
    wi.exclusive = false;
    wi.bql = true;

    cpu_mutex_lock(cpu);
    queue_work_on_cpu_locked(cpu, &wi);
    while (!atomic_mb_read(&wi.done)) {
        CPUState *self_cpu = current_cpu;

        qemu_cond_wait(&cpu->cond, cpu->lock);
        current_cpu = self_cpu;
    }
    cpu_mutex_unlock(cpu);

    if (has_bql) {
        qemu_mutex_lock_iothread();
    }
}

void async_run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data)
{
    struct qemu_work_item *wi;

    wi = g_malloc0(sizeof(struct qemu_work_item));
    wi->func = func;
    wi->data = data;
    wi->free = true;
    wi->bql = true;

    queue_work_on_cpu(cpu, wi);
}

void async_run_on_cpu_no_bql(CPUState *cpu, run_on_cpu_func func,
                             run_on_cpu_data data)
{
    struct qemu_work_item *wi;

    wi = g_malloc0(sizeof(struct qemu_work_item));
    wi->func = func;
    wi->data = data;
    wi->free = true;
    /* wi->bql initialized to false */

    queue_work_on_cpu(cpu, wi);
}

/* Wait for pending exclusive operations to complete.  The CPU list lock
   must be held.  */
static inline void exclusive_idle(void)
{
    while (pending_cpus) {
        qemu_cond_wait(&exclusive_resume, &qemu_cpu_list_lock);
    }
}

/* Start an exclusive operation.
   Must only be called from outside cpu_exec.  */
void start_exclusive(void)
{
    CPUState *other_cpu;
    int running_cpus;

    qemu_mutex_lock(&qemu_cpu_list_lock);
    exclusive_idle();

    /* Make all other cpus stop executing.  */
    atomic_set(&pending_cpus, 1);

    /* Write pending_cpus before reading other_cpu->running.  */
    smp_mb();
    running_cpus = 0;
    CPU_FOREACH(other_cpu) {
        if (atomic_read(&other_cpu->running)) {
            other_cpu->has_waiter = true;
            running_cpus++;
            qemu_cpu_kick(other_cpu);
        }
    }

    atomic_set(&pending_cpus, running_cpus + 1);
    while (pending_cpus > 1) {
        qemu_cond_wait(&exclusive_cond, &qemu_cpu_list_lock);
    }

    /* Can release mutex, no one will enter another exclusive
     * section until end_exclusive resets pending_cpus to 0.
     */
    qemu_mutex_unlock(&qemu_cpu_list_lock);

    current_cpu->in_exclusive_context = true;
}

/* Finish an exclusive operation.  */
void end_exclusive(void)
{
    current_cpu->in_exclusive_context = false;

    qemu_mutex_lock(&qemu_cpu_list_lock);
    atomic_set(&pending_cpus, 0);
    qemu_cond_broadcast(&exclusive_resume);
    qemu_mutex_unlock(&qemu_cpu_list_lock);
}

/* Wait for exclusive ops to finish, and begin cpu execution.  */
void cpu_exec_start(CPUState *cpu)
{
    atomic_set(&cpu->running, true);

    /* Write cpu->running before reading pending_cpus.  */
    smp_mb();

    /* 1. start_exclusive saw cpu->running == true and pending_cpus >= 1.
     * After taking the lock we'll see cpu->has_waiter == true and run---not
     * for long because start_exclusive kicked us.  cpu_exec_end will
     * decrement pending_cpus and signal the waiter.
     *
     * 2. start_exclusive saw cpu->running == false but pending_cpus >= 1.
     * This includes the case when an exclusive item is running now.
     * Then we'll see cpu->has_waiter == false and wait for the item to
     * complete.
     *
     * 3. pending_cpus == 0.  Then start_exclusive is definitely going to
     * see cpu->running == true, and it will kick the CPU.
     */
    if (unlikely(atomic_read(&pending_cpus))) {
        QEMU_LOCK_GUARD(&qemu_cpu_list_lock);
        if (!cpu->has_waiter) {
            /* Not counted in pending_cpus, let the exclusive item
             * run.  Since we have the lock, just set cpu->running to true
             * while holding it; no need to check pending_cpus again.
             */
            atomic_set(&cpu->running, false);
            exclusive_idle();
            /* Now pending_cpus is zero.  */
            atomic_set(&cpu->running, true);
        } else {
            /* Counted in pending_cpus, go ahead and release the
             * waiter at cpu_exec_end.
             */
        }
    }
}

/* Mark cpu as not executing, and release pending exclusive ops.  */
void cpu_exec_end(CPUState *cpu)
{
    atomic_set(&cpu->running, false);

    /* Write cpu->running before reading pending_cpus.  */
    smp_mb();

    /* 1. start_exclusive saw cpu->running == true.  Then it will increment
     * pending_cpus and wait for exclusive_cond.  After taking the lock
     * we'll see cpu->has_waiter == true.
     *
     * 2. start_exclusive saw cpu->running == false but here pending_cpus >= 1.
     * This includes the case when an exclusive item started after setting
     * cpu->running to false and before we read pending_cpus.  Then we'll see
     * cpu->has_waiter == false and not touch pending_cpus.  The next call to
     * cpu_exec_start will run exclusive_idle if still necessary, thus waiting
     * for the item to complete.
     *
     * 3. pending_cpus == 0.  Then start_exclusive is definitely going to
     * see cpu->running == false, and it can ignore this CPU until the
     * next cpu_exec_start.
     */
    if (unlikely(atomic_read(&pending_cpus))) {
        QEMU_LOCK_GUARD(&qemu_cpu_list_lock);
        if (cpu->has_waiter) {
            cpu->has_waiter = false;
            atomic_set(&pending_cpus, pending_cpus - 1);
            if (pending_cpus == 1) {
                qemu_cond_signal(&exclusive_cond);
            }
        }
    }
}

void async_safe_run_on_cpu(CPUState *cpu, run_on_cpu_func func,
                           run_on_cpu_data data)
{
    struct qemu_work_item *wi;

    wi = g_malloc0(sizeof(struct qemu_work_item));
    wi->func = func;
    wi->data = data;
    wi->free = true;
    wi->exclusive = true;
    /* wi->bql initialized to false */

    queue_work_on_cpu(cpu, wi);
}

/* Called with the CPU's lock held */
static void process_queued_cpu_work_locked(CPUState *cpu)
{
    struct qemu_work_item *wi;
    bool has_bql = qemu_mutex_iothread_locked();

    if (QSIMPLEQ_EMPTY(&cpu->work_list)) {
        return;
    }
    while (!QSIMPLEQ_EMPTY(&cpu->work_list)) {
        wi = QSIMPLEQ_FIRST(&cpu->work_list);
        QSIMPLEQ_REMOVE_HEAD(&cpu->work_list, node);
        cpu_mutex_unlock(cpu);
        if (wi->exclusive) {
            /* Running work items outside the BQL avoids the following deadlock:
             * 1) start_exclusive() is called with the BQL taken while another
             * CPU is running; 2) cpu_exec in the other CPU tries to takes the
             * BQL, so it goes to sleep; start_exclusive() is sleeping too, so
             * neither CPU can proceed.
             */
            g_assert(!wi->bql);
            if (has_bql) {
                qemu_mutex_unlock_iothread();
            }
            start_exclusive();
            wi->func(cpu, wi->data);
            end_exclusive();
            if (has_bql) {
                qemu_mutex_lock_iothread();
            }
        } else {
            if (wi->bql) {
                if (has_bql) {
                    wi->func(cpu, wi->data);
                } else {
                    qemu_mutex_lock_iothread();
                    wi->func(cpu, wi->data);
                    qemu_mutex_unlock_iothread();
                }
            } else {
                if (has_bql) {
                    qemu_mutex_unlock_iothread();
                    wi->func(cpu, wi->data);
                    qemu_mutex_lock_iothread();
                } else {
                    wi->func(cpu, wi->data);
                }
            }
        }
        cpu_mutex_lock(cpu);
        if (wi->free) {
            g_free(wi);
        } else {
            atomic_mb_set(&wi->done, true);
        }
    }
    qemu_cond_broadcast(&cpu->cond);
}

void process_queued_cpu_work(CPUState *cpu)
{
    cpu_mutex_lock(cpu);
    process_queued_cpu_work_locked(cpu);
    cpu_mutex_unlock(cpu);
}
