cmake_minimum_required(VERSION 3.13)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include($ENV{PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

set(project_name "sine_wave_spdif_24b" C CXX ASM)
project(${project_name})
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(../../src/pico_audio_32b pico_audio_32b)
add_subdirectory(../.. pico_audio_spdif_24b)

set(bin_name ${PROJECT_NAME})
add_executable(${PROJECT_NAME}
    sine_wave.cpp
)

pico_enable_stdio_usb(${bin_name} 1)
pico_enable_stdio_uart(${bin_name} 1)

target_link_libraries(${bin_name} PRIVATE
    pico_stdlib
    pico_audio_32b
    pico_audio_spdif_24b
)

# S/PDIF output pin (TOSLINK transmitter DIN)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_AUDIO_SPDIF_PIN=15
)
# set PIO and DMA IRQ for S/PDIF (default: PIO0, DMA_IRQ_0)
# 16-bit producer instead of the 32-bit (24-bit sent) one: SINE_WAVE_S16=1
#target_compile_definitions(${PROJECT_NAME} PRIVATE
#    PICO_AUDIO_SPDIF_PIO=1
#    PICO_AUDIO_SPDIF_DMA_IRQ=1
#    SINE_WAVE_S16=1
#)

pico_add_extra_outputs(${bin_name})
