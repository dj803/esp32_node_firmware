"""
modify_link_path.py — PlatformIO extra_script (pre-link stage)

1. Prepends the project-local ld/ directory to the linker search path so
   ld/memory.ld is found BEFORE the framework package's copy. This allows us
   to extend iram0_0_seg by 2 KB (into SRAM1) for the ESP-IDF sleep driver
   (v0.3.20).

2. Conditionally injects FIRMWARE_VERSION_OVERRIDE *only when* the
   FIRMWARE_VERSION env var is set and non-empty (v0.4.07).
   The previous unconditional `-DFIRMWARE_VERSION_OVERRIDE=\"${sysenv...}\"`
   in platformio.ini's build_flags expanded to `""` for local builds,
   defeating config.h's #ifdef-based fallback (which can't tell empty-string
   from undefined). Result: locally-flashed devices reported empty firmware
   in MQTT heartbeats. See SUGGESTED_IMPROVEMENTS.txt #43.
"""
import os
Import("env")  # noqa: F821 — SCons magic, not a regular import

ld_dir = os.path.join(env["PROJECT_DIR"], "ld").replace("\\", "/")
env.Prepend(LINKFLAGS=["-L" + ld_dir])

fw_version_env = os.environ.get("FIRMWARE_VERSION", "").strip()
if fw_version_env:
    env.Append(CPPDEFINES=[("FIRMWARE_VERSION_OVERRIDE", '\\"' + fw_version_env + '\\"')])
