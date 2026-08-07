"""Run the YAML/Python/C++/Swift consistency tests with the repository venv."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    return subprocess.call(
        [sys.executable, "-m", "pytest", str(ROOT / "pc_client/tests/test_protocol_consistency.py"), "-q"],
        cwd=ROOT,
    )


if __name__ == "__main__":
    raise SystemExit(main())
