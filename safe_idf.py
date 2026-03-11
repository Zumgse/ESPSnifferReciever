#!/usr/bin/env python3
"""
Run ESP-IDF commands from a path that is safe for shell command generation.

Why this exists:
Some ESP-IDF/CMake generated commands can fail when the project path contains
shell metacharacters (notably parentheses), resulting in errors like:
  /bin/sh: syntax error near unexpected token `('

Usage:
  ./safe_idf.py build
  ./safe_idf.py flash
  ./safe_idf.py -p /dev/ttyUSB0 flash monitor
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

UNSAFE_CHARS = set("()")
TARGET_CHIP = "esp32s3"  # Heltec WiFi LoRa 32 V3


def is_unsafe_path(path: Path) -> bool:
    s = str(path)
    return any(c in s for c in UNSAFE_CHARS)


def run_idf(cmd_args: list[str], cwd: Path) -> int:
    cmd = ["idf.py", *cmd_args]
    env = os.environ.copy()
    env.setdefault("IDF_TARGET", TARGET_CHIP)
    print(f"[safe_idf] target={env['IDF_TARGET']}, running: {shlex.join(cmd)}")
    return subprocess.call(cmd, cwd=cwd, env=env)


def main() -> int:
    project_dir = Path(__file__).resolve().parent
    args = sys.argv[1:] or ["build"]

    if not is_unsafe_path(project_dir):
        return run_idf(args, project_dir)

    # Work around path parsing failures by building from a symlink without unsafe chars.
    link_dir = Path.home() / "esp_safe_projects"
    link_dir.mkdir(parents=True, exist_ok=True)

    link_name = project_dir.name
    for c in "() []{}":
        link_name = link_name.replace(c, "_")
    safe_link = link_dir / link_name

    if safe_link.exists() or safe_link.is_symlink():
        if safe_link.resolve() != project_dir:
            if safe_link.is_symlink() or safe_link.is_file():
                safe_link.unlink()
            else:
                shutil.rmtree(safe_link)
    if not safe_link.exists():
        safe_link.symlink_to(project_dir, target_is_directory=True)

    print(
        "[safe_idf] project path contains unsafe shell chars; using symlink:\n"
        f"  project: {project_dir}\n"
        f"  symlink: {safe_link}"
    )

    return run_idf(args, safe_link)


if __name__ == "__main__":
    raise SystemExit(main())
