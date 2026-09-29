#!/usr/bin/env bash
# fs-detect.sh -- find the FreeSWITCH on this host: binary, version, module
# and configuration directories, event-socket access, headers, libfreeswitch.
#
#   fs-detect.sh --show      KEY=VALUE lines (sourceable)
#   fs-detect.sh --moddir    print the module directory only
#
# Order of preference: the RUNNING switch (/proc/<pid>/exe, and its own
# answers over the event socket), then the freeswitch in PATH, then the usual
# package / source-install layouts.  Nothing is changed on the system.
set -u

FS_BIN="${FS_BIN:-}"
FS_PID=""
FS_VERSION=""
FS_MODDIR="${FS_MODDIR:-}"
FS_CONFDIR="${FS_CONFDIR:-}"
FS_LIB=""
FS_INCDIR=""
FS_PKG=""          # deb|rpm|apk|source
FS_CLI=""
FS_ESL_HOST="127.0.0.1"
FS_ESL_PORT="8021"
FS_ESL_PASS="ClueCon"
FS_RUNNING=0

_first_existing() { for p in "$@"; do [ -e "$p" ] && { echo "$p"; return 0; }; done; return 1; }

detect_binary() {
	local pid
	pid=$(pidof freeswitch 2>/dev/null | awk '{print $1}')
	if [ -n "$pid" ] && [ -r "/proc/$pid/exe" ] || [ -n "$pid" ] && [ -L "/proc/$pid/exe" ]; then
		FS_PID=$pid
		FS_RUNNING=1
		[ -z "$FS_BIN" ] && FS_BIN=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
	fi
	if [ -z "$FS_BIN" ]; then
		FS_BIN=$(command -v freeswitch 2>/dev/null || _first_existing /usr/bin/freeswitch /usr/local/freeswitch/bin/freeswitch /opt/freeswitch/bin/freeswitch || true)
	fi
	return 0
	return 0
}

detect_version() {
	[ -n "$FS_BIN" ] && [ -x "$FS_BIN" ] || return 0
	# "FreeSWITCH version: 1.10.11-release~64bit (...)"
	FS_VERSION=$("$FS_BIN" -version 2>/dev/null | sed -n 's/.*[Vv]ersion:\{0,1\} *\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -1)
}

detect_lib() {
	[ -n "$FS_BIN" ] || return 0
	FS_LIB=$(ldd "$FS_BIN" 2>/dev/null | awk '/libfreeswitch\.so/ && !f {print $3; f=1}' || true)
	[ -n "$FS_LIB" ] && FS_LIB=$(readlink -f "$FS_LIB")
	return 0
}

detect_cli() {
	local d cfg
	FS_CLI=$(command -v fs_cli 2>/dev/null || true)
	[ -z "$FS_CLI" ] && [ -n "$FS_BIN" ] && [ -x "$(dirname "$FS_BIN")/fs_cli" ] && FS_CLI="$(dirname "$FS_BIN")/fs_cli"
	# event socket settings from the configuration, when readable
	for d in "$FS_CONFDIR" /etc/freeswitch /usr/local/freeswitch/conf /opt/freeswitch/conf; do
		[ -n "$d" ] || continue
		cfg="$d/autoload_configs/event_socket.conf.xml"
		if [ -r "$cfg" ]; then
			local ip port pass
			ip=$(sed -n 's/.*name="listen-ip"[^>]*value="\([^"]*\)".*/\1/p' "$cfg" | head -1)
			port=$(sed -n 's/.*name="listen-port"[^>]*value="\([^"]*\)".*/\1/p' "$cfg" | head -1)
			pass=$(sed -n 's/.*name="password"[^>]*value="\([^"]*\)".*/\1/p' "$cfg" | head -1)
			case "$ip" in ""|"::"|"0.0.0.0"|"\$\${local_ip_v4}") ip=127.0.0.1 ;; esac
			[ -n "$ip" ] && FS_ESL_HOST=$ip
			[ -n "$port" ] && FS_ESL_PORT=$port
			[ -n "$pass" ] && case "$pass" in \$\$*) ;; *) FS_ESL_PASS=$pass ;; esac
			break
		fi
	done
	return 0
}

fs_x() {
	# run an API command on the running switch; empty output on failure
	[ -n "$FS_CLI" ] && [ "$FS_RUNNING" = 1 ] || return 1
	timeout 10 "$FS_CLI" -H "$FS_ESL_HOST" -P "$FS_ESL_PORT" -p "$FS_ESL_PASS" -x "$*" 2>/dev/null
}

detect_dirs() {
	local v
	if [ "$FS_RUNNING" = 1 ] && [ -n "$FS_CLI" ]; then
		v=$(fs_x 'global_getvar mod_dir' | tr -d '\r' | head -1)
		[ -z "$FS_MODDIR" ] && [ -d "$v" ] && FS_MODDIR=$v
		v=$(fs_x 'global_getvar conf_dir' | tr -d '\r' | head -1)
		[ -z "$FS_CONFDIR" ] && [ -d "$v" ] && FS_CONFDIR=$v
	fi
	if [ -z "$FS_MODDIR" ]; then
		local d
		for d in /usr/lib/freeswitch/mod /usr/lib64/freeswitch/mod /usr/local/freeswitch/mod /opt/freeswitch/mod \
		         "$(dirname "$(dirname "${FS_BIN:-/x/x}")")/lib/freeswitch/mod"; do
			if [ -e "$d/mod_commands.so" ] || [ -e "$d/mod_dptools.so" ]; then FS_MODDIR=$d; break; fi
		done
	fi
	if [ -z "$FS_CONFDIR" ]; then
		FS_CONFDIR=$(_first_existing /etc/freeswitch /usr/local/freeswitch/conf /opt/freeswitch/conf || true)
	fi
	return 0
}

detect_pkg() {
	[ -n "$FS_BIN" ] || return 0
	if command -v dpkg >/dev/null 2>&1 && dpkg -S "$FS_BIN" >/dev/null 2>&1; then FS_PKG=deb
	elif command -v rpm >/dev/null 2>&1 && rpm -qf "$FS_BIN" >/dev/null 2>&1; then FS_PKG=rpm
	elif command -v apk >/dev/null 2>&1 && apk info --who-owns "$FS_BIN" >/dev/null 2>&1; then FS_PKG=apk
	else FS_PKG=source
	fi
	return 0
}

# A header dir is usable when it has switch.h, switch_am_config.h and libteletone.h.
valid_incdir() {
	[ -f "$1/switch.h" ] && [ -f "$1/switch_am_config.h" ] && [ -f "$1/libteletone.h" ]
}

detect_headers() {
	local d pc
	for pc in "${PKG_CONFIG_PATH:-}" /usr/local/freeswitch/lib/pkgconfig "$(dirname "${FS_LIB:-/x/x}")/pkgconfig"; do
		d=$(PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --variable=includedir freeswitch 2>/dev/null || true)
		[ -n "$d" ] && valid_incdir "$d" && { FS_INCDIR=$d; return 0; }
		d=$(PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --cflags-only-I freeswitch 2>/dev/null | sed 's/^-I//; s/ .*//' || true)
		[ -n "$d" ] && valid_incdir "$d" && { FS_INCDIR=$d; return 0; }
	done
	for d in /usr/include/freeswitch /usr/local/freeswitch/include/freeswitch /opt/freeswitch/include/freeswitch \
	         "$(dirname "$(dirname "${FS_BIN:-/x/x}")")/include/freeswitch"; do
		valid_incdir "$d" && { FS_INCDIR=$d; return 0; }
	done
	return 0
}

detect_all() {
	detect_binary
	detect_version
	detect_lib
	detect_cli
	detect_dirs
	detect_cli   # again: conf dir may be known now
	detect_pkg
	detect_headers
	return 0
}

show() {
	cat <<EOF
FS_BIN=${FS_BIN}
FS_PID=${FS_PID}
FS_RUNNING=${FS_RUNNING}
FS_VERSION=${FS_VERSION}
FS_PKG=${FS_PKG}
FS_LIB=${FS_LIB}
FS_MODDIR=${FS_MODDIR}
FS_CONFDIR=${FS_CONFDIR}
FS_INCDIR=${FS_INCDIR}
FS_CLI=${FS_CLI}
FS_ESL=${FS_ESL_HOST}:${FS_ESL_PORT}
EOF
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	detect_all
	case "${1:---show}" in
		--moddir) [ -n "$FS_MODDIR" ] && echo "$FS_MODDIR" ;;
		--show) show ;;
		*) echo "usage: $0 [--show|--moddir]" >&2; exit 2 ;;
	esac
fi
