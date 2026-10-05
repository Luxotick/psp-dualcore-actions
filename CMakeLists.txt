cmake_minimum_required(VERSION 3.10)

set(CMAKE_TOOLCHAIN_FILE
    "$ENV{PSPDEV}/psp/share/pspdev.cmake"
    CACHE PATH "PSP toolchain file")

project(psp-dualcore-actions C ASM)

add_executable(psp-dualcore-actions
    main.c
    me_loop.c
)

target_compile_options(psp-dualcore-actions PRIVATE
    -Os
    -G0
    -Wall
    -Wextra
    -fno-pic
)

target_link_libraries(psp-dualcore-actions
    me-core-mapper
    me-stask
    pspdebug
    pspdisplay
    pspge
    pspkernel
    psppower
    pspaudiocodec
    psputility
    pspkubridge
)

create_pbp_file(
    TARGET psp-dualcore-actions
    TITLE "PSP Dual-Core Actions"
    BUILD_PRX
)