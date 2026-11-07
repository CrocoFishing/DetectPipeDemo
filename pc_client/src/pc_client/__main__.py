from __future__ import annotations

import argparse
import asyncio
import json
import logging
import time
from pathlib import Path
from typing import TYPE_CHECKING

from .ble_client import BleFaceSession, ReceivedFaceImage, append_metrics_csv, scan_devices
from .database import FaceDatabase
from .protocol import StatusCode
from .recognizer import recognize_embedding

if TYPE_CHECKING:
    from .face_engine import InsightFaceEngine


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="python -m pc_client")
    root.add_argument("--database", default="face_demo.db")
    root.add_argument("--event-database", default="event_store.db")
    root.add_argument("--model", default="buffalo_l")
    root.add_argument("--threshold", type=float, default=0.45)
    root.add_argument("--verbose", action="store_true")
    commands = root.add_subparsers(dest="command", required=True)
    register = commands.add_parser("register")
    register.add_argument("--person-id", required=True)
    register.add_argument("--name", required=True)
    register.add_argument("--image", required=True)
    register.add_argument("--allow-largest-face", action="store_true")
    register.add_argument("--save-original", action="store_true")
    commands.add_parser("list")
    remove = commands.add_parser("remove")
    remove.add_argument("--person-id", required=True)
    recognize = commands.add_parser("recognize-file")
    recognize.add_argument("--image", required=True)
    commands.add_parser("scan").add_argument("--timeout", type=float, default=10.0)
    run = commands.add_parser("run")
    run.add_argument("--device-name", default="ESP32S3-FACE-DEMO")
    run.add_argument("--wait-only", action="store_true")
    test = commands.add_parser("ble-test")
    test.add_argument("--device-name", default="ESP32S3-FACE-DEMO")
    test.add_argument("--sizes-kb", default="20,50,100")
    test.add_argument("--repeat", type=int, default=10)
    test.add_argument("--csv", default="../results/phase05/actual/ble_metrics.csv")
    return root


def engine(model: str) -> InsightFaceEngine:  # InsightFace import is intentionally lazy and expensive
    from .face_engine import InsightFaceEngine

    return InsightFaceEngine(model)


async def run_client(args: argparse.Namespace) -> None:
    face_engine = engine(args.model)
    database = FaceDatabase(args.database)
    session: BleFaceSession

    async def on_image(image: ReceivedFaceImage) -> None:
        started = time.perf_counter()
        try:
            vector = face_engine.embedding_from_bytes(image.jpeg)
            found = recognize_embedding(vector, database, args.model, args.threshold)
            status = found.status
            person_id = found.person_id
            person_name = found.person_name
            similarity = found.similarity
        except Exception as exc:
            logging.exception("recognition failed")
            status = StatusCode.FAILED
            person_id = ""
            person_name = str(exc)[:64]
            similarity = 0.0
        processing_time_ms = int((time.perf_counter() - started) * 1000)
        try:
            await session.send_recognition_result(
                image.request_id,
                image.image_id,
                status,
                person_id,
                person_name,
                similarity,
                processing_time_ms,
                event_id=image.event_id,
            )
        except Exception:
            logging.exception("result delivery failed; EventStore retains recognition")
            return
        logging.info(
            "face_result request_id=%d image_id=%d face=%d/%d status=%s person_id=%s "
            "name=%s similarity=%.4f processing_ms=%d",
            image.request_id,
            image.image_id,
            image.face_index + 1,
            image.face_count,
            status.name,
            person_id,
            person_name,
            similarity,
            processing_time_ms,
        )
        summary = session.complete_face(image, status)
        if summary:
            logging.info(
                "face_batch_complete request_id=%d faces=%d ok=%d unknown=%d failed=%d",
                summary.request_id,
                summary.face_count,
                summary.ok_count,
                summary.unknown_count,
                summary.failed_count,
            )

    session = BleFaceSession(args.device_name, on_image, event_database=args.event_database)
    try:
        await session.connect()
        if not args.wait_only:
            await session.start_capture(1)
        while True:
            await asyncio.sleep(1)
    finally:
        database.close()
        await session.disconnect()
        session.close()


async def ble_test(args: argparse.Namespace) -> None:
    sizes = [int(value) * 1000 for value in args.sizes_kb.split(",")]
    session = BleFaceSession(args.device_name, event_database=args.event_database)
    try:
        await session.connect()
        request_id = 100
        for size in sizes:
            for _ in range(args.repeat):
                await session.start_capture(request_id, size)
                await asyncio.wait_for(session.image_complete.wait(), timeout=20)
                append_metrics_csv(args.csv, session.metrics())
                request_id += 1
                await asyncio.sleep(1)
    finally:
        await session.disconnect()
        session.close()


def main() -> None:
    args = parser().parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s"
    )
    if args.command == "register":
        embedding, image = engine(args.model).embedding_from_file(
            args.image, allow_largest=args.allow_largest_face
        )
        with FaceDatabase(args.database) as database:
            sample_id = database.add_sample(
                person_id=args.person_id,
                name=args.name,
                embedding=embedding,
                model_name=args.model,
                image_bytes=image,
                save_original=args.save_original,
            )
        print(json.dumps({"sample_id": sample_id, "person_id": args.person_id}, ensure_ascii=False))
    elif args.command == "list":
        with FaceDatabase(args.database) as database:
            print(
                json.dumps(
                    [
                        {"person_id": p.person_id, "name": p.name, "samples": p.sample_count}
                        for p in database.list_people()
                    ],
                    ensure_ascii=False,
                    indent=2,
                )
            )
    elif args.command == "remove":
        with FaceDatabase(args.database) as database:
            removed = database.remove_person(args.person_id)
        print(json.dumps({"person_id": args.person_id, "removed": removed}))
    elif args.command == "recognize-file":
        vector, _ = engine(args.model).embedding_from_file(Path(args.image))
        with FaceDatabase(args.database) as database:
            result = recognize_embedding(vector, database, args.model, args.threshold)
        print(
            json.dumps(
                {
                    "status": result.status.name,
                    "person_id": result.person_id,
                    "person_name": result.person_name,
                    "similarity": result.similarity,
                    "processing_time_ms": result.processing_time_ms,
                },
                ensure_ascii=False,
            )
        )
    elif args.command == "scan":
        print(json.dumps(asyncio.run(scan_devices(args.timeout)), ensure_ascii=False, indent=2))
    elif args.command == "run":
        asyncio.run(run_client(args))
    elif args.command == "ble-test":
        asyncio.run(ble_test(args))


if __name__ == "__main__":
    main()
