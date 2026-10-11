"""Pinned PSP build, runnable package and artifact checks (Windows or Linux)."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PINS = {
    "custom-core": ("https://github.com/mcidclan/psp-media-engine-custom-core.git",
                    "fe871e12754060f3b42fbca6b251bb8ee3ade453", "custom-core-build.patch"),
    "safe-task": ("https://github.com/mcidclan/psp-media-engine-safe-task.git",
                  "7d4c41f77b0be8815a720e0c8212ed504c01806d", "safe-task.patch"),
}
# Source-only dependencies compiled directly by CMakeLists.txt (no patch, no install).
SOURCE_PINS = {
    "bearssl": ("https://www.bearssl.org/git/BearSSL",
                "7bea48e5e850ab4cafbe68d3765cdaba13a86d6f"),
    "stb": ("https://github.com/nothings/stb.git",
            "2c980bb59875b0d32144a71867fbdebb2f77cd20"),
}


def run(*args, cwd=ROOT, capture=False):
    result = subprocess.run([str(a) for a in args], cwd=cwd, check=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.decode("utf-8", errors="replace").strip() if capture else None


def configure(cmake, source, build, generator, *options):
    run(cmake, "-S", source, "-B", build, "-G", generator,
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.10", *options)


def fetch_sources():
    for name, (url, revision) in SOURCE_PINS.items():
        source = ROOT / ".deps" / name
        if not source.exists():
            run("git", "clone", url, source)
            run("git", "-C", source, "checkout", "--detach", revision)
        if run("git", "-C", source, "rev-parse", "HEAD", capture=True) != revision:
            raise RuntimeError(f"{name}: wrong dependency revision; refusing to reset local work")


def prepare_dependencies(cmake, generator):
    for name, (url, revision, patch_name) in PINS.items():
        source = ROOT / ".deps" / name
        patch = ROOT / "patches" / patch_name
        if not source.exists():
            run("git", "clone", url, source)
            run("git", "-C", source, "checkout", "--detach", revision)
        if run("git", "-C", source, "rev-parse", "HEAD", capture=True) != revision:
            raise RuntimeError(f"{name}: wrong dependency revision; refusing to reset local work")
        reverse = subprocess.run(["git", "-C", str(source), "apply", "--reverse",
                                  "--check", str(patch)], capture_output=True)
        if reverse.returncode != 0:
            run("git", "-C", source, "apply", "--check", patch)
            run("git", "-C", source, "apply", patch)
        build = source / "build-pinned"
        options = ["-DPRX_FREE=0"] if name == "safe-task" else []
        configure(cmake, source, build, generator, *options)
        run(cmake, "--build", build, "--target", "install", "--parallel", "4")


def package(build):
    dist = ROOT / "dist"
    game = dist / "PSP" / "GAME" / "PSPOTIFY"
    debug = dist / "debug"
    game.mkdir(parents=True, exist_ok=True)
    debug.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / "EBOOT.PBP", game / "EBOOT.PBP")
    shutil.copy2(build / "psp-dualcore-actions", debug / "psp-dualcore-actions.elf")
    for name in ("psp-dualcore-actions.prx", "psp-dualcore-actions.map"):
        shutil.copy2(build / name, debug / name)
    bridge_build = ROOT / ".deps" / "safe-task" / "build-pinned"
    shutil.copy2(bridge_build / "kernel" / "kcall", debug / "kcall.elf")
    shutil.copy2(bridge_build / "kernel" / "kcall.prx", debug / "kcall.prx")
    shutil.copytree(ROOT / "licenses", game / "licenses", dirs_exist_ok=True)
    shutil.copy2(ROOT / "docs" / "SETUP.md", game / "SETUP.md")
    run(sys.executable, ROOT / "scripts" / "verify_artifacts.py", build, "--output", debug)
    metadata = {
        "status": "BUILD VERIFIED; REAL PSP-3000 TEST REQUIRED",
        "compiler": run("psp-gcc", "--version", capture=True).splitlines()[0],
        "dependencies": {**{name: revision for name, (_, revision, _) in PINS.items()},
                         **{name: revision for name, (_, revision) in SOURCE_PINS.items()}},
        "patches_sha256": {name: hashlib.sha256((ROOT / "patches" / patch).read_bytes()).hexdigest()
                           for name, (_, _, patch) in PINS.items()},
        "eboot_sha256": hashlib.sha256((game / "EBOOT.PBP").read_bytes()).hexdigest(),
        "eboot_bytes": (game / "EBOOT.PBP").stat().st_size,
    }
    (dist / "BUILD.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    with zipfile.ZipFile(dist / "PSPotify-ME.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted((dist / "PSP").rglob("*")):
            if path.is_file(): archive.write(path, path.relative_to(dist))
        archive.write(dist / "BUILD.json", "BUILD.json")
    print(metadata["status"])
    print(game / "EBOOT.PBP")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="Ninja" if os.name == "nt" else "Unix Makefiles")
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()
    if not os.environ.get("PSPDEV"):
        raise RuntimeError("PSPDEV must name the installed toolchain")
    fetch_sources()
    prepare_dependencies(args.cmake, args.generator)
    build = ROOT / args.build_dir
    configure(args.cmake, ROOT, build, args.generator)
    run(args.cmake, "--build", build, "--parallel", "4")
    package(build)


if __name__ == "__main__":
    main()
