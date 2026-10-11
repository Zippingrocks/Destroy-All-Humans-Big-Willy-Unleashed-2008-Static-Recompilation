"""Reject private/generated artifacts from the public source tree."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BANNED_SUFFIXES = {
    ".dol", ".elf", ".exe", ".iso", ".rvz", ".sav", ".wad", ".wbfs",
}
BANNED_NAMES = {
    "controller-session.json",
    "furonlog.log",
    "recomp_crash.log",
}
PRIVATE_MARKERS = (
    b"C:\\Users\\Bilbo",
    b"D:\\.codex\\",
    b"github_pat_",
    b"ghp_",
)
MAX_FILE_SIZE = 25 * 1024 * 1024


def tracked_files() -> list[Path]:
    output = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=ROOT
    )
    return [ROOT / item.decode("utf-8") for item in output.split(b"\0") if item]


def main() -> int:
    errors: list[str] = []
    for path in tracked_files():
        relative = path.relative_to(ROOT).as_posix()
        if path.suffix.lower() in BANNED_SUFFIXES:
            errors.append(f"forbidden binary/game-data extension: {relative}")
        if path.name.lower() in BANNED_NAMES:
            errors.append(f"forbidden private/runtime file: {relative}")
        size = path.stat().st_size
        if size > MAX_FILE_SIZE:
            errors.append(f"file exceeds 25 MiB public-source limit: {relative}")
        if size <= 2 * 1024 * 1024 and relative != "tools/check_repository.py":
            data = path.read_bytes()
            for marker in PRIVATE_MARKERS:
                if marker.lower() in data.lower():
                    errors.append(
                        f"private path or credential marker {marker!r}: {relative}"
                    )

    if errors:
        print("Repository policy check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Repository policy check passed ({len(tracked_files())} tracked files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
