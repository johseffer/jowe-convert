#!/bin/sh
# Installs jvconvert v0.3.0 on macOS (arm64) or Linux (x86_64, arm64).
# Downloads one file from the jowe-convert GitHub release, checks its SHA-256 and
# copies the jvconvert binary into a folder (default: ~/.local/bin).
#
# Usage: sh install.sh [--yes] [install-dir]
set -eu

VERSION="v0.3.0"
BASE="https://github.com/johseffer/jowe-convert/releases/download/$VERSION"

YES=0
DEST="$HOME/.local/bin"
for arg in "$@"; do
  case "$arg" in
    --yes|-y) YES=1 ;;
    *) DEST="$arg" ;;
  esac
done

os=$(uname -s)
arch=$(uname -m)
case "$os/$arch" in
  Darwin/arm64)
    asset="jvconvert-$VERSION-macos-arm64.tar.gz"
    sha="af43a2eed876cc10c30ce669957bf7f1520aa9f9c093bf310e1a709e019637e9" ;;
  Linux/x86_64|Linux/amd64)
    asset="jvconvert-$VERSION-linux-x64.tar.gz"
    sha="9d7fa56f04c0fb09e1f132ae1665ccad1b543bc4b9542eb6c8d11acd9603ec0e" ;;
  Linux/aarch64|Linux/arm64)
    asset="jvconvert-$VERSION-linux-arm64.tar.gz"
    sha="bc739d2efa02d6bf02844d3909e9513b311b445dba5d4371183495751cce3010" ;;
  *)
    echo "No prebuilt jvconvert for $os/$arch. Build it from source:"
    echo "  https://github.com/johseffer/jowe-convert#build"
    exit 1 ;;
esac

url="$BASE/$asset"
echo "Download: $url"
echo "SHA-256:  $sha"
echo "Install:  $DEST/jvconvert"
if [ "$YES" -ne 1 ]; then
  printf "Continue? [y/N] "
  read -r answer
  case "$answer" in y|Y|yes|YES) ;; *) echo "Cancelled."; exit 1 ;; esac
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fL --proto '=https' -o "$tmp/$asset" "$url"

if command -v sha256sum >/dev/null 2>&1; then
  got=$(sha256sum "$tmp/$asset" | cut -d' ' -f1)
else
  got=$(shasum -a 256 "$tmp/$asset" | cut -d' ' -f1)
fi
if [ "$got" != "$sha" ]; then
  echo "SHA-256 mismatch: expected $sha, got $got. Nothing was installed."
  exit 1
fi
echo "SHA-256 OK"

tar -xzf "$tmp/$asset" -C "$tmp" ./jvconvert ./LICENSE
mkdir -p "$DEST"
install -m 0755 "$tmp/jvconvert" "$DEST/jvconvert"
if ! "$DEST/jvconvert" --version; then
  echo "jvconvert was copied but does not start here. The v0.3.0 Linux builds need a recent glibc"
  echo "(x86_64: 2.43 or newer; arm64: 2.38 or newer). Check yours with: ldd --version"
  echo "Otherwise build from source: https://github.com/johseffer/jowe-convert#build"
  exit 1
fi

cat <<EOF

Installed $DEST/jvconvert

Add it to Claude Code (pick the folders it may read and write):
  claude mcp add jvconvert -- "$DEST/jvconvert" mcp --allow "\$HOME/Media"

Check what this machine can convert:
  "$DEST/jvconvert" formats
EOF
if [ "$os" = "Linux" ]; then
  echo
  echo "Video re-encoding on Linux needs Cisco's OpenH264 2.6 (optional): see install-openh264.sh"
fi
