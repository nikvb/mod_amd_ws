#!/usr/bin/env bash
# Fail when install.sh does not embed the current sources or template.
# Compares file CONTENTS (gzip output differs between gzip builds, so bytes can't be compared).
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
strip_payload() { awk '/^__MOD_AMD_WS_PAYLOAD__$/ {skip=0} !skip {print} /cat <<.__MOD_AMD_WS_PAYLOAD__./ {skip=1}' "$1" | grep -v '^PAYLOAD_SHA256='; }
extract() {  # install.sh -> dir
	awk '/^__MOD_AMD_WS_PAYLOAD__$/ {p=0} p {print} /cat <<.__MOD_AMD_WS_PAYLOAD__./ {p=1}' "$1" | base64 -d | tar -xzf - -C "$2"
}
OUT="$T/fresh.sh" ./tools/gen-installer.sh >/dev/null
if ! diff <(strip_payload install.sh) <(strip_payload "$T/fresh.sh") >"$T/d"; then
	echo "install.sh template part is stale (run make installer):" >&2; head -20 "$T/d" >&2; exit 1
fi
mkdir "$T/a" "$T/b"
extract install.sh "$T/a"
extract "$T/fresh.sh" "$T/b"
if ! diff -r "$T/a" "$T/b" >"$T/d"; then
	echo "install.sh embeds stale sources (run make installer):" >&2; head -20 "$T/d" >&2; exit 1
fi
sha=$(sed -n 's/^PAYLOAD_SHA256="\(.*\)"/\1/p' install.sh)
got=$(awk '/^__MOD_AMD_WS_PAYLOAD__$/ {p=0} p {print} /cat <<.__MOD_AMD_WS_PAYLOAD__./ {p=1}' install.sh | base64 -d | sha256sum | awk '{print $1}')
[ "$sha" = "$got" ] || { echo "install.sh payload checksum mismatch" >&2; exit 1; }
echo "install.sh is up to date"
