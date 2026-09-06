/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Modified by Elehobica, 2026: 24 bit (S32 input) subframe update, non-blocking producer give

#ifndef _PICO_AUDIO_SPDIF_SAMPLE_ENCODING_H
#define _PICO_AUDIO_SPDIF_SAMPLE_ENCODING_H

#include "pico/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// producer_pool_give of the built-in connection (non-blocking, see sample_encoding.cpp)
void stereo_s16_to_spdif_producer_give(audio_connection_t *connection, audio_buffer_t *buffer);
void stereo_s32_to_spdif_producer_give(audio_connection_t *connection, audio_buffer_t *buffer);

// One S/PDIF subframe (32 time slots) biphase-mark encoded into 64 bits, sent l then h, LSB first.
//   l bits  0..7   preamble (slots 0..3)
//   l bits  8..23  slots 4..11: the low 8 bits of the 24-bit audio word
//   l bits 24..31  slots 12..15
//   h bits  0..23  slots 16..27 (audio MSB at slot 27)
//   h bits 24..31  slots 28..31: V, U, C, P
// Every data bit is "01" (0) or "11" (1) here; the PIO turns the stream into NRZI.
typedef struct {
    uint32_t l;
    uint32_t h;
} spdif_subframe_t;

// byte -> 16 encoded bits, bit 16 = parity of the byte
extern uint32_t spdif_lookup[256];

// 16 bit sample into slots 12..27, slots 4..11 zero (the subframe is pre-initialized: preamble, V/U/C)
static inline void spdif_update_subframe(spdif_subframe_t *subframe, int16_t sample) {
    uint32_t sl = spdif_lookup[(uint8_t) sample];
    uint32_t sh = spdif_lookup[(uint8_t) (sample >> 8u)];
    subframe->l = (subframe->l & 0xffu) | 0x555500u | (sl << 24u);
    uint32_t ph = subframe->h >> 24u;
    uint32_t h = (((uint16_t) sh) << 8u) |
                 (((uint16_t) sl) >> 8u);
    uint32_t p = (sl >> 16u) ^ (sh >> 16u);
    p = p ^ ((__mul_instruction(ph & 0x2a, 0x2a) >> 6u) & 1u);   // parity of V, U, C
    subframe->h = h | ((ph & 0x7f) << 24u) | (p << 31u);
}

// the upper 24 bits of a left-justified 32 bit sample into slots 4..27 (bits 7..0 are dropped)
static inline void spdif_update_subframe_24(spdif_subframe_t *subframe, int32_t sample) {
    uint32_t s0 = spdif_lookup[(uint8_t) (sample >> 8u)];
    uint32_t s1 = spdif_lookup[(uint8_t) (sample >> 16u)];
    uint32_t s2 = spdif_lookup[(uint8_t) (sample >> 24u)];
    subframe->l = (subframe->l & 0xffu) | (((uint16_t) s0) << 8u) | (s1 << 24u);
    uint32_t ph = subframe->h >> 24u;
    uint32_t h = (((uint16_t) s2) << 8u) |
                 (((uint16_t) s1) >> 8u);
    uint32_t p = (s0 >> 16u) ^ (s1 >> 16u) ^ (s2 >> 16u);
    p = p ^ ((__mul_instruction(ph & 0x2a, 0x2a) >> 6u) & 1u);   // parity of V, U, C
    subframe->h = h | ((ph & 0x7f) << 24u) | (p << 31u);
}

#ifdef __cplusplus
}
#endif

#endif //_PICO_AUDIO_SPDIF_SAMPLE_ENCODING_H
