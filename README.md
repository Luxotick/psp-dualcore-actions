# PSP Media Engine Two Operands

**BUILD VERIFIED — REAL PSP-3000 TEST REQUIRED.**

Minimal asymmetric main Allegrex → Media Engine → main Allegrex proof targeting
PSP-3000, firmware 6.61 and ARK-5. Both operands are written by the main CPU at
runtime into a static, aligned shared task. Only the dispatched ME callback adds
them. The main CPU checks the returned result, state, ME marker, echoed operands
and per-job sequence before printing PASS. No CPU fallback or emulator backend
exists.

The default run tests `37 + 7 = 44`, then `1234 + 5678 = 6912`, with both actual
returned results retained on screen. X repeats; TRIANGLE runs four vectors;
START exits through checked ME teardown. Unknown model/firmware/image is rejected
before patching. Job waits are limited to two seconds, and timeout disables
reuse of potentially live ME storage.

## Test on hardware

After a build, copy `dist/PSP` to the Memory Stick root, or extract
`dist/PSP-ME-Two-Operands.zip` there. The runnable file is:

```text
PSP/GAME/ME_TWO_OPERANDS/EBOOT.PBP
```

The writable game directory is needed for the automatically extracted embedded
`kcall.prx`. Launch **PSP Media Engine Two Operands** from Game.
See [hardware instructions and failure codes](docs/HARDWARE_TEST.md).

## Build

Requires PSPDEV/PSPSDK, Git, Python 3.8+ and CMake. With `PSPDEV` set and the PSP
tools on PATH, run:

```sh
python3 scripts/build.py
```

The script checks out fixed ME revisions under ignored `.deps`, applies the
reviewable patches, installs the dependencies, builds both EBOOTs, inspects the
main binary and packages it. It never resets dependency work with a different
revision. CMake/Ninja is the Windows build route. An existing PSPDEV installation
with the patched libraries also supports `make`; this alternative build was
verified with the installed Windows toolchain.

On this computer:

```powershell
$env:PSPDEV = 'C:/pspdev'
$env:PATH = 'C:\pspdev\bin;C:\msys64\usr\bin;' + $env:PATH
python scripts/build.py --cmake 'C:\Program Files\CMake\bin\cmake.exe'
```

GitHub Actions uses `pspdev/pspdev:v20261001` and the same script/pinned revisions.
The remote workflow has been configured; only local builds were executed in this
session. Its downloadable artifact contains the game ZIP and unstripped
ELF/map/PRX/disassembly under `debug`. `BUILD.json` records dependency commits,
patch hashes, compiler and EBOOT checksum. The screen probe remains a separate
display-only diagnostic and does not test ME execution.

## Design and research

The selected path is upstream **Classic ME Safe Task with an embedded kernel
bridge PRX**, using Custom Core's `t2img` mappings and cache helpers. The baseline
added a hardcoded 5 to a single shared input and had unchecked dispatch/module
results. It also linked kernel-only imports into user-mode EBOOTs.

See the [engineering audit](docs/AUDIT.md) for pinned sources and the reasoning
for choosing Classic over Mini/MIST, and [implementation notes](docs/IMPLEMENTATION.md)
for cache ownership, calling conventions, diagnostics, timeout and teardown.
Source mapping support is not proof of execution on PSP-3000/ARK-5.

The upstream ME work is by **mcidclan (m-c/d)**. The two MIT licenses are retained
in [licenses](licenses) and included in the runnable package. The root repository
had no license file; no new license is assigned to its original work. No ARK GPL
implementation code is copied into this project.
