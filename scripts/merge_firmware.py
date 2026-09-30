# PlatformIO post-build script: merge bootloader + partitions + app into one .bin
# Usage: pio run -t merge -e c6touch

Import("env")

import os
from os.path import join


def _flash_freq(env):
    raw = str(env.BoardConfig().get("build.f_flash", "40000000L"))
    digits = "".join(ch for ch in raw if ch.isdigit())
    return {
        "80000000": "80m",
        "40000000": "40m",
        "26000000": "26m",
        "20000000": "20m",
    }.get(digits, "40m")


def merge_firmware(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    progname = env.subst("${PROGNAME}")
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    esptool = join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    boot_app0 = join(framework_dir, "tools", "partitions", "boot_app0.bin")
    merged = join(build_dir, "firmware-merged.bin")
    mcu = env.BoardConfig().get("build.mcu", "esp32c6")
    flash_size = env.BoardConfig().get("upload.flash_size", "16MB")
    flash_mode = env.BoardConfig().get("build.flash_mode", "dio")
    flash_freq = _flash_freq(env)

    bootloader = join(build_dir, "bootloader.bin")
    partitions = join(build_dir, "partitions.bin")
    firmware = join(build_dir, f"{progname}.bin")

    for path, label in (
        (bootloader, "bootloader.bin"),
        (partitions, "partitions.bin"),
        (boot_app0, "boot_app0.bin"),
        (firmware, f"{progname}.bin"),
    ):
        if not os.path.isfile(path):
            raise FileNotFoundError(f"Missing {label}: {path}")

    cmd = [
        env.subst("$PYTHONEXE"),
        esptool,
        "--chip",
        mcu,
        "merge_bin",
        "-o",
        merged,
        "--flash_mode",
        str(flash_mode),
        "--flash_freq",
        flash_freq,
        "--flash_size",
        str(flash_size),
        "0x0",
        bootloader,
        "0x8000",
        partitions,
        "0xe000",
        boot_app0,
        "0x10000",
        firmware,
    ]
    print(f"Merging flash image -> {merged}")
    env.Execute(" ".join(f'"{c}"' if " " in c else c for c in cmd))
    return None


env.AddCustomTarget(
    name="merge",
    dependencies="${BUILD_DIR}/${PROGNAME}.bin",
    actions=env.Action(merge_firmware, "Merging flash image for web flasher"),
    title="Merge firmware",
    description="Create firmware-merged.bin (bootloader + partitions + app)",
)
