<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# anyps5-retro-host protocol

The contract between the AnyPS5 launchers and `anyps5-retro-host`, the
separate program that runs emulator cores. Version 1 covers single-player
PS1.

- [docs/protocol.md](docs/protocol.md): the protocol.
- [fixtures/v1](fixtures/v1): every message, and whole conversations
  (valid and invalid), for the host's and the launcher's conformance tests.
- [tools/check_fixtures.py](tools/check_fixtures.py): checks the fixtures
  against the document. Run `python3 tools/check_fixtures.py`.

Everything here is MIT ([LICENSES/MIT.txt](LICENSES/MIT.txt)). The JSON
fixtures cannot hold a comment, so [REUSE.toml](REUSE.toml) gives their
licence. A launcher may copy any of it.
