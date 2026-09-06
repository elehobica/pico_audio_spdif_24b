# 24bit S/PDIF Tx Library for Raspberry Pi Pico / Pico 2

[![Build](https://github.com/elehobica/pico_audio_spdif_24b/actions/workflows/build-binaries.yml/badge.svg)](https://github.com/elehobica/pico_audio_spdif_24b/actions/workflows/build-binaries.yml)

## Overview
S/PDIF (TOSLINK / coaxial) transmitter for Raspberry Pi Pico / Pico 2 on top of `pico_audio_32b`
(the pico_audio variant bundled with [pico_audio_i2s_32b](https://github.com/elehobica/pico_audio_i2s_32b)),
so that it can be used side by side with `pico_audio_i2s_32b` in one firmware.
Derived from `pico_audio_spdif` of [pico-extras](https://github.com/raspberrypi/pico-extras) (52fd7a7).

* Channels: 2ch (Stereo)
* Bit resolution: 24bit (the S/PDIF maximum) or 16bit + 8bit zero padding, selected by the producer format
* Sampling Frequency: 44.1 kHz / 48 kHz (the channel status word says 44.1 kHz, see `SPDIF_CONTROL_WORD`)
* Output: one GPIO driven by a PIO state machine (biphase-mark), 2 DMA channels in ping-pong

## Supported Board and Peripheral Devices
* Raspberry Pi Pico
* Raspberry Pi Pico 2
* TOSLINK transmitter module (TOTX173, TOTX1350 or similar: 3.3 V, DIN from the GPIO)

## Pin Assignment
The output pin is given by `audio_spdif_config_t::pin` (`PICO_AUDIO_SPDIF_PIN` in the samples); any GPIO can be used.

| Pico Pin # | GPIO | Function | Connection |
----|----|----|----
| 20 | GP15 | SPDIF_TX | to TOSLINK Tx module DIN |
| 36 | 3V3 | 3.3V | to TOSLINK Tx module VCC |
| 38 | GND | GND | GND |

## Input formats
The producer pool must be stereo. The audio bits of a S/PDIF subframe are the 24 time slots 4..27 (MSB at 27).

| producer `pcm_format` | sample | placed as |
----|----|----
| `AUDIO_PCM_FORMAT_S16` | int16_t | slots 12..27, slots 4..11 (the low 8 bits) zero |
| `AUDIO_PCM_FORMAT_S32` | int32_t, left justified | bits 31..8 into slots 27..4, bits 7..0 dropped |

## Differences from pico-extras pico_audio_spdif
* built on `pico_audio_32b` (`audio_format_t::pcm_format`) instead of pico-extras `pico_audio`
* `S32` input (24bit) in addition to `S16`
* two DMA channels chained to each other (`audio_spdif_config_t::dma_channel0 / dma_channel1`), so that the next
  block is already queued in the DMA when one completes: a DMA IRQ delayed by another interrupt handler no longer
  starves the PIO TX FIFO (about 20 us deep at 44.1 kHz). The IRQ has one block (192 frames, 4.35 ms) to refill
* the producer's `give_audio_buffer()` encodes into the consumer blocks without blocking: when no consumer block
  is free the remaining frames are dropped (`audio_spdif_dropped_frames()`) instead of waiting, so that the give
  may be called from an interrupt handler (e.g. the I2S DMA IRQ of `pico_audio_i2s_32b`)
* `spdif_callback_func()` (weak) is called from the DMA IRQ once per block played, to feed the producer pool
  from it like `i2s_callback_func()` of `pico_audio_i2s_32b`
* the PIO clock divider is `clk_sys / sample_freq` in 8.8 fixed point, which is exactly half of the divider
  `pico_audio_i2s_32b` uses for 32bit stereo, so both outputs run at the same rate from one producer

## Configuration (compile definitions)
| define | default | note |
----|----|----
| `PICO_AUDIO_SPDIF_PIO` | 0 | PIO block (0 or 1) |
| `PICO_AUDIO_SPDIF_DMA_IRQ` | 0 | DMA IRQ (0 or 1) |
| `PICO_AUDIO_SPDIF_PIN` | 0 | output pin of `audio_spdif_default_config` |
| `PICO_AUDIO_SPDIF_BUFFERS_PER_CHANNEL` | 3 | default consumer block count of `audio_spdif_connect()` |

`PICO_AUDIO_SPDIF_BLOCK_SAMPLE_COUNT` (192 frames) is fixed by S/PDIF (one channel status block).

## Usage
```c
#include "pico/audio.h"
#include "pico/audio_spdif.h"

static audio_format_t fmt = { .sample_freq = 44100, .pcm_format = AUDIO_PCM_FORMAT_S16, .channel_count = AUDIO_CHANNEL_STEREO };
static audio_buffer_format_t producer_fmt = { .format = &fmt, .sample_stride = 4 };
static audio_spdif_config_t cfg = { .pin = 15, .dma_channel0 = 0, .dma_channel1 = 1, .pio_sm = 0 };

audio_buffer_pool_t* pool = audio_new_producer_pool(&producer_fmt, 3, 192);    // a multiple of 192 frames
audio_spdif_setup(&fmt, &cfg);
audio_spdif_connect(pool);                          // or audio_spdif_connect_extra(pool, true, blocks, NULL)
audio_spdif_set_enabled(true);

// producer (e.g. in spdif_callback_func()): take, fill, give (the give encodes the frames into S/PDIF blocks)
audio_buffer_t* ab = take_audio_buffer(pool, false);
if (ab) { /* fill ab->buffer->bytes */ ab->sample_count = ab->max_sample_count; give_audio_buffer(pool, ab); }
```
A producer buffer length that is a multiple of 192 frames keeps the give from leaving a partly filled block
behind (a partial block is completed by the next give). A give of N frames needs N / 192 free consumer blocks
plus the 2 in the DMA; `audio_spdif_connect_extra()` sets the block count.

`src/pico_audio_32b` is a copy of the one in `pico_audio_i2s_32b` (0.8.2). When both libraries are used in one
project, add `pico_audio_32b` once (from either copy), see `samples/sine_wave_spdif_24b/CMakeLists.txt`.

## How to build with docker image
* Builds the firmware inside [pico-sdk-dev-docker:sdk-2.3.0](https://hub.docker.com/r/elehobica/pico-sdk-dev-docker) (same image used by CI). Requires Docker; no local Pico SDK setup is needed.
* `samples/build_docker.sh` drives the container build. The sample to build is taken from the current directory, so run it from inside the sample folder you want to build (`samples/xxxx`).
```
$ git clone -b main https://github.com/elehobica/pico_audio_spdif_24b.git
$ cd pico_audio_spdif_24b/samples/xxxx
$ ../build_docker.sh           # build both targets (default)
$ ../build_docker.sh pico      # build only Pico    -> build/xxxx.uf2
$ ../build_docker.sh pico2     # build only Pico 2  -> build2/xxxx.uf2
```
* Outputs: `build/xxxx.uf2` (Pico), `build2/xxxx.uf2` (Pico 2)
* Download "*.uf2" on RPI-RP2 or RP2350 drive

## How to build in local
* See ["Getting started with Raspberry Pi Pico"](https://datasheets.raspberrypi.org/pico/getting-started-with-pico.pdf)
* Put "pico-sdk", "pico-examples" and "pico-extras" on the same level with this project folder.
* Set environmental variables for PICO_SDK_PATH, PICO_EXTRAS_PATH and PICO_EXAMPLES_PATH
* Confirmed with Pico SDK 2.3.0
```
> git clone -b 2.3.0 https://github.com/raspberrypi/pico-sdk.git
> cd pico-sdk
> git submodule update -i
> cd ..
> git clone -b sdk-2.3.0 https://github.com/raspberrypi/pico-examples.git
>
> git clone -b sdk-2.3.0 https://github.com/raspberrypi/pico-extras.git
> 
> git clone -b main https://github.com/elehobica/pico_audio_spdif_24b.git
```
### Windows
* Build is confirmed in Developer Command Prompt for VS 2022 and Visual Studio Code on Windows environment
* Confirmed with cmake-3.27.2-windows-x86_64 and gcc-arm-none-eabi-10.3-2021.10-win32
* Lanuch "Developer Command Prompt for VS 2022"
```
> cd pico_audio_spdif_24b\samples\xxxx
> mkdir build && cd build
> cmake -G "NMake Makefiles" ..  ; (for Raspberry Pi Pico 1 series)
> cmake -G "NMake Makefiles" -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..  ; (for Raspberry Pi Pico 2)
> nmake
```
* Put "*.uf2" on RPI-RP2 or RP2350 drive
### Linux
* Build is confirmed with [pico-sdk-dev-docker:sdk-2.3.0]( https://hub.docker.com/r/elehobica/pico-sdk-dev-docker)
* Confirmed with cmake-3.22.1 and arm-none-eabi-gcc (15:10.3-2021.07-4) 10.3.1
```
$ cd pico_audio_spdif_24b/samples/xxxx
$ mkdir build && cd build
$ cmake ..  # (for Raspberry Pi Pico 1 series)
$ cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ..  # (for Raspberry Pi Pico 2)
$ make -j4
```
* Download "*.uf2" on RPI-RP2 or RP2350 drive

## Application Example
* [pico_cd_player](https://github.com/elehobica/pico_cd_player): S/PDIF output next to `pico_audio_i2s_32b`, fed from the I2S DMA IRQ

## License
BSD-3-Clause (Raspberry Pi (Trading) Ltd., modifications by Elehobica)
