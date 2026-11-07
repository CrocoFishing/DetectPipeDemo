"""Reconstruct and verify Event 1 associations from a copied SD flat directory."""

from __future__ import annotations

import argparse
import csv
import json
import re
import wave
from pathlib import Path
from typing import Any


def reconstruct(directory: Path, *, verify_wav: bool = True) -> list[dict[str, Any]]:
    events: dict[str, dict[str, Any]] = {}

    def owner(key: str) -> dict[str, Any]:
        if not re.fullmatch(r"[0-9A-F]{16}", key) or int(key, 16) == 0:
            raise ValueError(f"invalid Event ID: {key}")
        return events.setdefault(
            key, {"event_id": key, "lifecycle": [], "audio": None, "faces": []}
        )

    def rows(name: str) -> list[dict[str, str]]:
        path = directory / name
        if not path.exists():
            return []
        with path.open(encoding="utf-8", newline="") as handle:
            return [row for row in csv.DictReader(handle) if row.get("event_id")]

    for row in rows("event_lifecycle.csv"):
        event = owner(row["event_id"])
        lifecycle = event["lifecycle"]
        if row["state"] == "OPEN" and any(r["state"] == "OPEN" for r in lifecycle):
            raise ValueError(f"duplicate OPEN for {row['event_id']}")
        if row["state"] == "COMPLETE" and any(
            r["state"] == "COMPLETE" for r in lifecycle
        ):
            raise ValueError(f"duplicate COMPLETE for {row['event_id']}")
        lifecycle.append(row)
    for row in rows("audio_events.csv"):
        event = owner(row["event_id"])
        if event["audio"] is not None:
            raise ValueError(f"duplicate audio owner {row['event_id']}")
        if row["audio_source"] != "RAW":
            raise ValueError(f"unexpected audio source for {row['event_id']}")
        if (
            int(row["sample_rate"]),
            int(row["channels"]),
            int(row["bits_per_sample"]),
        ) != (16000, 1, 16):
            raise ValueError(f"invalid audio format for {row['event_id']}")
        expected_name = f"event_{row['event_id']}.wav"
        if row["audio_file"] != expected_name:
            raise ValueError(
                f"WAV filename disagrees with Event ID: {row['audio_file']}"
            )
        event["audio"] = row
        if row["audio_status"] == "READY":
            first, start, end, last = (
                int(row[field])
                for field in (
                    "audio_first_sample",
                    "vad_start_sample",
                    "vad_end_sample",
                    "audio_last_sample",
                )
            )
            if not first <= start <= end <= last or first == last:
                raise ValueError(f"invalid sample boundaries for {row['event_id']}")
            row["has_preroll"] = str(first < start)
            if verify_wav:
                with wave.open(str(directory / expected_name), "rb") as wav:
                    if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (
                        16000,
                        1,
                        2,
                    ):
                        raise ValueError(f"invalid WAV format for {row['event_id']}")
                    if wav.getnframes() != last - first:
                        raise ValueError(
                            f"WAV/sample metadata discontinuity for {row['event_id']}"
                        )
                    if len(wav.readframes(wav.getnframes())) != 2 * (last - first):
                        raise ValueError(f"truncated WAV for {row['event_id']}")
    image_owners: dict[tuple[str, int], tuple[str, int]] = {}
    request_owners: dict[tuple[str, int], str] = {}
    for row in rows("face_associations.csv"):
        key = row["event_id"]
        event = owner(key)
        request_id, image_id, index, count = (
            int(row[field])
            for field in ("request_id", "image_id", "face_index", "face_count")
        )
        if request_id:
            request_key = key[:8], request_id
            previous = request_owners.setdefault(request_key, key)
            if previous != key:
                raise ValueError("capture request crossed Event owners")
            if event.get("request_id", request_id) != request_id:
                raise ValueError("Event has multiple capture requests")
            event["request_id"] = request_id
        if image_id:
            if not request_id or not 0 <= index < count:
                raise ValueError("invalid face sequence")
            image_key = key[:8], image_id
            if image_key in image_owners:
                raise ValueError("duplicate/crossed image owner")
            image_owners[image_key] = key, request_id
            if any(int(f["face_index"]) == index for f in event["faces"]):
                raise ValueError("duplicate face index")
            if event["faces"] and int(event["faces"][0]["face_count"]) != count:
                raise ValueError("face_count changed within Event")
            event["faces"].append(row)
        elif index != -1 or count < 0:
            raise ValueError("invalid terminal sentinel")
        else:
            if "batch_terminal_status" in event:
                raise ValueError("duplicate batch terminal sentinel")
            event["batch_terminal_status"] = row["status"]
            event["face_status"] = row["status"]
            event["expected_face_count"] = count
    for event in events.values():
        event["faces"].sort(key=lambda f: int(f["face_index"]))
        for transition in event["lifecycle"]:
            if transition["state"] == "FACE_BATCH_END":
                event["face_status"] = transition["status"]
            elif transition["state"] == "COMPLETE":
                event["status"] = transition["status"]
        event.setdefault("status", "INCOMPLETE")
        if event["status"] in ("COMPLETE", "COMPLETE_WITH_ERROR"):
            errors = []
            transitions = event["lifecycle"]
            states = [row["state"] for row in transitions]
            for required in ("OPEN", "AUDIO_READY", "FACE_BATCH_END", "COMPLETE"):
                if states.count(required) != 1:
                    errors.append(
                        f"missing or duplicated {required} lifecycle evidence"
                    )
            if not errors:
                if states.index("OPEN") != 0 or any(
                    states.index(branch) > states.index("COMPLETE")
                    for branch in ("AUDIO_READY", "FACE_BATCH_END")
                ):
                    errors.append("lifecycle transitions out of order")
                audio_transition = next(
                    row for row in transitions if row["state"] == "AUDIO_READY"
                )
                if (
                    not event["audio"]
                    or audio_transition["status"] != event["audio"]["audio_status"]
                ):
                    errors.append(
                        "audio terminal metadata missing or disagrees with lifecycle"
                    )
                if event.get("batch_terminal_status") != event.get("face_status"):
                    errors.append(
                        "face terminal sentinel missing or disagrees with lifecycle"
                    )
            if errors:
                event["declared_status"] = event["status"]
                event["status"] = "INCOMPLETE"
                event["validation_errors"] = errors
        if event["status"] == "COMPLETE":
            if not event["audio"] or event["audio"]["audio_status"] != "READY":
                raise ValueError("successful COMPLETE lacks READY audio")
            if event.get("face_status") not in ("COMPLETED", "NO_FACE"):
                raise ValueError("successful COMPLETE lacks successful face terminal")
            if event["faces"] and len(event["faces"]) != int(
                event["faces"][0]["face_count"]
            ):
                raise ValueError("successful COMPLETE missing face associations")
            if event["face_status"] == "COMPLETED" and len(event["faces"]) != event.get(
                "expected_face_count"
            ):
                raise ValueError("successful COMPLETE disagrees with batch count")
    if not events:
        raise ValueError("no Event metadata found")
    return [events[key] for key in sorted(events)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument(
        "--metadata-only", action="store_true", help="skip WAV byte validation"
    )
    parser.add_argument("--output", type=Path, help="write reconstructed JSON")
    args = parser.parse_args()
    result = reconstruct(args.directory, verify_wav=not args.metadata_only)
    serialized = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(serialized + "\n", encoding="utf-8")
    else:
        print(serialized)
    if any(event.get("validation_errors") for event in result):
        raise SystemExit(2)


if __name__ == "__main__":
    main()
