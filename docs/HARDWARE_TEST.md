# PSP-3000 / 6.61 / ARK-5 device test

Status: **BUILD VERIFIED. REAL PSP-3000 TEST REQUIRED.** No emulator or real ME
execution is claimed by the build checks.

Copy the packaged `PSP` directory to the Memory Stick root. The executable must
be `ms0:/PSP/GAME/ME_TWO_OPERANDS/EBOOT.PBP`. Launch **PSP Media Engine Two
Operands** under Game. The game directory must be writable: the embedded
kernel bridge writes `kcall.prx` there automatically. No manual PRX installation,
firmware flashing or signing is required for the target CFW.

The first vector displays `CPU input A: 37 B: 7`, `ME result=44`,
`state=3`, `ME marker=4D45504F`, matching sequence numbers, and
`PASS - calculation executed by ME`. The second vector runs automatically:
`1234 + 5678 = 6912`. The final summary retains both actual returned results.

- **X:** repeat both vectors. Try at least 10 repeats.
- **TRIANGLE:** run four vectors, adding `100 + 23` and `-40 + 82`.
- **START / HOME exit:** restore the original firmware instructions through
  an ME cleanup task, then return to XMB. Relaunch and repeat the test.
- **START after timeout/uncertain ME state:** request a cold reboot rather than
  unloading application memory that a delayed ME callback might still use.
  If that request fails, diagnostics stay visible; physically reboot the PSP.

The app locks the power switch while the dispatcher is active and keeps the
idle timer ticking. Suspend/resume is deliberately outside this milestone;
do not toggle the sleep switch during the session. The lock is released after
normal cleanup. Neither suspend/resume nor cleanup has device verification yet.

## What to report

For success, send a photo of the final two-vector summary and the diagnostics
showing state, marker and sequence; report whether 10 X repeats, TRIANGLE,
START exit and relaunch work. Say which ARK-5 build is installed.

For a crash/hang, send the **last visible stage number and text**, any exact
hexadecimal/decimal return code, model ID, firmware, table/witness and shared
pointer. A photo is ideal. Stages identify the boundary:

| Stage | Operation |
|---|---|
| 00 | Working directory and exit callback |
| 02 | Writing/loading kernel bridge; detecting model and image |
| 03 | Patching Classic dispatcher |
| 31 | Loading AVCODEC |
| 32 | Code publication and getEDRAM I-cache activation |
| 33 | Releasing temporary codec EDRAM |
| 05–07 | Preparing both operands, cache publication, dispatch |
| 08 | Bounded two-second wait for firmware completion |
| 09–11 | Returned data, protocol/result validation, PASS |
| 12 | Restoring original firmware instructions on ME |
| 90 | Cold reboot requested after uncertain state |

If timeout diagnostics appear, include **state, magic, version, A, B, result,
marker, error, sequence, returned sequence and ME-read operands**. A timeout
does not prove that the task was cancelled; the storage is retained and repeat
is disabled. Stages 31–33 call Sony synchronous APIs; a firmware-level hang
inside these calls cannot be converted into an application task timeout.

| Application code | Meaning |
|---|---|
| -110 / FFFFFF92 | ME completion or DDR flush timed out |
| -1001 | Unsupported model, firmware or image; no patch is applied |
| -1002 | Code/shared pointer outside verified user RAM or unaligned |
| -1003 | Protocol/state/ME error invalid |
| -1004 | Returned result wrong or sentinel unchanged |
| -1005 | ME-only marker missing |
| -1006 | Job sequence stale or mismatched |
| -1007 | Submitted or echoed operands differ |
| -1008 | Launcher did not provide a usable application path |

Other codes are passed through from PSP APIs. Initialization accepts PSP model
IDs 2/3/6/8 (03g/04g/07g/09g), firmware `06060110` (6.61), and the upstream
`t2img` witness `279C637C`, table 2. These guards establish that the selected
source mapping applies; they do not certify that ARK-5 and the physical PSP
will execute it successfully.
