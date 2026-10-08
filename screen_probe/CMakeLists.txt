add_executable(psp-screen-probe
    main.c
)

target_compile_options(psp-screen-probe PRIVATE
    -Os
    -G0
    -Wall
    -Wextra
    -fno-pic
)

target_link_libraries(psp-screen-probe
    pspctrl
    pspgu
    pspdisplay
    pspge
)

create_pbp_file(
    TARGET psp-screen-probe
    TITLE "PSP Screen Probe"
)
