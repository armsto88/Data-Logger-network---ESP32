#!/usr/bin/env python3
"""PlatformIO pre-build hook: inject the git build id as FW_GIT.

Shared by node and mothership envs via:
    extra_scripts = pre:<relpath>/scripts/fw_version.py

Sets FW_GIT to the short commit hash plus "-dirty" when the working tree has
tracked/staged changes. This replaces __DATE__/__TIME__, which freeze across
cached reflashes (see memory: flash-recovery-bootloader). Falls back to "nogit"
when git is unavailable so bench builds still compile.
"""
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)


def _git(*args):
    try:
        return subprocess.check_output(
            ["git", *args], stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return ""


def _build_id():
    short = _git("rev-parse", "--short", "HEAD")
    if not short:
        return "nogit"
    dirty = ""
    # Non-zero exit from either check => uncommitted tracked/staged changes.
    unstaged = subprocess.call(
        ["git", "diff", "--quiet"], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL
    )
    staged = subprocess.call(
        ["git", "diff", "--cached", "--quiet"], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL
    )
    if unstaged != 0 or staged != 0:
        dirty = "-dirty"
    return short + dirty


build_id = _build_id()
env.Append(CPPDEFINES=[("FW_GIT", env.StringifyMacro(build_id))])  # noqa: F821
print(f"[fw_version] FW_GIT={build_id}")
