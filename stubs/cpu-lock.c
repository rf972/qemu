#include "qemu/osdep.h"
#include "hw/core/cpu.h"

void cpu_mutex_lock_impl(CPUState *cpu, const char *file, int line)
{
}

void cpu_mutex_unlock_impl(CPUState *cpu, const char *file, int line)
{
}

bool cpu_mutex_locked(const CPUState *cpu)
{
    return true;
}

bool no_cpu_mutex_locked(void)
{
    return true;
}
