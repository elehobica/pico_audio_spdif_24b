/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Modified by Elehobica, 2026: S/PDIF output with pico_audio_spdif_24b

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/audio.h"
#include "pico/audio_spdif.h"

#define SINE_WAVE_TABLE_LEN 2048
#define SAMPLES_PER_BUFFER PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT   // one S/PDIF block (192 frames) per callback

// 0: 32-bit producer, the upper 24 bits are sent (S/PDIF 24-bit)
// 1: 16-bit producer, sent with 8 zero bits appended
#ifndef SINE_WAVE_S16
#define SINE_WAVE_S16 0
#endif

audio_buffer_pool_t *ap;
static bool decode_flg = false;

static audio_format_t audio_format = {
    .sample_freq = 44100,
    .pcm_format = SINE_WAVE_S16 ? AUDIO_PCM_FORMAT_S16 : AUDIO_PCM_FORMAT_S32,
    .channel_count = AUDIO_CHANNEL_STEREO
};

static audio_buffer_format_t producer_format = {
    .format = &audio_format,
    .sample_stride = SINE_WAVE_S16 ? 4 : 8
};

static audio_spdif_config_t spdif_config = {
    .pin = PICO_AUDIO_SPDIF_PIN,
    .dma_channel0 = 0,
    .dma_channel1 = 1,
    .pio_sm = 0
};

static int16_t sine_wave_table[SINE_WAVE_TABLE_LEN];
uint32_t step0 = 0x200000;
uint32_t step1 = 0x200000;
uint32_t pos0 = 0;
uint32_t pos1 = 0;
const uint32_t pos_max = 0x10000 * SINE_WAVE_TABLE_LEN;
uint vol = 20;

audio_buffer_pool_t *spdif_audio_init(uint32_t sample_freq)
{
    audio_format.sample_freq = sample_freq;

    audio_buffer_pool_t *producer_pool = audio_new_producer_pool(&producer_format, 3, SAMPLES_PER_BUFFER);
    ap = producer_pool;

    bool __unused ok;
    const audio_format_t *output_format;

    output_format = audio_spdif_setup(&audio_format, &spdif_config);
    if (!output_format) {
        panic("PicoAudio: Unable to open audio device.\n");
    }

    ok = audio_spdif_connect(producer_pool);    // PICO_AUDIO_SPDIF_BUFFERS_PER_CHANNEL (3) blocks
    assert(ok);
    { // initial buffer data: silence
        audio_buffer_t *ab = take_audio_buffer(producer_pool, true);
        memset(ab->buffer->bytes, 0, ab->max_sample_count * producer_format.sample_stride);
        ab->sample_count = ab->max_sample_count;
        give_audio_buffer(producer_pool, ab);
    }
    audio_spdif_set_enabled(true);

    decode_flg = true;
    return producer_pool;
}

int main() {

    stdio_init_all();

    for (int i = 0; i < SINE_WAVE_TABLE_LEN; i++) {
        sine_wave_table[i] = 32767 * cosf(i * 2 * (float) (M_PI / SINE_WAVE_TABLE_LEN));
    }

    ap = spdif_audio_init(44100);
    printf("S/PDIF %s on GP%d\n", SINE_WAVE_S16 ? "16-bit" : "24-bit", PICO_AUDIO_SPDIF_PIN);

    while (true) {
        int c = getchar_timeout_us(0);
        if (c >= 0) {
            if (c == '-' && vol) vol--;
            if ((c == '=' || c == '+') && vol < 256) vol++;
            if (c == '[' && step0 > 0x10000) step0 -= 0x10000;
            if (c == ']' && step0 < (SINE_WAVE_TABLE_LEN / 16) * 0x20000) step0 += 0x10000;
            if (c == '{' && step1 > 0x10000) step1 -= 0x10000;
            if (c == '}' && step1 < (SINE_WAVE_TABLE_LEN / 16) * 0x20000) step1 += 0x10000;
            if (c == 'd') printf("dropped frames = %u\n", (unsigned) audio_spdif_dropped_frames());
            if (c == 'q') break;
            printf("vol = %d, step0 = %d, step1 = %d      \r", static_cast<int>(vol), static_cast<int>(step0 >> 16), static_cast<int>(step1 >> 16));
        }
    }
    puts("\n");
    return 0;
}

void decode()
{
    audio_buffer_t *buffer = take_audio_buffer(ap, false);
    if (buffer == NULL) { return; }
#if SINE_WAVE_S16
    int16_t *samples = (int16_t *) buffer->buffer->bytes;
    for (uint i = 0; i < buffer->max_sample_count; i++) {
        samples[i*2+0] = (int16_t) ((vol * sine_wave_table[pos0 >> 16u]) >> 8u);  // L
        samples[i*2+1] = (int16_t) ((vol * sine_wave_table[pos1 >> 16u]) >> 8u);  // R
        pos0 += step0;
        pos1 += step1;
        if (pos0 >= pos_max) pos0 -= pos_max;
        if (pos1 >= pos_max) pos1 -= pos_max;
    }
#else
    int32_t *samples = (int32_t *) buffer->buffer->bytes;
    for (uint i = 0; i < buffer->max_sample_count; i++) {
        int32_t value0 = (vol * sine_wave_table[pos0 >> 16u]) << 8u;
        int32_t value1 = (vol * sine_wave_table[pos1 >> 16u]) << 8u;
        // use 32bit full scale (the S/PDIF output sends the upper 24 bits)
        samples[i*2+0] = value0 + (value0 >> 16u);  // L
        samples[i*2+1] = value1 + (value1 >> 16u);  // R
        pos0 += step0;
        pos1 += step1;
        if (pos0 >= pos_max) pos0 -= pos_max;
        if (pos1 >= pos_max) pos1 -= pos_max;
    }
#endif
    buffer->sample_count = buffer->max_sample_count;
    give_audio_buffer(ap, buffer);      // encodes the block that the DMA plays next
    return;
}

extern "C" {
// callback from:
//   void __isr __time_critical_func(audio_spdif_dma_irq_handler)()
//   defined at pico_audio_spdif_24b/src/audio_spdif.c
//   where spdif_callback_func() is declared with __attribute__((weak))
void spdif_callback_func()
{
    if (decode_flg) {
        decode();
    }
}
}
