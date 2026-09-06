/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Modified by Elehobica, 2026
//  - built on pico_audio_32b (elehobica/pico_audio_i2s_32b) instead of pico-extras pico_audio
//  - S16 (16 bit + 8 bit zero padding) and S32 (upper 24 bits) stereo input
//  - two DMA channels in ping-pong (chain_to), like pico_audio_i2s_32b
//  - non-blocking producer give (drops frames instead of waiting), usable from an IRQ

#ifndef _PICO_AUDIO_SPDIF_H
#define _PICO_AUDIO_SPDIF_H

#include "pico/audio.h"

/** \file audio_spdif.h
 *  \defgroup pico_audio_spdif pico_audio_spdif
 *  S/PDIF audio output using the PIO
 *
 * This library uses the \ref pio system to implement a S/PDIF audio interface
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PICO_AUDIO_SPDIF_DMA_IRQ
#ifdef PICO_AUDIO_DMA_IRQ
#define PICO_AUDIO_SPDIF_DMA_IRQ PICO_AUDIO_DMA_IRQ
#else
#define PICO_AUDIO_SPDIF_DMA_IRQ 0
#endif
#endif

#ifndef PICO_AUDIO_SPDIF_PIO
#ifdef PICO_AUDIO_PIO
#define PICO_AUDIO_SPDIF_PIO PICO_AUDIO_PIO
#else
#define PICO_AUDIO_SPDIF_PIO 0
#endif
#endif

#if !(PICO_AUDIO_SPDIF_DMA_IRQ == 0 || PICO_AUDIO_SPDIF_DMA_IRQ == 1)
#error PICO_AUDIO_SPDIF_DMA_IRQ must be 0 or 1
#endif

#if !(PICO_AUDIO_SPDIF_PIO == 0 || PICO_AUDIO_SPDIF_PIO == 1)
#error PICO_AUDIO_SPDIF_PIO must be 0 or 1
#endif

#ifndef PICO_AUDIO_SPDIF_BUFFERS_PER_CHANNEL
#ifdef PICO_AUDIO_BUFFERS_PER_CHANNEL
#define PICO_AUDIO_SPDIF_BUFFERS_PER_CHANNEL PICO_AUDIO_BUFFERS_PER_CHANNEL
#else
#define PICO_AUDIO_SPDIF_BUFFERS_PER_CHANNEL 3u
#endif
#endif

// fixed by S/PDIF: one channel status block
#define PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT 192u

// Allow use of pico_audio driver without actually doing anything much
#ifndef PICO_AUDIO_SPDIF_NOOP
#ifdef PICO_AUDIO_NOOP
#define PICO_AUDIO_SPDIF_NOOP PICO_AUDIO_NOOP
#else
#define PICO_AUDIO_SPDIF_NOOP 0
#endif
#endif

#ifndef PICO_AUDIO_SPDIF_PIN
//#warning PICO_AUDIO_SPDIF_PIN should be defined when using AUDIO_SPDIF
#define PICO_AUDIO_SPDIF_PIN 0
#endif

// pcm_format tag of the consumer (encoded) blocks; not a PCM format of pico_audio_32b
#define AUDIO_BUFFER_FORMAT_PIO_SPDIF 1300

/** \brief Base configuration structure used when setting up
 * \ingroup audio_spdif
 *
 * dma_channel0 / dma_channel1 run in ping-pong: each one chains to the other when it completes
 */
typedef struct audio_spdif_config {
    uint8_t pin;
    uint8_t dma_channel0;
    uint8_t dma_channel1;
    uint8_t pio_sm;
} audio_spdif_config_t;

extern const audio_spdif_config_t audio_spdif_default_config;

/** \brief Set up system to output S/PDIF audio
 * \ingroup audio_spdif
 *
 * \param intended_audio_format stereo, AUDIO_PCM_FORMAT_S16 or AUDIO_PCM_FORMAT_S32 (upper 24 bits are sent)
 * \param config The configuration to apply.
 */
const audio_format_t *audio_spdif_setup(const audio_format_t *intended_audio_format,
                                        const audio_spdif_config_t *config);

/** \brief Connect a producer pool with a custom connection (NULL = the built-in encoding connection)
 * \ingroup audio_spdif
 */
bool audio_spdif_connect_thru(audio_buffer_pool_t *producer, audio_connection_t *connection);

/** \brief Connect a producer pool with PICO_AUDIO_SPDIF_BUFFERS_PER_CHANNEL consumer blocks
 * \ingroup audio_spdif
 */
bool audio_spdif_connect(audio_buffer_pool_t *producer);

/** \brief Connect a producer pool
 * \ingroup audio_spdif
 *
 * \param producer stereo S16 or S32 producer pool
 * \param buffer_on_give ignored: the S/PDIF encoding is always done on the producer's give
 * \param buffer_count consumer blocks of PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT frames (2 * sizeof(spdif_subframe_t)
 *                     bytes per frame). A give of N frames needs N / 192 free blocks, plus 2 in the DMA
 * \param connection NULL for the built-in encoding connection
 */
bool audio_spdif_connect_extra(audio_buffer_pool_t *producer, bool buffer_on_give, uint buffer_count,
                               audio_connection_t *connection);

/** \brief Enable / disable the S/PDIF output (PIO + DMA + IRQ). Silence blocks are sent while no block is prepared
 * \ingroup audio_spdif
 */
void audio_spdif_set_enabled(bool enabled);

/** \brief Frames dropped by the producer give because no consumer block was free (statistics)
 * \ingroup audio_spdif
 */
uint32_t audio_spdif_dropped_frames(void);

/** \brief Called from the DMA IRQ every time a block (PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT frames) has been
 * played and the next one was queued. Weak (empty by default): an application may feed the producer pool
 * from it, one block per call (see samples/), like i2s_callback_func() of pico_audio_i2s_32b
 * \ingroup audio_spdif
 */
void spdif_callback_func(void);

#ifdef __cplusplus
}
#endif

#endif //_PICO_AUDIO_SPDIF_H
