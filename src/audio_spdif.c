/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Modified by Elehobica, 2026
//  - pico_audio_32b (audio_format_t::pcm_format) instead of pico-extras pico_audio
//  - two DMA channels in ping-pong (chain_to), IRQ handler / claims like pico_audio_i2s_32b
//  - S16 / S32 stereo input (see sample_encoding.cpp)

#include <stdio.h>
#include "pico/audio_spdif.h"
#include "pico/audio_spdif/sample_encoding.h"
#include "audio_spdif.pio.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

CU_REGISTER_DEBUG_PINS(audio_timing)

// ---- select at most one ---
//CU_SELECT_DEBUG_PINS(audio_timing)

#define audio_pio __CONCAT(pio, PICO_AUDIO_SPDIF_PIO)
#define GPIO_FUNC_PIOx __CONCAT(GPIO_FUNC_PIO, PICO_AUDIO_SPDIF_PIO)
#define DREQ_PIOx_TX0 __CONCAT(__CONCAT(DREQ_PIO, PICO_AUDIO_SPDIF_PIO), _TX0)
#define DMA_IRQ_x __CONCAT(DMA_IRQ_, PICO_AUDIO_SPDIF_DMA_IRQ)

#define SPDIF_PCM_FORMAT ((audio_pcm_format_t) AUDIO_BUFFER_FORMAT_PIO_SPDIF)

static struct {
    audio_buffer_t *playing_buffer0;
    audio_buffer_t *playing_buffer1;
    uint32_t freq;
    uint8_t pio_sm;
    uint8_t dma_channel0;
    uint8_t dma_channel1;
} shared_state;

static dma_channel_config dma_config0;
static dma_channel_config dma_config1;

static audio_format_t pio_spdif_consumer_format;
static audio_buffer_format_t pio_spdif_consumer_buffer_format = {
        .format = &pio_spdif_consumer_format,
};

static audio_buffer_t silence_buffer = {
        .sample_count =  PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT,
        .max_sample_count =  PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT,
        .format = &pio_spdif_consumer_buffer_format
};

static audio_buffer_pool_t *audio_spdif_consumer;
static bool audio_enabled;
static bool irq_handler_added;

static void __isr __time_critical_func(audio_spdif_dma_irq_handler)();

// application hook, one call per block played (weak: nothing by default)
__attribute__((weak)) void spdif_callback_func(void) {
}

const audio_spdif_config_t audio_spdif_default_config = {
    .pin = PICO_AUDIO_SPDIF_PIN,
    .dma_channel0 = 0,
    .dma_channel1 = 1,
    .pio_sm = 0,
};

#define SR_44100 0
#define SR_48000 1

#define PREAMBLE_X 0b11001001
#define PREAMBLE_Y 0b01101001
#define PREAMBLE_Z 0b00111001

#define SPDIF_CONTROL_WORD (\
    0x4 | /* copying allowed */ \
    0x20 | /* PCM encoder/decoder */ \
    (SR_44100 << 24) /* todo is this required */ \
    )

// each buffer is pre-filled with data
static void init_spdif_buffer(audio_buffer_t *buffer) {
    // BIT DESCRIPTIONS:
    //    0-3     Preamble                    A synchronisation preamble (biphase mark code violation) for audio blocks, frames, and subframes.
    //    4-7     Auxiliary sample (optional) A low-quality auxiliary channel used as specified in the channel status word, notably for producer talkback or recording studio-to-studio communication.
    //    8-27, or 4-27                       Audio sample. One sample stored with most significant bit (MSB) last. If the auxiliary sample is used, bits 4-7 are not included. Data with smaller sample bit depths always have MSB at bit 27 and are zero-extended towards the least significant bit (LSB).
    //    28      Validity (V)                Unset if the audio data are correct and suitable for D/A conversion. During the presence of defective samples, the receiving equipment may be instructed to mute its output. It is used by most CD players to indicate that concealment rather than error correction is taking place.
    //    29      User data (U)               Forms a serial data stream for each channel (with 1 bit per frame), with a format specified in the channel status word.
    //    30      Channel status (C)          Bits from each frame of an audio block are collated giving a 192-bit channel status word. Its structure depends on whether AES3 or S/PDIF is used.
    //    31      Parity (P)

    // We want to pre-encode (in our fixed length 192 buffers), the
    //   * Preamble
    //   * Aux (0)
    //   * Low4 (0)
    //
    //   * V(0)
    //   * U(0)
    //   * C(0) (or from SPDIF_CONTROL_WORD in the first 32)

    // note everything is encoded in NRZI
    // regular data bits are encoded
    // 0 -> 10 (LSB first)
    // 1 -> 11

    assert(buffer->max_sample_count == PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT);
    spdif_subframe_t *p = (spdif_subframe_t *)buffer->buffer->bytes;
    for(uint i=0;i<PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT;i++) {
        uint c_bit = i < 32 ? (SPDIF_CONTROL_WORD >> i) & 1u: 0;

        p->l = (i ? PREAMBLE_X : PREAMBLE_Z) | 0b10101010101010100000000;
        p->h = 0x55000000u | (c_bit << 29u);
        p++;
        p->l = PREAMBLE_Y | 0b10101010101010100000000;
        p->h = 0x55000000u | (c_bit << 29u);
        p++;
    }
}

uint32_t spdif_lookup[256];

const audio_format_t *audio_spdif_setup(const audio_format_t *intended_audio_format,
                                        const audio_spdif_config_t *config) {
    for(uint i=0;i<256;i++) {
        uint32_t v = 0x5555;
        uint p = 0;
        for(uint j = 0; j<8; j++) {
            if (i & (1<<j)) {
                p ^= 1;
                v |= (2<<(j*2));
            }
        }
        spdif_lookup[i] = v | (p << 16u);
    }
    uint func = GPIO_FUNC_PIOx;
    gpio_set_function(config->pin, func);

    uint8_t sm = shared_state.pio_sm = config->pio_sm;
    pio_sm_claim(audio_pio, sm);

    uint offset = pio_add_program(audio_pio, &audio_spdif_program);

    spdif_program_init(audio_pio, sm, offset, config->pin);

    silence_buffer.buffer = pico_buffer_alloc(PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT * 2 * sizeof(spdif_subframe_t));
    init_spdif_buffer(&silence_buffer);
    spdif_subframe_t *sf = (spdif_subframe_t *)silence_buffer.buffer->bytes;
    for(uint i=0;i<silence_buffer.sample_count;i++) {
        spdif_update_subframe(sf++, 0);
        spdif_update_subframe(sf++, 0);
    }

    __mem_fence_release();
    uint8_t dma_channel0 = shared_state.dma_channel0 = config->dma_channel0;
    uint8_t dma_channel1 = shared_state.dma_channel1 = config->dma_channel1;

    // DMA0 (configuration only): chains to DMA1 when it completes
    dma_config0 = dma_channel_get_default_config(dma_channel0);
    channel_config_set_transfer_data_size(&dma_config0, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config0, true);
    channel_config_set_write_increment(&dma_config0, false);
    channel_config_set_dreq(&dma_config0, DREQ_PIOx_TX0 + sm);
    channel_config_set_chain_to(&dma_config0, dma_channel1);
    // DMA1 (configuration only): chains to DMA0 when it completes
    dma_config1 = dma_channel_get_default_config(dma_channel1);
    channel_config_set_transfer_data_size(&dma_config1, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config1, true);
    channel_config_set_write_increment(&dma_config1, false);
    channel_config_set_dreq(&dma_config1, DREQ_PIOx_TX0 + sm);
    channel_config_set_chain_to(&dma_config1, dma_channel0);

    return intended_audio_format;
}

static void update_pio_frequency(uint32_t sample_freq) {
    printf("setting PIO freq for target sampling freq = %d Hz\n", (int) sample_freq);
    uint32_t system_clock_frequency = clock_get_hz(clk_sys);
    assert(system_clock_frequency < 0x40000000);
    // 8.8 fixed point: coincidentally * 256 (for 8 bit fraction) / (2 (channels) * 32 (bits) * 2 (time periods) * 2 cycles per time period)
    uint32_t divider = system_clock_frequency / sample_freq;
    assert(divider < 0x1000000);
    float pio_freq = (float) system_clock_frequency * 256 / divider;
    printf("System clock at %u Hz, S/PDIF clock divider %d/256: PIO freq %7.4f Hz\n", (uint) system_clock_frequency, (uint) divider, pio_freq);
    pio_sm_set_clkdiv_int_frac(audio_pio, shared_state.pio_sm, divider >> 8u, divider & 0xffu);
    shared_state.freq = sample_freq;
}

static audio_buffer_t *wrap_consumer_take(audio_connection_t *connection, bool block) {
    // support dynamic frequency shifting
    if (connection->producer_pool->format->sample_freq != shared_state.freq) {
        update_pio_frequency(connection->producer_pool->format->sample_freq);
    }
    return consumer_pool_take_buffer_default(connection, block);
}

static void wrap_producer_give(audio_connection_t *connection, audio_buffer_t *buffer) {
    switch (buffer->format->format->pcm_format) {
        case AUDIO_PCM_FORMAT_S16:
            stereo_s16_to_spdif_producer_give(connection, buffer);
            break;
        case AUDIO_PCM_FORMAT_S32:
            stereo_s32_to_spdif_producer_give(connection, buffer);
            break;
        default:
            panic_unsupported();
    }
}

static struct producer_pool_blocking_give_connection m2s_audio_spdif_connection = {
        .core = {
                .consumer_pool_take = wrap_consumer_take,
                .consumer_pool_give = consumer_pool_give_buffer_default,
                .producer_pool_take = producer_pool_take_buffer_default,
                .producer_pool_give = wrap_producer_give,
        }
};

bool audio_spdif_connect_thru(audio_buffer_pool_t *producer, audio_connection_t *connection) {
    return audio_spdif_connect_extra(producer, true, PICO_AUDIO_SPDIF_BUFFERS_PER_CHANNEL, connection);
}

bool audio_spdif_connect(audio_buffer_pool_t *producer) {
    return audio_spdif_connect_thru(producer, NULL);
}

bool audio_spdif_connect_extra(audio_buffer_pool_t *producer, bool buffer_on_give, uint buffer_count,
                               audio_connection_t *connection) {
    (void) buffer_on_give;      // the encoding is always done on the producer's give
    printf("Connecting PIO S/PDIF audio\n");

    assert(producer->format->pcm_format == AUDIO_PCM_FORMAT_S16 || producer->format->pcm_format == AUDIO_PCM_FORMAT_S32);
    assert(producer->format->channel_count == AUDIO_CHANNEL_STEREO);
    pio_spdif_consumer_format.pcm_format = SPDIF_PCM_FORMAT;
    pio_spdif_consumer_format.sample_freq = producer->format->sample_freq;
    pio_spdif_consumer_format.channel_count = AUDIO_CHANNEL_STEREO;
    pio_spdif_consumer_buffer_format.sample_stride = 2 * sizeof(spdif_subframe_t);

    audio_spdif_consumer = audio_new_consumer_pool(&pio_spdif_consumer_buffer_format, buffer_count, PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT);
    for (audio_buffer_t *buffer = audio_spdif_consumer->free_list; buffer; buffer = buffer->next) {
        init_spdif_buffer(buffer);
    }

    update_pio_frequency(producer->format->sample_freq);

    // todo cleanup threading
    __mem_fence_release();

    if (!connection) {
        printf("Copying stereo %s to S/PDIF at %d Hz\n",
               (producer->format->pcm_format == AUDIO_PCM_FORMAT_S32) ? "S32 (24 bit)" : "S16",
               (int) producer->format->sample_freq);
        connection = &m2s_audio_spdif_connection.core;
    }
    audio_complete_connection(connection, producer, audio_spdif_consumer);
    return true;
}

// queue the next block on dma_channel; the other channel chains to it when it completes
static void audio_start_dma_transfer(uint dma_channel, dma_channel_config *dma_config, audio_buffer_t **playing_buffer) {
    assert(!*playing_buffer);
    audio_buffer_t *ab = take_audio_buffer(audio_spdif_consumer, false);

    *playing_buffer = ab;
    if (!ab) {
        DEBUG_PINS_XOR(audio_timing, 1);
        DEBUG_PINS_XOR(audio_timing, 2);
        DEBUG_PINS_XOR(audio_timing, 1);
        // just play some silence
        ab = &silence_buffer;
    }
    assert(ab->sample_count);
    assert(ab->format->format->pcm_format == SPDIF_PCM_FORMAT);
    assert(ab->format->format->channel_count == AUDIO_CHANNEL_STEREO);
    assert(ab->format->sample_stride == 2 * sizeof(spdif_subframe_t));
    dma_channel_configure(
        dma_channel,
        dma_config,
        &audio_pio->txf[shared_state.pio_sm],   // dest
        ab->buffer->bytes,                      // src
        ab->sample_count * 4,                   // count: 2 subframes x 2 words per frame
        false                                   // trigger
    );
}

// irq handler for DMA
void __isr __time_critical_func(audio_spdif_dma_irq_handler)() {
#if PICO_AUDIO_SPDIF_NOOP
    assert(false);
#else
    uint dma_channel0 = shared_state.dma_channel0;
    uint dma_channel1 = shared_state.dma_channel1;
    if (dma_irqn_get_channel_status(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel0)) {
        dma_irqn_acknowledge_channel(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel0);
        DEBUG_PINS_SET(audio_timing, 4);
        // free the buffer we just finished
        if (shared_state.playing_buffer0) {
            give_audio_buffer(audio_spdif_consumer, shared_state.playing_buffer0);
            shared_state.playing_buffer0 = NULL;
        }
        audio_start_dma_transfer(dma_channel0, &dma_config0, &shared_state.playing_buffer0);
        DEBUG_PINS_CLR(audio_timing, 4);
        spdif_callback_func();
    } else if (dma_irqn_get_channel_status(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel1)) {
        dma_irqn_acknowledge_channel(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel1);
        DEBUG_PINS_SET(audio_timing, 4);
        // free the buffer we just finished
        if (shared_state.playing_buffer1) {
            give_audio_buffer(audio_spdif_consumer, shared_state.playing_buffer1);
            shared_state.playing_buffer1 = NULL;
        }
        audio_start_dma_transfer(dma_channel1, &dma_config1, &shared_state.playing_buffer1);
        DEBUG_PINS_CLR(audio_timing, 4);
        spdif_callback_func();
    }
#endif
}

void audio_spdif_set_enabled(bool enabled) {
    if (enabled == audio_enabled) {
        return;
    }
#ifndef NDEBUG
    printf("%s PIO S/PDIF audio (on core %d)\n", enabled ? "Enabling" : "Disabling", get_core_num());
#endif
    uint dma_channel0 = shared_state.dma_channel0;
    uint dma_channel1 = shared_state.dma_channel1;

    if (enabled) {
        dma_channel_claim(dma_channel0);
        dma_channel_claim(dma_channel1);
        audio_start_dma_transfer(dma_channel0, &dma_config0, &shared_state.playing_buffer0);
        audio_start_dma_transfer(dma_channel1, &dma_config1, &shared_state.playing_buffer1);
        if (!irq_handler_added) {
            irq_add_shared_handler(DMA_IRQ_x, audio_spdif_dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
            irq_handler_added = true;
        }
        dma_irqn_set_channel_enabled(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel0, true);
        dma_irqn_set_channel_enabled(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel1, true);
        irq_set_enabled(DMA_IRQ_x, true);
        pio_sm_set_enabled(audio_pio, shared_state.pio_sm, true);
        dma_channel_start(dma_channel0);
    } else {
        pio_sm_set_enabled(audio_pio, shared_state.pio_sm, false);
        dma_irqn_set_channel_enabled(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel0, false);
        dma_irqn_set_channel_enabled(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel1, false);
        // break the chain before the abort, so that the other channel is not re-triggered meanwhile
        hw_clear_bits(&dma_hw->ch[dma_channel0].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
        hw_clear_bits(&dma_hw->ch[dma_channel1].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
        dma_channel_abort(dma_channel0);
        dma_channel_abort(dma_channel1);
        dma_irqn_acknowledge_channel(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel0);
        dma_irqn_acknowledge_channel(PICO_AUDIO_SPDIF_DMA_IRQ, dma_channel1);
        if (shared_state.playing_buffer0) {
            give_audio_buffer(audio_spdif_consumer, shared_state.playing_buffer0);
            shared_state.playing_buffer0 = NULL;
        }
        if (shared_state.playing_buffer1) {
            give_audio_buffer(audio_spdif_consumer, shared_state.playing_buffer1);
            shared_state.playing_buffer1 = NULL;
        }
        dma_channel_unclaim(dma_channel0);
        dma_channel_unclaim(dma_channel1);
        pio_sm_clear_fifos(audio_pio, shared_state.pio_sm);
    }
    audio_enabled = enabled;
}
