#define THREAD_NUM 8
#define WORKER_PER_CORE 8
#define BUCKET_NUM 2
#define N 40000000

#include <x86intrin.h>

inline unsigned long long rdtscp()
{
    unsigned int aux;
    return __rdtscp(&aux);
}
