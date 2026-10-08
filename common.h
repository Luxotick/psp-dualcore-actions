#ifndef COMMON_H
#define COMMON_H
#include <stdint.h>
#define TASK_MAGIC UINT32_C(0x4D455453)
#define TASK_VERSION 1u
#define ME_MARKER UINT32_C(0x4D45504F)
#define RESULT_SENTINEL INT32_MIN
#define TASK_IDLE 0u
#define TASK_READY 1u
#define TASK_RUNNING 2u
#define TASK_DONE 3u
#define TASK_ERROR 4u
#define ME_ERROR_PROTOCOL 1u
#define ME_ERROR_OVERFLOW 2u
typedef struct __attribute__((aligned(64))) SharedTask {
    volatile uint32_t magic, version, state;
    volatile int32_t a, b, result;
    volatile uint32_t me_marker, error, sequence, completed_sequence;
    volatile int32_t read_a, read_b;
    uint32_t reserved[4];
} SharedTask;
_Static_assert(sizeof(SharedTask) == 64, "Protocol occupies exactly one cache line");
_Static_assert(_Alignof(SharedTask) == 64, "Protocol must be cache-line aligned");
_Static_assert(sizeof(void *) == 4, "This protocol targets the 32-bit PSP ABI");
static inline void shared_sync(void)
{
    // Volatile accesses alone do not order stores across the processors.
    __asm__ volatile("sync" ::: "memory");
}
void me_loop(void *param);
#endif
