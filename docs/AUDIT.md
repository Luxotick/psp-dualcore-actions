# Engineering audit (2026-10-08, before implementation)

Baseline: Luxotick/psp-dualcore-actions `2169b6dc7f6409b4a57feff639c2012fc1cacbb6`.
All application sources, both build systems, the workflow and screen probe were
read. There are no repository AGENTS.md instructions and no simulator backend.

## What the baseline proves

`main.c` supplies only `input=37`. `me_loop.c` reads it and adds literal `5`.
If actually dispatched on hardware, that proves one shared input and one shared
output, not two runtime operands. The screen merely prints state/result; it
does not show a validated PASS. No device evidence exists in the checkout.
The screen probe tests GU display initialization only; it never uses the ME.

UI, initialization, cache publication and dispatch run on the main Allegrex.
Only `me_loop` and the upstream firmware callbacks are intended to run on ME.
Nothing calls `me_loop` on the main CPU, but a build alone cannot establish that
the hardware dispatch works.

The 64-byte static bridge has appropriate alignment, padding and lifetime.
CPU writeback/invalidate before dispatch, ME invalidate/read/writeback, and CPU
invalidate after waiting form a plausible cache protocol. Volatile constrains
compiler accesses but does not synchronize the processors. The function and
shared object reside in application system RAM, not ME local EDRAM; the
framework's examples explicitly use these addresses. `-G0` avoids dependence on
the main CPU's global-pointer register in ME callbacks. No ELF is independently
loaded into the ME; firmware executes code from shared system RAM.

## Concrete defects and assumptions

- Only one operand crosses the bridge; no sentinel, sequence, marker or protocol
  validation, and no second vector/repeat UI.
- Module load, EDRAM acquisition/release and dispatch results are ignored.
- Classic dispatch itself discards the kernel call's return value.
- Upstream `waitMeReady` stops after ~500 iterations, clears busy flags even on
  timeout, returns success, and suspends interrupts again on a break without
  properly restoring the original state. It is not a completion proof.
- Diagnostics cannot distinguish module load, cache activation, dispatch and
  actual ME return. HOME polling is less useful than START for a user app.
- Floating `latest` toolchain/dependency refs make CI non-repeatable. README
  says Make while Actions actually uses CMake. Static-library order is fragile.
- Both user-mode targets explicitly link `pspkernel`; final binary inspection
  subsequently confirmed this selects kernel-only imports in the EBOOT.
- Classic patches firmware instructions to call application code. Upstream
  exposes no teardown to restore those instructions before the app disappears.
- Sleep/wake claims are upstream claims; neither resume nor ARK-5 has been
  validated on this PSP-3000.

## Current upstream inspected

- [Safe Task](https://github.com/mcidclan/psp-media-engine-safe-task/tree/7d4c41f77b0be8815a720e0c8212ed504c01806d),
  including Classic/Mini/MIST, assembly wrappers, bridge PRX, loader, examples,
  cache utilities, build configuration and MIST notes.
- [Custom Core](https://github.com/mcidclan/psp-media-engine-custom-core/tree/fe871e12754060f3b42fbca6b251bb8ee3ade453),
  including firmware witness, mapping tables, cache APIs, EDRAM/DMACPLUS APIs,
  kernel bridge and exception-handler implementation.
- [Reload](https://github.com/mcidclan/psp-media-engine-reload) and
  [Cracking the Unknown](https://github.com/mcidclan/psp-media-engine-cracking-the-unknown),
  especially shared RAM versus local EDRAM, cached aliases and t2img syscall map.
- ARK-4 now redirects to [ARK-5](https://github.com/PSP-Arkfive/ARK-5).
  Current ARK-5 source `a9b74547c0d1785cc77981462cf095bd5a76434f` retains
  kernel module loading, NID resolution and kuBridge model access. No evidence
  was found requiring a special ME workaround. ARK's GPL source is researched,
  not copied or linked into this application.

## Selected approach

Retain **Classic safe-task, embedded kernel PRX (PRX_FREE=0)**, with a small
reviewable dependency patch for error propagation, bounded polling, bounded DDR
flush and restoring saved firmware instructions through an ME cleanup task.
This keeps the existing dispatch/calling convention and mapped cache helpers.
Classic uses an invalid custom syscall index, leaving real syscall indices
available; AVCODEC getEDRAM activates the ME I-cache invalidation hook.

Mini hot-patches an H.264 syscall and relies on its first instruction-cache fill.
MIST injects a kernel-alias task into local EDRAM's syscall table via DMACPLUS;
its task takes `(int index, void *param)` and must tail-return through
`meSafeTaskMistFinish`. MIST also restarts ME during initialization, has an
unverified model-specific refresh constant, void injection results, unchecked
allocation and unbounded DMA waits with dispatch/interrupt suspension. Those
extra operations are unnecessary for addition and are not selected here.
No raw reset vector, custom exception handler, DMA or EDRAM allocation is needed
for the selected proof.

Custom Core selects the image from an actual firmware witness at `0x88300018`,
not just the marketing model. The `t2img` mapping is described upstream as
Slim+ (PSP-2000 and newer) and contains the cache functions needed here.
The app must reject unknown images, firmware other than 6.61 and models outside
the PSP-3000 family before patching. This is **source compatibility evidence**,
not PSP-3000/ARK-5 hardware certification. Both ME libraries are MIT; their
copyright and licenses must accompany the runnable distribution.
