cmake_minimum_required(VERSION 3.10)

set(CMAKE_TOOLCHAIN_FILE
    "$ENV{PSPDEV}/psp/share/pspdev.cmake"
    CACHE PATH "PSP toolchain file")

project(psp-dualcore-actions C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(screen_probe)

add_executable(psp-dualcore-actions
    main.c
    me_loop.c
)

target_compile_options(psp-dualcore-actions PRIVATE
    -Os
    -G0
    -Wall
    -Wextra
    -Werror
    -Wformat=2
    -Wshadow
    -Wstrict-prototypes
    -fno-pic
    -fno-lto
)
target_include_directories(psp-dualcore-actions SYSTEM PRIVATE
    "$ENV{PSPDEV}/psp/include" "$ENV{PSPDEV}/psp/sdk/include")

target_link_libraries(psp-dualcore-actions
    me-stask
    me-core-mapper
    pspdebug
    pspctrl
    pspdisplay
    pspge
    psppower
    pspaudiocodec
    psputility
    pspsdk
)
target_link_options(psp-dualcore-actions PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/psp-dualcore-actions.map")

create_pbp_file(
    TARGET psp-dualcore-actions
    TITLE "PSP Media Engine Two Operands"
    VERSION "02.00"
    BUILD_PRX
)
