#define THREAD_NUM 6
#define WORKER_PER_CORE 2
#define BUCKET_NUM 2
#define N 100000000

#include <x86intrin.h>

inline unsigned long long rdtscp()
{
    unsigned int aux;
    return __rdtscp(&aux);
}
