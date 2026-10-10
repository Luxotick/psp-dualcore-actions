cmake_minimum_required(VERSION 3.10)

set(CMAKE_TOOLCHAIN_FILE
    "$ENV{PSPDEV}/psp/share/pspdev.cmake"
    CACHE PATH "PSP toolchain file")

project(psp-dualcore-actions C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(screen_probe)

# BearSSL (TLS 1.2): the firmware sceSsl stack cannot negotiate TLS 1.2,
# which every Spotify HTTPS endpoint requires. Fetched by scripts/build.py.
set(BEARSSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/bearssl")
file(GLOB_RECURSE BEARSSL_SOURCES "${BEARSSL_DIR}/src/*.c")
add_library(bearssl STATIC ${BEARSSL_SOURCES})
target_include_directories(bearssl SYSTEM PUBLIC "${BEARSSL_DIR}/inc")
target_include_directories(bearssl PRIVATE "${BEARSSL_DIR}/src")
# No OS RNG or clock on the PSP: spotify/tls.c injects entropy and sets the
# X.509 validation time from the RTC.
target_compile_definitions(bearssl PRIVATE
    BR_USE_URANDOM=0 BR_USE_GETENTROPY=0 BR_USE_WIN32_RAND=0 BR_RDRAND=0
    BR_USE_UNIX_TIME=0 BR_USE_WIN32_TIME=0)
target_compile_options(bearssl PRIVATE -Os -G0 -fno-pic -fno-lto)

# stb_vorbis: Spotify streams are Ogg Vorbis, which the PSP codecs cannot
# decode. Built for speed; decoder memory is provided by spotify/player.c.
set(STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/stb")
add_library(stbvorbis STATIC spotify/stb_vorbis_impl.c)
target_include_directories(stbvorbis SYSTEM PUBLIC "${STB_DIR}")
target_compile_options(stbvorbis PRIVATE -O2 -G0 -fno-pic -fno-lto -w)
add_library(stbimage STATIC spotify/stb_image_impl.c)
target_include_directories(stbimage SYSTEM PUBLIC "${STB_DIR}")
target_compile_options(stbimage PRIVATE -O2 -G0 -fno-pic -fno-lto -w)

add_executable(psp-dualcore-actions
    main.c
    me_loop.c
    spotify/shannon.c
    spotify/sha1.c
    spotify/dh.c
    spotify/handshake.c
    spotify/config.c
    spotify/login.c
    spotify/audiokey.c
    spotify/http.c
    spotify/log.c
    spotify/tls.c
    spotify/webapi.c
    spotify/audiodecrypt.c
    spotify/player.c
    spotify/me_vorbis.c
    spotify/me_trampoline.S
    spotify/session.c
    spotify/gfx.c
    spotify/ui.c
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
    pspaudio
    bearssl
    stbvorbis
    stbimage
    psprtc
    pspnet_resolver
    pspnet_inet
    pspnet_apctl
    pspnet
    psppower
    pspaudiocodec
    psputility
    pspwlan
    pspsdk
)
target_link_options(psp-dualcore-actions PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/psp-dualcore-actions.map")

create_pbp_file(
    TARGET psp-dualcore-actions
    TITLE "PSPotify ME"
    VERSION "02.00"
    BUILD_PRX
)
