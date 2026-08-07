"""Convert canonical ESP serial state/metric lines to CSV and JSONL evidence."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

METRIC = re.compile(
    r"\[METRIC\]\s+phase=(?P<phase>\d+)\s+image_id=(?P<image_id>\d+)\s+"
    r"metric=(?P<metric>\S+)\s+value=(?P<value>[-+\d.]+)\s+unit=(?P<unit>\S+)"
)
STATE = re.compile(
    r"\[STATE\]\s+from=(?P<from>\S+)\s+to=(?P<to>\S+)\s+"
    r"image_id=(?P<image_id>\d+)\s+trigger=(?P<trigger>\S+)"
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--metrics-csv", type=Path, required=True)
    parser.add_argument("--events-jsonl", type=Path, required=True)
    args = parser.parse_args()
    args.metrics_csv.parent.mkdir(parents=True, exist_ok=True)
    args.events_jsonl.parent.mkdir(parents=True, exist_ok=True)
    metrics: list[dict[str, str]] = []
    events: list[dict[str, str]] = []
    for line_number, line in enumerate(args.log.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if match := METRIC.search(line):
            metrics.append({"line": str(line_number), **match.groupdict()})
        if match := STATE.search(line):
            events.append({"line": str(line_number), **match.groupdict()})
    with args.metrics_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["line", "phase", "image_id", "metric", "value", "unit"])
        writer.writeheader()
        writer.writerows(metrics)
    with args.events_jsonl.open("w", encoding="utf-8") as handle:
        for event in events:
            handle.write(json.dumps(event, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()

