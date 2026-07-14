#!/usr/bin/env bash
#
# release.sh - build, publish, and verify an OTA firmware release.
#
# Devices poll https://adamrunner.com/downloads/esp32-sim7670g/manifest.json
# and update when its "version" differs from the running app's esp_app_desc
# version. This script builds the app, uploads the .bin under a versioned
# name, then atomically swaps in a manifest pointing at it, and finally
# verifies the whole thing from the public URL.
#
# Usage:
#   tools/release.sh [options]
#
# Options:
#   --no-build      Skip `idf.py build`; publish the existing build artifacts.
#   --bin PATH      Publish PATH instead of build/esp32-sim7670g.bin (implies
#                   --no-build). Useful for CI or testing against a snapshot.
#   --desc PATH     Read version from PATH instead of build/project_description.json.
#   --allow-dirty   Permit publishing a version ending in -dirty.
#   --force         Overwrite an already-published <ver>.bin on the server.
#   --dry-run       Print what would be published without touching the server.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SSH_HOST="adamrunner"                                # tailscale alias, key auth
REMOTE_DIR="adamrunner.com/downloads/esp32-sim7670g" # relative to $HOME on server
BASE_URL="https://adamrunner.com/downloads/esp32-sim7670g"
PROJECT="esp32-sim7670g"

BIN=""
DESC=""
DO_BUILD=1
ALLOW_DIRTY=0
FORCE=0
DRY_RUN=0

die() { echo "release.sh: error: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)    DO_BUILD=0 ;;
        --bin)         BIN="$2"; DO_BUILD=0; shift ;;
        --desc)        DESC="$2"; shift ;;
        --allow-dirty) ALLOW_DIRTY=1 ;;
        --force)       FORCE=1 ;;
        --dry-run)     DRY_RUN=1 ;;
        -h|--help)     sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)             die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

# --- build ------------------------------------------------------------------

if [[ $DO_BUILD -eq 1 ]]; then
    if ! command -v idf.py >/dev/null 2>&1; then
        # ESP-IDF venv is py3.13 on this machine; export.sh picks the wrong
        # one without IDF_PYTHON_ENV_PATH.
        export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
        source "$HOME/esp/v5.5/esp-idf/export.sh"
    fi
    (cd "$REPO_ROOT" && idf.py build)
fi

BIN="${BIN:-$REPO_ROOT/build/$PROJECT.bin}"
DESC="${DESC:-$REPO_ROOT/build/project_description.json}"
[[ -f "$BIN" ]]  || die "binary not found: $BIN"
[[ -f "$DESC" ]] || die "project description not found: $DESC (build first?)"

# --- gather release metadata --------------------------------------------------

VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project_version"])' "$DESC")"
[[ -n "$VERSION" ]] || die "empty project_version in $DESC"

# The manifest version must equal the esp_app_desc version baked into this
# exact binary, or the fleet will update in a loop. Guard against a stale
# project_description.json by reading the string out of the app image itself
# (esp_app_desc_t sits at offset 0x20, version field at 0x30, 32 bytes).
EMBEDDED="$(python3 - "$BIN" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read(112)
if struct.unpack('<I', b[32:36])[0] != 0xABCD5432:
    sys.exit('not an ESP app image (bad esp_app_desc magic)')
print(b[48:80].split(b'\0')[0].decode())
EOF
)"
[[ "$EMBEDDED" == "$VERSION" ]] || \
    die "version mismatch: binary says '$EMBEDDED', $DESC says '$VERSION' (stale build dir?)"

if [[ "$VERSION" == *-dirty && $ALLOW_DIRTY -eq 0 ]]; then
    die "refusing to publish dirty version '$VERSION' (commit first, or pass --allow-dirty)"
fi

# The manifest URL the image polls is baked in at compile time, and an
# `idf.py -DOTA_MANIFEST_URL=...` test override STICKS in the CMake cache
# across later plain builds (clear with `idf.py fullclean`). A published
# image polling a staging manifest never sees another release, so check the
# string actually embedded in the binary. No flag bypasses this.
BAKED_URL="$(grep -a -o 'https://[a-zA-Z0-9./_-]*manifest\.json' "$BIN" | sort -u)"
[[ "$BAKED_URL" == "$BASE_URL/manifest.json" ]] || \
    die "binary polls '$BAKED_URL', not $BASE_URL/manifest.json (stale OTA_MANIFEST_URL in the CMake cache? run: idf.py fullclean)"

if command -v sha256sum >/dev/null 2>&1; then
    SHA256="$(sha256sum "$BIN" | cut -d' ' -f1)"
else
    SHA256="$(shasum -a 256 "$BIN" | cut -d' ' -f1)"  # macOS
fi
SIZE="$(wc -c < "$BIN" | tr -d ' ')"

ARTIFACT="$PROJECT-$VERSION.bin"
BIN_URL="$BASE_URL/$ARTIFACT"
MANIFEST="$(printf '{"version": "%s", "url": "%s", "sha256": "%s", "size": %s}\n' \
    "$VERSION" "$BIN_URL" "$SHA256" "$SIZE")"

echo "version:  $VERSION"
echo "binary:   $BIN"
echo "artifact: $ARTIFACT ($SIZE bytes)"
echo "sha256:   $SHA256"
echo "manifest: $MANIFEST"

if [[ $DRY_RUN -eq 1 ]]; then
    echo "dry run: would upload $ARTIFACT and manifest.json to $SSH_HOST:~/$REMOTE_DIR/"
    exit 0
fi

# --- publish ------------------------------------------------------------------

# Cloudflare edge-caches .bin responses, so a published artifact name must
# never be reused with different content. Refuse to overwrite by default.
if ssh "$SSH_HOST" "test -f '$REMOTE_DIR/$ARTIFACT'"; then
    if [[ $FORCE -eq 0 ]]; then
        die "$ARTIFACT already published; the edge may have cached it. Use --force only if you know it was never fetched."
    fi
    echo "WARNING: overwriting already-published $ARTIFACT (--force)" >&2
fi

# Upload the .bin first (staged, then mv into place), and only then swap the
# manifest in atomically, so a polling device never sees a manifest that
# points at a missing or partial binary.
ssh "$SSH_HOST" "mkdir -p '$REMOTE_DIR'"
scp -q "$BIN" "$SSH_HOST:$REMOTE_DIR/$ARTIFACT.tmp"
ssh "$SSH_HOST" "mv '$REMOTE_DIR/$ARTIFACT.tmp' '$REMOTE_DIR/$ARTIFACT'"
echo "$MANIFEST" | ssh "$SSH_HOST" "cat > '$REMOTE_DIR/manifest.json.tmp' && mv '$REMOTE_DIR/manifest.json.tmp' '$REMOTE_DIR/manifest.json'"
echo "published: $BIN_URL"
echo "published: $BASE_URL/manifest.json"

# --- verify from the outside ----------------------------------------------------

echo "verifying via $BASE_URL ..."
TMPDIR_V="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_V"' EXIT

# Manifest must be origin-fresh (Cloudflare serves .json as DYNAMIC).
curl -fsS "$BASE_URL/manifest.json" -o "$TMPDIR_V/manifest.json"
GOT_VER="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$TMPDIR_V/manifest.json")"
[[ "$GOT_VER" == "$VERSION" ]] || die "verify: manifest version '$GOT_VER' != '$VERSION' (stale origin?)"

# Full download: 200, correct size, sha256 matches the manifest.
HTTP_CODE="$(curl -fsS -w '%{http_code}' "$BIN_URL" -o "$TMPDIR_V/fw.bin")"
[[ "$HTTP_CODE" == "200" ]] || die "verify: GET $BIN_URL returned $HTTP_CODE"
GOT_SIZE="$(wc -c < "$TMPDIR_V/fw.bin" | tr -d ' ')"
[[ "$GOT_SIZE" == "$SIZE" ]] || die "verify: downloaded $GOT_SIZE bytes, expected $SIZE"
if command -v sha256sum >/dev/null 2>&1; then
    GOT_SHA="$(sha256sum "$TMPDIR_V/fw.bin" | cut -d' ' -f1)"
else
    GOT_SHA="$(shasum -a 256 "$TMPDIR_V/fw.bin" | cut -d' ' -f1)"
fi
[[ "$GOT_SHA" == "$SHA256" ]] || die "verify: downloaded sha256 $GOT_SHA != $SHA256"

# The OTA client resumes with Range requests; the edge must answer 206.
RANGE_CODE="$(curl -fsS -w '%{http_code}' -r 0-1023 "$BIN_URL" -o "$TMPDIR_V/range.bin")"
[[ "$RANGE_CODE" == "206" ]] || die "verify: Range request returned $RANGE_CODE, expected 206"
RANGE_SIZE="$(wc -c < "$TMPDIR_V/range.bin" | tr -d ' ')"
[[ "$RANGE_SIZE" == "1024" ]] || die "verify: Range request returned $RANGE_SIZE bytes, expected 1024"

echo "OK: manifest fresh, $ARTIFACT is $SIZE bytes, sha256 verified, Range->206"
