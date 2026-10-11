# Contributing to PSPotify ME

Thanks for wanting to help. Bug reports, fixes, new features and testing on other PSP models are all welcome.

## Pull requests

- **Write the PR description yourself, in your own words.** Explain what you changed, why, and how you tested it. AI-written PR descriptions will be closed.
- **AI tools are fine for the code.** If you used one, you still own the change. You should understand it and be able to answer review questions about it. I want to talk to the person who made the change, not to a generated summary.
- **Keep PRs focused.** One fix or feature per PR is easier to review and test on hardware.
- **Say how you tested it.** Mention the model, the firmware and what you tried (e.g. "PSP-3000, 6.61 ARK, played a 20-track playlist with next/previous"). If you could only build it, say so.
- **CI must pass.** The build treats warnings as errors and runs `scripts/verify_artifacts.py`. Code that runs on the Media Engine must not use syscalls or `$gp`; the check enforces this.

## Bug reports

Open an issue with:
- the PSP model, firmware and CFW,
- what you did and what happened,
- `spotify.log` from the game folder (`ms0:/PSP/GAME/PSPOTIFY/`).

The log contains your Spotify username but no passwords or tokens. Still, look through it before you post it.

## Building

See [Building](README.md#building) in the README. Pushes to any branch build in GitHub Actions too, so you can test a change without a local toolchain.
