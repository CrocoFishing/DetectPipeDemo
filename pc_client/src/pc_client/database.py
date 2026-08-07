from __future__ import annotations

import hashlib
import sqlite3
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

import numpy as np
import numpy.typing as npt

SCHEMA_VERSION = 1


class DatabaseModelMismatch(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class Person:
    person_id: str
    name: str
    sample_count: int


class FaceDatabase:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.connection = sqlite3.connect(self.path)
        self.connection.execute("PRAGMA foreign_keys=ON")
        self._migrate()

    def close(self) -> None:
        self.connection.close()

    def __enter__(self) -> FaceDatabase:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def _migrate(self) -> None:
        self.connection.executescript("""
            CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS persons(
                person_id TEXT PRIMARY KEY, name TEXT NOT NULL, created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS face_samples(
                sample_id INTEGER PRIMARY KEY AUTOINCREMENT,
                person_id TEXT NOT NULL REFERENCES persons(person_id) ON DELETE CASCADE,
                embedding BLOB NOT NULL, embedding_dimension INTEGER NOT NULL,
                model_name TEXT NOT NULL, image_sha256 TEXT NOT NULL,
                source_image BLOB, created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
        """)
        row = self.connection.execute("SELECT version FROM schema_version LIMIT 1").fetchone()
        if row is None:
            self.connection.execute("INSERT INTO schema_version(version) VALUES(?)", (SCHEMA_VERSION,))
        elif row[0] != SCHEMA_VERSION:
            raise RuntimeError(f"unsupported database schema {row[0]}")
        self.connection.commit()

    def _ensure_model(self, model_name: str, dimension: int) -> None:
        settings = dict(self.connection.execute("SELECT key,value FROM settings"))
        if settings and (
            settings.get("model_name") != model_name
            or int(settings.get("embedding_dimension", -1)) != dimension
        ):
            raise DatabaseModelMismatch("database embedding model/dimension mismatch")
        self.connection.executemany(
            "INSERT OR REPLACE INTO settings(key,value) VALUES(?,?)",
            [("model_name", model_name), ("embedding_dimension", str(dimension))],
        )

    def add_sample(
        self,
        *,
        person_id: str,
        name: str,
        embedding: npt.NDArray[np.float32],
        model_name: str,
        image_bytes: bytes,
        save_original: bool = False,
    ) -> int:
        if not person_id.strip() or not name.strip():
            raise ValueError("person_id and name are required")
        vector = np.asarray(embedding, dtype=np.float32).reshape(-1)
        norm = float(np.linalg.norm(vector))
        if vector.size == 0 or norm <= 0:
            raise ValueError("invalid embedding")
        normalized = np.asarray(vector / np.float32(norm), dtype=np.float32)
        self._ensure_model(model_name, int(normalized.size))
        now = datetime.now(UTC).isoformat()
        self.connection.execute(
            "INSERT INTO persons(person_id,name,created_at) VALUES(?,?,?) "
            "ON CONFLICT(person_id) DO UPDATE SET name=excluded.name",
            (person_id, name, now),
        )
        cursor = self.connection.execute(
            "INSERT INTO face_samples(person_id,embedding,embedding_dimension,model_name,"
            "image_sha256,source_image,created_at) "
            "VALUES(?,?,?,?,?,?,?)",
            (
                person_id,
                normalized.tobytes(),
                normalized.size,
                model_name,
                hashlib.sha256(image_bytes).hexdigest(),
                image_bytes if save_original else None,
                now,
            ),
        )
        self.connection.commit()
        if cursor.lastrowid is None:
            raise RuntimeError("SQLite did not return a sample id")
        return cursor.lastrowid

    def list_people(self) -> list[Person]:
        rows = self.connection.execute(
            "SELECT p.person_id,p.name,COUNT(s.sample_id) FROM persons p "
            "LEFT JOIN face_samples s ON s.person_id=p.person_id "
            "GROUP BY p.person_id,p.name ORDER BY p.person_id"
        )
        return [Person(str(r[0]), str(r[1]), int(r[2])) for r in rows]

    def remove_person(self, person_id: str) -> bool:
        cursor = self.connection.execute("DELETE FROM persons WHERE person_id=?", (person_id,))
        self.connection.commit()
        return cursor.rowcount > 0

    def embeddings(self, model_name: str, dimension: int) -> list[tuple[str, str, npt.NDArray[np.float32]]]:
        self._ensure_model(model_name, dimension)
        rows = self.connection.execute(
            "SELECT p.person_id,p.name,s.embedding FROM face_samples s "
            "JOIN persons p ON p.person_id=s.person_id WHERE s.model_name=? "
            "AND s.embedding_dimension=?",
            (model_name, dimension),
        )
        return [
            (str(pid), str(name), np.frombuffer(blob, dtype=np.float32).copy()) for pid, name, blob in rows
        ]
