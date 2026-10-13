/* SPDX-License-Identifier: BSD-3-Clause */
#include "msm_audio_capture.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct msm_capture_aio) == 20, "ARM32 audio buffer ABI");
_Static_assert(offsetof(struct msm_capture_aio, private_data) == 12, "private data ABI");
_Static_assert(offsetof(struct msm_capture_aio, metadata_size) == 16, "metadata ABI");

static bool span(uint32_t address, uint32_t length)
{
    return address && length && (uint64_t) address + length <= UINT64_C(0x100000000);
}
static bool inside(uint32_t address, uint32_t length, struct msm_capture_region region)
{
    return span(address, length) && span(region.address, region.length) &&
           address >= region.address &&
           (uint64_t) address + length <= (uint64_t) region.address + region.length;
}
static bool overlaps(uint32_t left, uint32_t left_length, uint32_t right, uint32_t right_length)
{
    return (uint64_t) left < (uint64_t) right + right_length &&
           (uint64_t) right < (uint64_t) left + left_length;
}
void msm_capture_flush(struct msm_capture_state *state)
{
    memset(state->pending, 0, sizeof(state->pending));
}
const char *msm_capture_register(struct msm_capture_state *state, uint32_t address, uint32_t length)
{
    size_t free_slot = MSM_CAPTURE_SLOTS;
    if (!span(address, length)) return "registration_span";
    for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index) {
        struct msm_capture_region region = state->regions[index];
        if (!region.length) free_slot = index;
        else if (overlaps(address, length, region.address, region.length)) {
            msm_capture_flush(state);
            return "registration_overlap";
        }
    }
    if (free_slot == MSM_CAPTURE_SLOTS) return "registration_limit";
    state->regions[free_slot] = (struct msm_capture_region) {address, length};
    return NULL;
}
const char *msm_capture_unregister(struct msm_capture_state *state, uint32_t address)
{
    for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index) {
        if (state->regions[index].address == address && state->regions[index].length) {
            for (size_t pending = 0; pending < MSM_CAPTURE_SLOTS; ++pending)
                if (state->pending[pending].occupied &&
                    inside(state->pending[pending].buffer.address,
                           state->pending[pending].buffer.length, state->regions[index]))
                    state->pending[pending].occupied = false;
            state->regions[index] = (struct msm_capture_region) {0, 0};
            return NULL;
        }
    }
    return "unregistered_address";
}
const char *msm_capture_submit(struct msm_capture_state *state, const struct msm_capture_aio *buffer)
{
    bool registered = false;
    size_t free_slot = MSM_CAPTURE_SLOTS;
    if (buffer->length < 56 || buffer->length > MSM_CAPTURE_LIMIT ||
        !span(buffer->address, buffer->length) || buffer->metadata_size != 32)
        return "submission_span_or_metadata";
    for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index) {
        if (inside(buffer->address, buffer->length, state->regions[index])) registered = true;
        if (!state->pending[index].occupied) free_slot = index;
        else if (overlaps(buffer->address, buffer->length,
                          state->pending[index].buffer.address,
                          state->pending[index].buffer.length)) {
            msm_capture_flush(state);
            return "submission_overlap";
        }
    }
    if (!registered) return "submission_unregistered";
    if (free_slot == MSM_CAPTURE_SLOTS) return "submission_limit";
    state->pending[free_slot].buffer = *buffer;
    state->pending[free_slot].occupied = true;
    return NULL;
}
static uint32_t word(const unsigned char *bytes)
{
    return (uint32_t) bytes[0] | (uint32_t) bytes[1] << 8 |
           (uint32_t) bytes[2] << 16 | (uint32_t) bytes[3] << 24;
}
void msm_capture_result_free(struct msm_capture_result *result)
{
    free(result->bytes);
    memset(result, 0, sizeof(*result));
}
const char *msm_capture_complete(struct msm_capture_state *state, const struct msm_capture_aio *buffer,
                                msm_capture_reader reader, void *opaque, struct msm_capture_result *result)
{
    struct msm_capture_aio submitted = {0};
    unsigned char *second;
    bool found = false, registered = false;
    memset(result, 0, sizeof(*result));
    for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index) {
        struct msm_capture_pending *pending = &state->pending[index];
        if (pending->occupied && pending->buffer.address == buffer->address &&
            pending->buffer.private_data == buffer->private_data) {
            submitted = pending->buffer;
            pending->occupied = false;
            found = true;
            break;
        }
    }
    if (!found) return "completion_unmatched";
    if (buffer->length != submitted.length || buffer->metadata_size != submitted.metadata_size ||
        buffer->data_length < 32 || buffer->data_length > submitted.length)
        return "completion_span_or_metadata";
    for (size_t index = 0; index < MSM_CAPTURE_SLOTS; ++index)
        if (inside(buffer->address, submitted.length, state->regions[index])) registered = true;
    if (!registered) return "completion_unregistered";
    if (state->total > MSM_CAPTURE_TOTAL - buffer->data_length) return "capture_total_limit";
    result->bytes = malloc(buffer->data_length);
    second = malloc(buffer->data_length);
    if (!result->bytes || !second) {
        free(second); msm_capture_result_free(result); return "capture_allocation";
    }
    if (reader(opaque, buffer->address, buffer->data_length, result->bytes) ||
        reader(opaque, buffer->address, buffer->data_length, second)) {
        free(second); msm_capture_result_free(result); return "capture_unreadable_or_remapped";
    }
    if (memcmp(result->bytes, second, buffer->data_length)) {
        free(second); msm_capture_result_free(result); return "capture_changed";
    }
    free(second);
    state->total += buffer->data_length;
    result->length = buffer->data_length;
    result->sequence = ++state->sequence;
    result->frames = word(result->bytes + 28);
    result->contiguous = true;
    if (result->frames == UINT32_MAX) {
        result->eos = true;
        return result->length >= 56 ? NULL : "eos_descriptor_short";
    }
    if (result->frames > (result->length - 32) / 24) return "descriptor_count_bounds";
    uint64_t previous_end = 0;
    for (uint32_t index = 0; index < result->frames; ++index) {
        const unsigned char *descriptor = result->bytes + 32 + index * 24;
        uint32_t offset = word(descriptor), length = word(descriptor + 4);
        uint64_t end = (uint64_t) offset + length;
        if (offset < (uint64_t) result->frames * 24 || end > result->length - 32)
            return "descriptor_payload_bounds";
        if (index && offset < previous_end) return "descriptor_overlap_or_order";
        if (index && offset != previous_end) result->contiguous = false;
        previous_end = end;
    }
    return NULL;
}
