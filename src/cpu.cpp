#define _GNU_SOURCE

#include "cpu.hh"

#include <sched.h> 

int assignToThisCore(int core_id)
{
    thread_local cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    return sched_setaffinity(0, sizeof(mask), &mask);
}
