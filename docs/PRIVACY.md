# Security and Privacy

This repository defaults to **development mode**: pairing and encryption are not required, and an unpaired client can write Recognition Result. Settings are centralized in `components/board_support/include/board_config.hpp`, not scattered. This is suitable only for a controlled demo.

Without authenticated encryption, a nearby client can connect, trigger captures, observe cropped JPEGs, or forge a recognition result. A product must require LE Secure Connections pairing/bonding, encrypted authenticated Control/Result writes, one authorized peer identity, key rotation/removal, replay protection beyond recent request IDs, and an application signature/MAC or secure session over GATT. It should not trust a display name as identity.

Received JPEGs are not saved by the PC client by default. Registration source photos are not stored in SQLite unless `--save-original` is explicitly used. SQLite always contains person IDs, display names, biometric embeddings, model metadata, hashes, and timestamps; treat it as sensitive personal/biometric data, restrict file ACLs, encrypt it at rest, define retention/deletion, and obtain consent where required.

Firmware logs display a recognized name in development mode. Set the central policy off for production and prefer opaque person IDs. Avoid uploading serial logs because they may contain identity and radio/location-related metrics.

The Phase 3 smoke-test app intentionally emits its cropped JPEG as `[JPEG_B64]` log chunks so the actual device output can be restored and decoded on the PC before BLE is introduced. This behavior exists only in that thin test app, not the shared pipeline or Phase 6 firmware. Phase 3 logs therefore contain biometric image data and must be protected and deleted according to the test retention policy.

InsightFace code and pretrained model packs have separate licenses/usage restrictions. Review the exact model pack license and suitability (including non-commercial research restrictions that may apply to pretrained data) before distribution or product use. This demo's technical integration is not a license grant.

The demo differs from a product in physical hardening, secure boot/flash encryption, signed OTA, BLE authorization, encrypted database, privacy UI/consent, audit retention, liveness/anti-spoofing, model governance, threat modeling, and regulatory validation.
