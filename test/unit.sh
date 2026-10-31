#!/usr/bin/env bash
# Engine + WebSocket client against the mock server, without FreeSWITCH.
# Each case: mock path | engine_cli args | expected STATUS | expected CAUSE (regex)
set -euo pipefail
cd "$(dirname "$0")/.."
[ -x test/engine_cli ] || make -s test/engine_cli
S=$(mktemp -d)
trap 'kill $MOCK 2>/dev/null || true; rm -rf "$S"' EXIT
python3 test/gen_audio.py "$S/audio" >/dev/null
python3 test/mock_amd_server.py --port 0 --port-file "$S/port" --control "$S/ctl" --record "$S/rec" >"$S/mock.log" 2>&1 &
MOCK=$!
for _ in $(seq 50); do [ -s "$S/port" ] && break; sleep 0.1; done
P=$(cat "$S/port")
A=$S/audio
pass=0 fail=0
run() {  # name mockpath expstatus expcause args...
	local name=$1 path=$2 est=$3 ecause=$4; shift 4
	echo "$path" >"$S/ctl"
	local out st ca
	out=$(./test/engine_cli -p "$P" "$@" 2>/dev/null)
	st=$(sed -n 's/^STATUS=//p' <<<"$out"); ca=$(sed -n 's/^CAUSE=//p' <<<"$out")
	if [ "$st" = "$est" ] && [[ "$ca" =~ $ecause ]]; then
		pass=$((pass + 1)); printf '%-16s PASS  %s/%s\n' "$name" "$st" "$ca"
	else
		fail=$((fail + 1)); printf '%-16s FAIL  got %s/%s want %s/%s\n' "$name" "$st" "$ca" "$est" "$ecause"
	fi
}
run human        "/human?after=2"   HUMAN   '^HUMAN$'             "$A/human.sln"
run machine      "/machine?after=1" MACHINE '^MACHINE$'           "$A/machine.sln"
run amd_detected "/status?value=AMD_DETECTED" MACHINE '^AMD_DETECTED$' "$A/machine.sln"
run amdy_guard   "/amdy"            HUMAN   '^HUMAN$'             "$A/machine.sln"
run fragmented   "/fragmented"      HUMAN   '^HUMAN$'             "$A/human.sln"
run eof_human    "/eof_human"       HUMAN   '^HUMAN$'             -after none "$A/hello_then_nothing.sln"
run eof_ack      "/eof_ack"         NOTSURE '^EOF_INCONCLUSIVE$'  -after none "$A/hello_then_nothing.sln"
run timeout      "/silent"          NOTSURE '^SERVER_TIMEOUT$'    -t 3000 "$A/human.sln"
run noaudio      "/silent"          NOTSURE '^NOAUDIODATA-2000$'  -t 2000 /dev/null
run close        "/close?after=1"   HUMAN   '^PROCESSING_ERROR$'  "$A/machine.sln"
run burst_close  "/burst_close"     MACHINE '^MACHINE$'           "$A/machine.sln"
run hangup       "/silent"          HANGUP  '^HANGUP$'            -after hangup:0 "$A/hello_then_nothing.sln"
run refused      "/human"           HUMAN   '^CONNECTION_ERROR$'  -p 1 "$A/human.sln"
run local_human  "/human"           HUMAN   '^HUMAN$'             -m local "$A/human.sln"
run local_mach   "/human"           MACHINE '^(MAXWORDS|LONGGREETING)$' -m local "$A/machine.sln"
run fallback     "/human"           MACHINE '^(MAXWORDS|LONGGREETING)$' -p 1 -F "$A/machine.sln"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
