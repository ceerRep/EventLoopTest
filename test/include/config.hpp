#define THREAD_NUM thread_num
#define WORKER_PER_CORE worker_per_core
#define BUCKET_NUM bucket_num
#define N workload_size

#include <x86intrin.h>

extern int thread_num, worker_per_core, bucket_num, workload_size;

inline unsigned long long rdtscp()
{
    unsigned int aux;
    return __rdtscp(&aux);
}
