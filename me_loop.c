#include "common.h"
#include <me-core-mapper/me-core-mapper.h>

// Called only by the firmware ME dispatcher using TaskFunc(void *).
// -G0, no PIC/LTO: no SC $gp, PSP syscalls, libc or UI dependencies here.
__attribute__((noinline, aligned(64)))
void me_loop(void *param)
{
    SharedTask *task = param;
    if (task == 0) return;
    // Remove the ME's previous job from D-cache before consuming CPU inputs.
    meCoreDcacheInvalidateRange(task, sizeof *task);
    shared_sync();
    if (task->magic != TASK_MAGIC || task->version != TASK_VERSION ||
        task->state != TASK_READY || task->sequence == 0) {
        task->error = ME_ERROR_PROTOCOL;
        task->state = TASK_ERROR;
    } else {
        task->state = TASK_RUNNING;
        // Publish RUNNING for timeout diagnostics; CPU never writes while busy.
        shared_sync();
        meCoreDcacheWritebackRange(task, sizeof *task);
        const int32_t a = task->a;
        const int32_t b = task->b;
        task->read_a = a;
        task->read_b = b;
        if ((b > 0 && a > INT32_MAX - b) ||
            (b < 0 && a < INT32_MIN - b)) {
            task->error = ME_ERROR_OVERFLOW;
            task->state = TASK_ERROR;
        } else {
            task->result = a + b;
            task->me_marker = ME_MARKER;
            task->completed_sequence = task->sequence;
            shared_sync();
            task->state = TASK_DONE;
        }
    }
    // Publish outputs to system RAM before firmware clears its busy flags.
    shared_sync();
    meCoreDcacheWritebackRange(task, sizeof *task);
    shared_sync();
}

__attribute__((noinline, aligned(64)))
void me_audio_loop(void *param)
{
    SharedAudioTask *task = param;
    if (task == 0) return;
    meCoreDcacheInvalidateRange(task, sizeof *task);
    shared_sync();
    if (task->magic != TASK_MAGIC || task->version != TASK_VERSION ||
        task->state != AUDIO_TASK_READY || task->sequence == 0 ||
        task->sample_count > AUDIO_MAX_SAMPLES || task->gain_q15 > 32768u) {
        task->error = AUDIO_ERROR_PROTOCOL;
        task->state = AUDIO_TASK_ERROR;
    } else {
        task->state = AUDIO_TASK_RUNNING;
        shared_sync();
        meCoreDcacheWritebackRange(task, sizeof *task);
        int32_t peak = 0;
        for (uint32_t i = 0; i < task->sample_count; ++i) {
            const int32_t input = task->samples[i];
            int32_t output = (input * (int32_t)task->gain_q15 + 16384) >> 15;
            if (output > INT16_MAX) output = INT16_MAX;
            if (output < INT16_MIN) output = INT16_MIN;
            task->samples[i] = (int16_t)output;
            if (output < 0) output = -output;
            if (output > peak) peak = output;
        }
        task->peak = peak;
        task->completed_sequence = task->sequence;
        task->state = AUDIO_TASK_DONE;
    }
    shared_sync();
    meCoreDcacheWritebackRange(task, sizeof *task);
    shared_sync();
}
