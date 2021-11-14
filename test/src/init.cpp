#include <cstdlib>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#define GETENV_DEFAULT(name, x) ({const char* p = getenv(name); p ? p : (x);})

int thread_num, worker_per_core, bucket_num, workload_size;

__attribute__((constructor))
void init()
{
    printf("Initializing...\n");
    thread_num = atoi(GETENV_DEFAULT("THREAD_NUM", "6"));
    worker_per_core = atoi(GETENV_DEFAULT("WORKER_PER_CORE", "2"));
    bucket_num = atoi(GETENV_DEFAULT("BUCKET_NUM", "2"));
    workload_size = atoi(GETENV_DEFAULT("WORKLOAD_SIZE", "100000000"));
}
