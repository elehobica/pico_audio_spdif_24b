# Change Log
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/)
and this project adheres to [Semantic Versioning](http://semver.org/).

## [Unreleased]
### Added
* Initial release: pico-extras `pico_audio_spdif` (52fd7a7) rebuilt on `pico_audio_32b` (from pico_audio_i2s_32b 0.8.2)
* S16 (16bit + 8bit zero padding) and S32 (upper 24bit) stereo input
* Two DMA channels in ping-pong (chain_to), non-blocking producer give with a dropped-frame counter, `spdif_callback_func()` hook
* Support Raspberry Pi Pico and Pico 2 (pico-sdk 2.3.0), GitHub Actions for build and release, `samples/build_docker.sh`
* `samples/sine_wave_spdif_24b`
