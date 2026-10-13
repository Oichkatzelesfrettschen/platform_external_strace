#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "msm_audio_capture.h"

static unsigned char memory[256];
static unsigned reads;
static bool unreadable, mutate;
static int read_memory(void *opaque, uint32_t address, size_t length, void *output)
{
    (void) opaque;
    ++reads;
    if (unreadable) return -1;
    if (address < 4096 || length > sizeof(memory) || address - 4096 > sizeof(memory) - length)
        return -1;
    memcpy(output, memory + (address - 4096), length);
    if (mutate && !(reads % 2)) ((unsigned char *)output)[60] ^= 1;
    return 0;
}
static void word(size_t offset, uint32_t value) { memcpy(memory + offset, &value, 4); }
int main(void)
{
    struct msm_capture_state state = {0};
    struct msm_capture_result result;
    struct msm_capture_aio buffer = {4096, 128, 80, 9000, 32, 0};
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    assert(reads == 0);
    assert(msm_capture_register(&state, 4096, 256) == NULL);
    assert(msm_capture_submit(&state, &buffer) == NULL);
    word(28, 1); word(32, 24); word(36, 24); word(40, 2);
    for (size_t index = 56; index < 80; ++index) memory[index] = (unsigned char) index;
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) == NULL);
    assert(result.length == 80 && result.frames == 1 && result.contiguous);
    assert(memcmp(result.bytes, memory, 80) == 0 && reads == 2);
    msm_capture_result_free(&result);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    assert(reads == 2);
    buffer.length = 257;
    assert(msm_capture_submit(&state, &buffer) != NULL);
    buffer.length = 128;
    assert(msm_capture_submit(&state, &buffer) == NULL);
    assert(msm_capture_submit(&state, &buffer) != NULL);
    msm_capture_flush(&state);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    assert(msm_capture_submit(&state, &buffer) == NULL);
    word(36, UINT32_MAX);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    msm_capture_result_free(&result);
    assert(msm_capture_unregister(&state, 4096) == NULL);
    assert(msm_capture_submit(&state, &buffer) != NULL);
    assert(msm_capture_register(&state, UINT32_MAX - 10, 20) != NULL);
    assert(msm_capture_register(&state, 4096, 256) == NULL);
    assert(msm_capture_register(&state, 4100, 128) != NULL);
    memset(memory, 0, sizeof(memory)); word(28, 2);
    word(32, 48); word(36, 8); word(56, 52); word(60, 8);
    buffer.data_length=96;
    assert(msm_capture_submit(&state, &buffer) == NULL);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    msm_capture_result_free(&result);
    word(56, 60);
    assert(msm_capture_submit(&state, &buffer) == NULL);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    msm_capture_result_free(&result);
    buffer.data_length=104;
    assert(msm_capture_submit(&state, &buffer) == NULL);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) == NULL);
    assert(!result.contiguous && result.frames==2);
    msm_capture_result_free(&result);
    word(56, 56);
    assert(msm_capture_submit(&state, &buffer) == NULL);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) == NULL);
    assert(result.contiguous);
    msm_capture_result_free(&result);
    word(28, UINT32_MAX-1);
    assert(msm_capture_submit(&state, &buffer) == NULL);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    msm_capture_result_free(&result);
    word(28, UINT32_MAX);
    assert(msm_capture_submit(&state, &buffer) == NULL);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) == NULL && result.eos);
    msm_capture_result_free(&result);
    assert(msm_capture_submit(&state, &buffer) == NULL);
    unreadable=true;
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL && !result.bytes);
    unreadable=false;
    assert(msm_capture_submit(&state, &buffer) == NULL);
    reads=0; mutate=true;
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL && !result.bytes);
    mutate=false;
    state.total=MSM_CAPTURE_TOTAL;
    assert(msm_capture_submit(&state, &buffer) == NULL);
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    msm_capture_flush(&state); state.total=0;
    assert(msm_capture_submit(&state, &buffer) == NULL);
    buffer.private_data++;
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    buffer.private_data--;
    buffer.data_length=129;
    assert(msm_capture_complete(&state, &buffer, read_memory, NULL, &result) != NULL);
    puts("msm audio registered-buffer tests pass");
    return 0;
}
