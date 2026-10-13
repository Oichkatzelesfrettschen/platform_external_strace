/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef STRACE_MSM_AUDIO_CAPTURE_H
#define STRACE_MSM_AUDIO_CAPTURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MSM_CAPTURE_LIMIT 65536U
#define MSM_CAPTURE_TOTAL (8U * 1024U * 1024U)
#define MSM_CAPTURE_SLOTS 64U
struct msm_capture_aio {
    uint32_t address, length, data_length, private_data;
    uint16_t metadata_size, padding;
};
struct msm_capture_region { uint32_t address, length; };
struct msm_capture_pending { struct msm_capture_aio buffer; bool occupied; };
struct msm_capture_state {
    struct msm_capture_region regions[MSM_CAPTURE_SLOTS];
    struct msm_capture_pending pending[MSM_CAPTURE_SLOTS];
    size_t total;
    uint64_t sequence;
};
struct msm_capture_result {
    unsigned char *bytes;
    uint32_t length, frames;
    uint64_t sequence;
    bool contiguous, eos;
};
typedef int (*msm_capture_reader)(void *, uint32_t, size_t, void *);
const char *msm_capture_register(struct msm_capture_state *, uint32_t, uint32_t);
const char *msm_capture_unregister(struct msm_capture_state *, uint32_t);
const char *msm_capture_submit(struct msm_capture_state *, const struct msm_capture_aio *);
const char *msm_capture_complete(struct msm_capture_state *, const struct msm_capture_aio *,
                                msm_capture_reader, void *, struct msm_capture_result *);
void msm_capture_flush(struct msm_capture_state *);
void msm_capture_result_free(struct msm_capture_result *);
#endif
