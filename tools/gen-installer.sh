#!/usr/bin/env bash
# Generate install.sh from tools/install.sh.in with the sources embedded.
# Deterministic: same sources -> byte-identical install.sh (checked in CI).
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define MOD_AMD_WS_VERSION "\(.*\)"/\1/p' src/mod_amd_ws.c)
[ -n "$VERSION" ] || { echo "cannot read MOD_AMD_WS_VERSION" >&2; exit 1; }

command -v tar >/dev/null && tar --version | grep -q 'GNU tar' || { echo "GNU tar required" >&2; exit 1; }
FILES="Makefile fs-detect.sh src/mod_amd_ws.c src/amd_engine.c src/amd_engine.h src/ws_client.c src/ws_client.h
       src/local_amd.c src/local_amd.h src/exports.map conf/amd_ws.conf.xml LICENSE"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# reproducible tarball: fixed owner, mtime and order
# shellcheck disable=SC2086
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00Z' -cf "$TMP/src.tar" $FILES
gzip -n -9 <"$TMP/src.tar" >"$TMP/src.tgz"
SHA=$(sha256sum "$TMP/src.tgz" | awk '{print $1}')
base64 -w 76 "$TMP/src.tgz" >"$TMP/payload.b64"

awk -v ver="$VERSION" -v sha="$SHA" -v pf="$TMP/payload.b64" '
	/@PAYLOAD@/ { while ((getline l < pf) > 0) print l; next }
	{ gsub(/@VERSION@/, ver); gsub(/@SHA256@/, sha); print }
' tools/install.sh.in >install.sh.tmp
chmod 0755 install.sh.tmp
OUT=${OUT:-install.sh}
mv install.sh.tmp "$OUT"
echo "$OUT: mod_amd_ws $VERSION, payload sha256 $SHA ($(wc -c <"$TMP/src.tgz") bytes)"
