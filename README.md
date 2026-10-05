# PSP Dual-Core Actions

This is a small PSP Media Engine safe-task proof of concept. The main CPU
creates a cache-line-aligned bridge, dispatches a Media Engine task, waits for
the safe-task dispatcher to report completion, and prints `37 + 5 = 42`.

The project uses `psp-media-engine-custom-core` for device-specific Media
Engine mappings and cache helpers, and `psp-media-engine-safe-task` for the
kernel bridge and task dispatch.

## Build

The GitHub Actions workflow clones and installs both Media Engine libraries,
then runs `make`. A successful run publishes `EBOOT.PBP` as the `EBOOT-PBP`
artifact.

For a local build, install both libraries into the PSPDEV toolchain first,
then run:

```sh
make clean
make
```

## Hardware test

Use a PSP running compatible homebrew firmware. Copy the generated `EBOOT.PBP`
into a game directory under `PSP/GAME`, launch it, and read the result on the
PSP screen. The expected output is `ME state: 1` and `ME result: 42`.

The Media Engine libraries target specific firmware and device mappings. A
successful CI build proves compilation only; it does not replace testing on
the intended PSP model and firmware.