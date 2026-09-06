if (NOT TARGET pico_audio_spdif_24b)
    add_library(pico_audio_spdif_24b INTERFACE)

    pico_generate_pio_header(pico_audio_spdif_24b
        ${CMAKE_CURRENT_LIST_DIR}/src/audio_spdif.pio
    )

    target_sources(pico_audio_spdif_24b INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/audio_spdif.c
        ${CMAKE_CURRENT_LIST_DIR}/src/sample_encoding.cpp
    )

    # pico_audio_32b comes from elehobica/pico_audio_i2s_32b (src/pico_audio_32b)
    target_link_libraries(pico_audio_spdif_24b INTERFACE
        pico_stdlib
        hardware_dma
        hardware_pio
        hardware_irq
        hardware_clocks
        pico_audio_32b
    )

    target_include_directories(pico_audio_spdif_24b INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/include
    )
endif()
