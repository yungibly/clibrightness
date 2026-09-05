#!/bin/bash
set -euo pipefail

version="${1:?usage: formula.sh VERSION SHA256}"
checksum="${2:?usage: formula.sh VERSION SHA256}"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ || ! "$checksum" =~ ^[0-9a-f]{64}$ ]]; then
  echo "invalid formula version or checksum" >&2
  exit 1
fi

cat <<EOF
class Clibrightness < Formula
  desc "Brightness-only control for ASUS PA249CGV over native USB-C on Apple Silicon"
  homepage "https://github.com/yungibly/clibrightness"
  url "https://github.com/yungibly/clibrightness/releases/download/v$version/clibrightness-$version-aarch64-apple-darwin.tar.gz"
  sha256 "$checksum"
  license "MIT"

  depends_on arch: :arm64
  depends_on macos: :tahoe

  def install
    bin.install "clibrightness"
  end

  test do
    # Help and invalid arguments return before display discovery or device IO.
    assert_match "Only ASUS PA249CGV", shell_output("#{bin}/clibrightness --help")
    assert_match "0 to 400", shell_output("#{bin}/clibrightness set 401 2>&1", 2)
  end
end
EOF
