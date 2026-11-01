"""Keep the app-facing SD examples executable against the actual inspector."""
from __future__ import annotations

import json
import runpy
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RECONSTRUCT = runpy.run_path(str(ROOT / "tools" / "inspect_event_sd.py"))["reconstruct"]


@pytest.mark.parametrize("scenario", ["complete", "busy", "incomplete"])
def test_documented_event_artifacts_reconstruct_exactly(scenario: str) -> None:
    directory = ROOT / "protocol" / "examples" / "event_data" / scenario
    expected = json.loads((directory / "expected.diagnostic.json").read_text(encoding="utf-8"))
    assert RECONSTRUCT(directory) == expected
