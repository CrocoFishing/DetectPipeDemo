# Reproducible Dependencies

Baseline dependency record follows. Event 1 integration, built on 2026-10-04, pins ESP-SR 2.5.3 and ESP-DL 3.3.11 because ESP-SR requires ESP-DL >=3.3.10; the detector wrapper and selected XGA / ESPDet configuration remain the same. The integrated app's generated `firmware/apps/event1_audio_face/dependencies.lock` records the exact resolved hashes. Legacy offline regression builds use the cached baseline ESP-DL 3.3.9 packages; a fresh normal build follows the updated 3.3.11 manifest.

| Dependency | Version/tag | Commit or component SHA-256 | Role |
|---|---|---|---|
| ESP-IDF | v6.0 (`v6.0-dirty` local describe) | Git `662a3be354759d9487bf4b1a629fadb766cb1800` | framework/toolchain |
| ESP-DL | 3.3.9 | upstream Git `12c0616de145b704e1149c474b9a1e852e631d67`; registry SHA-256 `0d5dd7fadd04854a82e1404f871e16980002a46dfe9796e52626d3c8d6566ad2` | inference runtime |
| human_face_detect | 0.5.0 | registry SHA-256 `a413ff0571a9823e82cc391e633bedae5f43cb950611e5ed3439a77ab754808b` | fixed ESPDet model wrapper/artifact |
| ESP-WHO | v1.1.0 | Git `205da6d3193989794a68c49f0b7dcf524bff5a74` | pinned architectural reference; not linked because its legacy face module uses a different detector API |
| esp32-camera | 2.1.7 | Git `202df95d7b1dc72e9303ad78f47b8dc9f339e6a1`; registry SHA-256 `bc9c8a6b51df777a014fa295825b3de5069bc0300c317acff20c97cf4a10ac7d` | OV3660/DVP/JPEG conversion |
| dl_fft | 0.6.0 | registry SHA-256 `a955ca29581de2177ddf25759273bd83f082b3c805e735fe6ee105858643989b` | ESP-DL transitive |
| esp_new_jpeg | 1.0.2 | registry SHA-256 `e1e7e6991193c41ed6c9fc386702aa7821433319e7aa9c3147cd3236310cd69c` | ESP-DL transitive |
| esp_jpeg | 1.3.1 | registry SHA-256 `defb83669293cbf86d0fa86b475ba5517aceed04ed70db435388c151ab37b5d7` | camera transitive |

Every app contains the generated `dependencies.lock`; manifests pin direct versions exactly, not `master`. The upstream S3 file is named `espdet_pico_224_224_face.espdl`; the repository's configuration name `espdet_pico_224_224_face_s8_s3` identifies the exact S8/S3 variant required by this demo. No other ESP-side detector is enabled.

Python used for validation: CPython 3.13.7. Locked direct packages: Bleak 1.1.1, InsightFace 0.7.3, ONNX Runtime 1.22.1, NumPy 2.2.5, OpenCV headless 4.12.0.88, PyYAML 6.0.3, pytest 9.0.3, ruff 0.12.12, and mypy 1.17.1. `requirements.lock` also pins build tools. InsightFace 0.7.3 was successfully built locally as a CPython 3.13 Windows wheel; another host may require MSVC Build Tools or may prefer Python 3.11.

Build commands:

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\tools\build_all.ps1
powershell.exe -ExecutionPolicy Bypass -File .\tools\create_python_venv.ps1
.\pc_client\.venv\Scripts\python.exe -m pip install -e .\pc_client
```

Known constraints: ESP-IDF must be in `C:\esp\v6.0\esp-idf` with the specified PowerShell profile, flash/PSRAM are configured for the XIAO S3 Sense 8 MB device, the model pack requires a large app partition, Event v2 messages require ATT MTU at least 67 bytes (image header alone: 36), and InsightFace model weights download separately on first use and have their own license terms.
