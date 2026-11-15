#!/bin/sh
# Installs jvconvert v0.3.1 on macOS (arm64) or Linux (x86_64, arm64).
# Downloads one file from the jowe-convert GitHub release, checks its SHA-256 and
# copies the jvconvert binary into a folder (default: ~/.local/bin).
#
# Usage: sh install.sh [--yes] [install-dir]
set -eu

VERSION="v0.3.1"
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
    sha="746a2f094b0394d07a56dafe30ca384774fd1dc070abd0ea8b6fbdbd9dd79cda" ;;
  Linux/x86_64|Linux/amd64)
    asset="jvconvert-$VERSION-linux-x64.tar.gz"
    sha="8173e53b4f3d7877296b87d52293eabbde84f2f976d7318d545b6bdd9a4cf5ee" ;;
  Linux/aarch64|Linux/arm64)
    asset="jvconvert-$VERSION-linux-arm64.tar.gz"
    sha="88c5ef24038c13e5fac9fcf599fee1540e8ff2714c33a257890441398b5b90b4" ;;
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
  echo "jvconvert was copied but does not start here. The v0.3.1 Linux builds need glibc 2.35 or newer"
  echo "(Ubuntu 22.04+, Debian 12+). Check yours with: ldd --version"
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
