#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

# Applies the host's SwanStation patches (docs/host.md "SwanStation patches"), in the order of their
# names, to a clean SwanStation checkout at the SWANSTATION_COMMIT the workflows name. Each must
# apply cleanly; any that does not stops the build. The patches go into git's index, whose files
# have the repository's LF line endings, and the files are then checked out again, so the patches
# also apply where git writes CRLF (Windows, core.autocrlf). Last it writes anyps5-core-source.txt
# into the checkout, which the sixth patch turns into the core's build identity (docs/netplay.md
# "Core build identity"): the SwanStation commit and each patch's SHA-256 and name, in the order
# applied, one per line with LF endings. The patches are LF in every checkout (.gitattributes).
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo 'Usage: cores/apply-swanstation-patches.sh SWANSTATION_CHECKOUT' >&2
  exit 2
fi

sha256() {
  if command -v sha256sum >/dev/null; then
    sha256sum < "$1" | cut -d ' ' -f 1
  else
    shasum -a 256 < "$1" | cut -d ' ' -f 1
  fi
}

patches=$(cd "$(dirname "$0")/patches" && pwd)
manifest="anyps5-retro-host core source 1
swanstation $(git -C "$1" rev-parse HEAD)
"
for patch in "$patches"/swanstation-*.patch; do
  git -C "$1" apply --cached --check "$patch"
  git -C "$1" apply --cached "$patch"
  manifest+="patch $(sha256 "$patch") $(basename "$patch")
"
  echo "applied $(basename "$patch")"
done
git -C "$1" checkout -- .
printf '%s' "$manifest" > "$1/anyps5-core-source.txt"
echo "wrote anyps5-core-source.txt:"
cat "$1/anyps5-core-source.txt"
