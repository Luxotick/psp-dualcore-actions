# CPU → ME → CPU protocol

The user EBOOT retains the normal main CPU UI and PSP APIs. A small embedded
MIT upstream kernel PRX performs privileged SC callbacks and model detection.
`sceKernelGetModel` lives in that PRX, never as a kernel-only user-EBOOT import.
Removing the baseline `pspkernel` link prevents kernel-only import tables from
being selected for user-mode application and probe binaries.

The selected upstream Classic mechanism patches three image-specific firmware
sites. The invalid custom syscall index dispatches `Task.func(Task.param)` on
ME. Code and task storage remain in lower user **system RAM**; they are not
placed in ME local EDRAM. Both are checked at runtime against the shared
`08800000..0A000000` range. The PRX relocator establishes the function address
before startup; `-G0`, no PIC and no LTO prevent reliance on the SC global
pointer or cross-file constant propagation into the task.

`SharedTask` occupies exactly one aligned 64-byte cache line in static storage.
CPU fills magic/version, both signed 32-bit operands, a changing nonzero job
sequence, READY, an invalid result and zero marker. ME invalidates the line,
checks the protocol, publishes RUNNING, loads both operands, checks signed
overflow, performs addition and publishes echoed inputs/result/marker/sequence
and DONE. UI is entirely on SC. CPU verifies all these fields after firmware
has cleared the actual busy flags. It never invokes the task function itself
or substitutes a fallback result. Expected constants are only validation
values on CPU.

## Cache ownership

| Boundary | Operation | Reason |
|---|---|---|
| Initial privileged callback | SC D-cache writeback-all, once | Publish relocated kernel callback code before bridge execution |
| ME activation | SC D-cache writeback-all, once | Publish task/wrappers, mapping and patch data before the patched getEDRAM invalidates ME I-cache |
| Per-job SC submission | `sync`; SC writeback-invalidate of 64-byte task | Put both operands in RAM and remove dirty SC copies |
| ME begins | Mapped ME D-cache invalidate of task; `sync` | Remove previous job's cached input/output |
| ME RUNNING/finish | `sync`; mapped ME writeback of task | Make progress/results visible before firmware completes |
| SC observation | SC D-cache invalidate of task; `sync` | Read ME's published values rather than old sentinels |
| Firmware busy polling | SC invalidate of upstream's separate 64-byte status line | Observe actual ME return without changing busy flags |
| Teardown | ME restore/writeback of saved instructions and ME I-cache invalidation; SC invalidate afterward | Remove pointers into the application before it is unloaded |

No job mixes cached and uncached aliases. Volatile preserves individual protocol
loads/stores; `sync` with a compiler memory clobber orders them. One producer owns
the task at a time; CPU never writes it while ME may still run. A timeout is not
cancellation, so the app retains the object and disables additional dispatches.

The one-time all-cache publication covers relocated application/library code
whose complete runtime extent is not exposed by this framework. Individual jobs
use range operations only. Upstream's kernel bridge also invalidates SC I-cache
when switching to kernel callback code; that existing behavior is preserved.

## Dependency patches

`patches/safe-task.patch` is applied only to the pinned MIT upstream revision.
It retains upstream dispatch/wrappers/cache mapping and adds:

- Real kernel-call return propagation for Classic initialization and dispatch;
  exact PRX file/load errors, truncation when writing the embedded PRX.
- A bounded readiness API that only observes busy state; no flag clearing, no
  forced success, and no leaking interrupt suspension on timeout.
- Bounded DDR flush in Classic submission; two-second job completion timeout.
- Kernel PRX model export and installation of the required nested bridge header.
- Saving/restoring the original six firmware instruction words via a cleanup
  task on ME, so both ME D/I-cache changes occur on the right processor.
- Portable PRX embedding and correct build dependencies for regenerated stubs.

The app never uses the legacy `meSafeTaskWaitReady` or the unchecked upstream
module-load helper. It checks AVCODEC loading and EDRAM acquisition/release
directly. Initialization after patches but before cache activation, failed
dispatch, timeout and failed cleanup leave storage resident and offer reboot.
The firmware core, RPC completion, image mappings and teardown remain real
hardware assumptions that cannot be established by compilation.

`patches/custom-core-build.patch` removes stdout redirection from a tool that
already writes its own stub file, regenerates imports when exports change and
uses the same portable PRX embedding helper. This avoids Windows file-sharing
and shell-quoting failures when generating the dependency archives.
No firmware mapping is invented or changed. Exception handlers, custom reset
cores, VME, DMACPLUS, task threads and pools are not included in the application.

## Verification scope

The final local build uses GCC 15.2.0, Windows CMake/Ninja and the MSYS2-hosted
[pspdev-win v2](https://github.com/dmang-dev/pspdev-win/releases/tag/v2) toolchain.
Its ZIP SHA-256 was verified:
`037f902acd90cfcf4711aeee6462479a6d0eabee6faf93ff44fe1081e653d512`.

`scripts/verify_artifacts.py` checks ELF/PBP/SFO/PRX structure, shared alignment,
actual optimized ME loads from offsets 12/16 and register addition/store to 20,
absence of ME syscalls or `$gp`, absence of a direct CPU call to `me_loop`, and
absence of kernel-only imports in the user EBOOT. It saves disassembly and a
JSON report beside the unstripped ELF/map/PRX and kernel bridge ELF. These are
static checks, **not a simulation or hardware result**.

Application warnings are errors. Third-party SDK/library headers are system
headers; their legacy prototypes are not edited or disguised as app warnings.
Upstream prints an informational warning that its kernel PRX is uncompressed;
that unsigned PRX is expected to be loaded by the target CFW. No encryption or
ARK-specific patch is added. CI uses a dated PSPDEV image and fixed ME commits;
it is configured but has not been run remotely in this session.
