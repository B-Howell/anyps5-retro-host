#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

# Applies the host's SwanStation patches (docs/host.md "SwanStation patches"), in the order of their
# names, to a clean SwanStation checkout at the SWANSTATION_COMMIT the workflows name. Each must
# apply cleanly; any that does not stops the build. The patches go into git's index, whose files
# have the repository's LF line endings, and the files are then checked out again, so the patches
# also apply where git writes CRLF (Windows, core.autocrlf).
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo 'Usage: cores/apply-swanstation-patches.sh SWANSTATION_CHECKOUT' >&2
  exit 2
fi

patches=$(cd "$(dirname "$0")/patches" && pwd)
for patch in "$patches"/swanstation-*.patch; do
  git -C "$1" apply --cached --check "$patch"
  git -C "$1" apply --cached "$patch"
  echo "applied $(basename "$patch")"
done
git -C "$1" checkout -- .
