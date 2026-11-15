#!/bin/sh
# Optional, Linux only: installs Cisco's OpenH264 2.6.0 so jvconvert can re-encode
# H.264 video. The binary comes straight from Cisco (ciscobinary.openh264.org), as
# Cisco's licence requires, and is used under Cisco's own licence and H.264 patent
# licence. jowe-convert never ships it.
#
# Usage: sh install-openh264.sh [--yes] [lib-dir]   (default lib-dir: ~/.local/lib)
set -eu

YES=0
DEST="$HOME/.local/lib"
for arg in "$@"; do
  case "$arg" in
    --yes|-y) YES=1 ;;
    *) DEST="$arg" ;;
  esac
done

[ "$(uname -s)" = "Linux" ] || { echo "OpenH264 is only needed on Linux."; exit 1; }
case "$(uname -m)" in
  x86_64|amd64)
    f="libopenh264-2.6.0-linux64.8.so.bz2"
    sha="27ab53323c110b76214c1c72222f459d17febbcd1e252136cadc292b0308d75b" ;;
  aarch64|arm64)
    f="libopenh264-2.6.0-linux-arm64.8.so.bz2"
    sha="a78aea7970150f46bcd3bb7994c9e6dd90bd7a9ea785920f5a73f6964e3fcda7" ;;
  *) echo "No Cisco OpenH264 build for $(uname -m)."; exit 1 ;;
esac

url="https://ciscobinary.openh264.org/$f"
echo "Download: $url  (from Cisco, under Cisco's licence)"
echo "SHA-256:  $sha"
echo "Install:  $DEST/libopenh264.so.8"
if [ "$YES" -ne 1 ]; then
  printf "Continue? [y/N] "
  read -r answer
  case "$answer" in y|Y|yes|YES) ;; *) echo "Cancelled."; exit 1 ;; esac
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fL --proto '=https' -o "$tmp/$f" "$url"
got=$(sha256sum "$tmp/$f" | cut -d' ' -f1)
if [ "$got" != "$sha" ]; then
  echo "SHA-256 mismatch: expected $sha, got $got. Nothing was installed."
  exit 1
fi
echo "SHA-256 OK"

mkdir -p "$DEST"
bunzip2 -c "$tmp/$f" > "$DEST/libopenh264.so.8"
echo
echo "Installed $DEST/libopenh264.so.8"
echo "Point jvconvert at it (add to your shell profile, or to the MCP server's env):"
echo "  export JOWE_CONVERT_OPENH264=\"$DEST/libopenh264.so.8\""
echo "Then 'jvconvert formats --kind video' should list a re-encode backend instead of 're-encode: no'."
