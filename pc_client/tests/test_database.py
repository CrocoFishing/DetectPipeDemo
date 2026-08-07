import sqlite3

import numpy as np
import pytest

from pc_client.database import DatabaseModelMismatch, FaceDatabase
from pc_client.protocol import StatusCode
from pc_client.recognizer import recognize_embedding


def test_schema_and_multiple_samples(tmp_path) -> None:
    path = tmp_path / "faces.db"
    with FaceDatabase(path) as database:
        database.add_sample(
            person_id="P001",
            name="Alice",
            embedding=np.array([1, 0], np.float32),
            model_name="test-model",
            image_bytes=b"one",
        )
        database.add_sample(
            person_id="P001",
            name="Alice",
            embedding=np.array([0.9, 0.1], np.float32),
            model_name="test-model",
            image_bytes=b"two",
        )
        assert database.list_people()[0].sample_count == 2
    connection = sqlite3.connect(path)
    assert {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")} >= {
        "persons",
        "face_samples",
        "settings",
        "schema_version",
    }
    assert connection.execute("SELECT source_image FROM face_samples LIMIT 1").fetchone()[0] is None


def test_model_mismatch(tmp_path) -> None:
    with FaceDatabase(tmp_path / "faces.db") as database:
        database.add_sample(
            person_id="P001",
            name="Alice",
            embedding=np.array([1, 0], np.float32),
            model_name="model-a",
            image_bytes=b"x",
        )
        with pytest.raises(DatabaseModelMismatch):
            database.add_sample(
                person_id="P002",
                name="Bob",
                embedding=np.array([1, 0, 0], np.float32),
                model_name="model-b",
                image_bytes=b"y",
            )


def test_recognition_and_unknown(tmp_path) -> None:
    with FaceDatabase(tmp_path / "faces.db") as database:
        database.add_sample(
            person_id="P001",
            name="Alice",
            embedding=np.array([1, 0], np.float32),
            model_name="test",
            image_bytes=b"x",
        )
        known = recognize_embedding(np.array([0.99, 0.01], np.float32), database, "test", 0.8)
        unknown = recognize_embedding(np.array([0, 1], np.float32), database, "test", 0.8)
        assert known.status is StatusCode.OK and known.person_id == "P001"
        assert unknown.status is StatusCode.UNKNOWN and unknown.person_id == ""
        assert database.remove_person("P001")
        assert not database.list_people()


def test_empty_database_returns_unknown(tmp_path) -> None:
    with FaceDatabase(tmp_path / "empty.db") as database:
        result = recognize_embedding(np.array([1, 0], np.float32), database, "test", 0.5)
        assert result.status is StatusCode.UNKNOWN
