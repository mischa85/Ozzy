## 🛑 One fix per PR

Got it working after changing ten things? Great. Now find out which change actually fixed it,
and submit only that one. PRs that bundle unrelated "fixes" will be closed.

## What does this fix?

<!-- One or two sentences. Link the issue (e.g. Fixes #72). -->

## Why is this the right fix?

<!--
What's your evidence? Pick what applies:
- Matches the official Ploytec driver (see .claude/re/ for what's already known)
- USB capture from the official Windows/macOS driver (attach it, compressed)
- Before/after debug log

"Claude/ChatGPT said so" is not evidence.
-->

## Tested on

- Device:
- Audio firmware version (from the debug log):
- OS + version:
- Backend (macOS kext / dext / Linux):

## Logs

<!-- Attach a before AND after debug log (macOS: debuglogs.command, Linux: dmesg). -->

## Checklist

- [ ] This PR contains **one** change. No unrelated refactors, renames or formatting.
- [ ] I tested with **only this change** applied on top of `main`.
- [ ] I checked `.claude/re/` and this doesn't contradict what's known about the official driver (or I explain why above).
- [ ] If this touches shared code: I considered what it does to the other devices (DB4, DB2, DX, 4D, Wizard 4).
