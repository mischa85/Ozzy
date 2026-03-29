# Ozzy — USB Audio Driver Project

## Project
Cross-platform USB audio driver for Ploytec-based devices (Linux kernel module + macOS kext/dext).

## Reverse Engineering
This project involves ongoing reverse engineering of the Ploytec USB protocol. RE findings, open questions, and suspicions are maintained in `.claude/re/`:
- `ploytec_device_table.md` — all devices, init flow, class layout, PCM codecs, dispatch table
- `ploytec_bulk_devices.md` — USB High Speed flag and transfer type selection (isPloytecBulkDevice is really isPloytecHighSpeedDevice)
- `ploytec_vendor_requests.md` — all known USB control requests ('I','V','A','B','M','T', etc.)
- `ploytec_vendor_request_I.md` — deep dive on vendor request 0x49 registers and bit fields
- `open_questions.md` — unresolved questions and investigation leads

When new RE findings come up in conversation, update these files. Treat protocol definitions as evolving — flag anything that looks partially understood.

## Code Style
- MIT license only (no GPL anywhere)
- Use `/* */` for method/function documentation
- Prefer structs and enums over #defines to make protocol code self-documenting
- Prefer zero-copy designs
