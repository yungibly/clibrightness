#!/bin/bash
set -euo pipefail

version="${1:?usage: package.sh VERSION}"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "invalid release version" >&2
  exit 1
fi

name="clibrightness-$version-aarch64-apple-darwin"
mkdir -p "dist/$name"
cp build/clibrightness LICENSE README.md "dist/$name/"
# Exclude macOS metadata and include only the named release files.
COPYFILE_DISABLE=1 tar -czf "dist/$name.tar.gz" -C "dist/$name" clibrightness LICENSE README.md
(
  cd dist
  shasum -a 256 "$name.tar.gz" > checksums.txt
)
