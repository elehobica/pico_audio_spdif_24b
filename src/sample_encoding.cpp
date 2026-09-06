/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Modified by Elehobica, 2026: S32 (24 bit) input, non-blocking producer give

#include <cstdio>
#include <algorithm>

#include "pico/sample_conversion.h"
#include "pico/audio_spdif/sample_encoding.h"
#include "pico/audio_spdif.h"

static_assert(8 == sizeof(spdif_subframe_t), "");

// subframe within SPDIF
struct FmtSPDIF : public FmtDetails<spdif_subframe_t> {
};

template<>
struct converting_copy<Stereo<FmtSPDIF>, Stereo<FmtS16>> {
    static void copy(spdif_subframe_t *dest, const int16_t *src, uint sample_count) {
        for (uint i = 0; i < sample_count * 2; i++) {
            spdif_update_subframe(dest++, *src++);
        }
    }
};

template<>
struct converting_copy<Stereo<FmtSPDIF>, Stereo<FmtS32>> {
    static void copy(spdif_subframe_t *dest, const int32_t *src, uint sample_count) {
        for (uint i = 0; i < sample_count * 2; i++) {
            spdif_update_subframe_24(dest++, *src++);
        }
    }
};

static volatile uint32_t dropped_frames = 0;

// Same as producer_pool_blocking_give() of pico_audio_32b, except that it never waits for a free
// consumer block: the caller may be an interrupt handler (the I2S DMA IRQ of pico_audio_i2s_32b), and
// the blocks are freed by the S/PDIF DMA IRQ, which cannot preempt a handler of the same priority.
// Frames that do not fit are dropped and counted.
template<typename FromFmt>
static void spdif_producer_give(audio_connection_t *connection, audio_buffer_t *buffer) {
    struct producer_pool_blocking_give_connection *pbc = (struct producer_pool_blocking_give_connection *) connection;
    assert(buffer->format->sample_stride == FromFmt::frame_stride);
    assert(buffer->format->format->channel_count == FromFmt::channel_count);
    uint32_t pos = 0;
    while (pos < buffer->sample_count) {
        if (!pbc->current_consumer_buffer) {
            pbc->current_consumer_buffer = get_free_audio_buffer(pbc->core.consumer_pool, false);
            if (!pbc->current_consumer_buffer) {
                dropped_frames += buffer->sample_count - pos;
                break;
            }
            pbc->current_consumer_buffer_pos = 0;
        }
        uint sample_count = std::min(buffer->sample_count - pos,
                                     pbc->current_consumer_buffer->max_sample_count - pbc->current_consumer_buffer_pos);
        converting_copy<Stereo<FmtSPDIF>, FromFmt>::copy(
                ((spdif_subframe_t *) pbc->current_consumer_buffer->buffer->bytes) +
                pbc->current_consumer_buffer_pos * 2,
                ((const typename FromFmt::sample_t *) buffer->buffer->bytes) + pos * FromFmt::channel_count,
                sample_count);
        pos += sample_count;
        pbc->current_consumer_buffer_pos += sample_count;
        if (pbc->current_consumer_buffer_pos == pbc->current_consumer_buffer->max_sample_count) {
            pbc->current_consumer_buffer->sample_count = pbc->current_consumer_buffer->max_sample_count;
            queue_full_audio_buffer(pbc->core.consumer_pool, pbc->current_consumer_buffer);
            pbc->current_consumer_buffer = NULL;
        }
    }
    queue_free_audio_buffer(pbc->core.producer_pool, buffer);
}

void stereo_s16_to_spdif_producer_give(audio_connection_t *connection, audio_buffer_t *buffer) {
    spdif_producer_give<Stereo<FmtS16>>(connection, buffer);
}

void stereo_s32_to_spdif_producer_give(audio_connection_t *connection, audio_buffer_t *buffer) {
    spdif_producer_give<Stereo<FmtS32>>(connection, buffer);
}

uint32_t audio_spdif_dropped_frames(void) {
    return dropped_frames;
}
