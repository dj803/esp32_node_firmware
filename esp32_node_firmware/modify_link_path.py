"""
modify_link_path.py — PlatformIO extra_script (pre-link stage)

Prepends the project-local ld/ directory to the linker search path so that
ld/memory.ld is found BEFORE the framework package's copy. This allows us to
extend iram0_0_seg by 2 KB (into SRAM1) to accommodate the ESP-IDF sleep
driver's IRAM_ATTR functions, added in v0.3.20 for cmd/sleep + cmd/deep_sleep.

The framework's -L paths are added by PlatformIO's framework integration scripts.
env.Prepend ensures our path comes first in the search order.
"""
import os
Import("env")  # noqa: F821 — SCons magic, not a regular import

ld_dir = os.path.join(env["PROJECT_DIR"], "ld").replace("\\", "/")
env.Prepend(LINKFLAGS=["-L" + ld_dir])
