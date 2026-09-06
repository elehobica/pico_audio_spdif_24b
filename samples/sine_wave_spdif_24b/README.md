# Raspberry Pi Pico sine_wave with S/PDIF (TOSLINK) output

Plays a sine wave (left and right at their own frequency) over S/PDIF as 24-bit samples
(`SINE_WAVE_S16=1` in `CMakeLists.txt`: 16-bit samples with 8 zero bits appended).

## Pin Assignment

### TOSLINK transmitter module (e.g. TOTX173, TOTX1350)
| Pico Pin # | Pin Name | Function | TOSLINK Tx module |
----|----|----|----
| 20 | GP15 | SPDIF_TX | DIN |
| 36 | 3V3 | 3.3V | VCC |
| 38 | GND | GND | GND |

The output pin is set by `PICO_AUDIO_SPDIF_PIN` in `CMakeLists.txt`. Any GPIO can be used (PIO side-set).
A coaxial S/PDIF output needs a driver (e.g. a 74HC04 buffer with a 1:1 pulse transformer) instead of the optical module.

### Serial (CP2102 module)
| Pico Pin # | Pin Name | Function | CP2102 module |
----|----|----|----
|  1 | GP0 | UART0_TX | RXD |
|  2 | GP1 | UART0_RX | TXD |
|  3 | GND | GND | GND |

USB serial (CDC) is enabled as well.

### Serial interface usage
* type '+' or '=' to increase volume
* type '-' to decrease volume
* type '[' to decrease left channel's frequency
* type ']' to increase left channel's frequency
* type '{' to decrease right channel's frequency
* type '}' to increase right channel's frequency
* type 'd' to print the count of frames dropped by the S/PDIF output (0 in normal operation)

## How it works
`spdif_callback_func()` (weak symbol of `pico_audio_spdif_24b`, called from the S/PDIF DMA IRQ every block of
192 frames) takes a producer buffer of 192 frames, fills it and gives it back; the give encodes the frames into
the S/PDIF block that the DMA plays next.
