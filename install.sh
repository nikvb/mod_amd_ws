#!/usr/bin/env bash
# mod_amd_ws installer -- answering machine detection over WebSocket for FreeSWITCH.
#
# GENERATED FILE: edit tools/install.sh.in and the sources, then run
# `make installer` (tools/gen-installer.sh).  The module sources are embedded
# at the end of this file, so the script needs nothing else from the network
# except build packages and, only when the switch has no development headers,
# the matching FreeSWITCH source tarball from GitHub.
#
#   curl -fsSL https://raw.githubusercontent.com/nikvb/mod_amd_ws/main/install.sh | sudo bash -s -- -y
#
# Options:
#   -y, --yes            do not ask before installing build packages
#   --dry-run            detect and print the plan; change nothing
#   --build-only         build mod_amd_ws.so into the current directory; install nothing
#   --host H --port P    endpoint written into a NEW amd_ws.conf.xml (default api.amdy.io:2700)
#   --tls                write tls=true into a NEW amd_ws.conf.xml
#   --no-config          do not create amd_ws.conf.xml
#   --no-autoload        do not add mod_amd_ws to modules.conf.xml
#   --no-load            do not load/reload the module in the running switch
#   --moddir DIR         module directory (default: asked from the running switch)
#   --confdir DIR        configuration directory (default: asked from the running switch)
#   --fs-src DIR         use headers from this FreeSWITCH source tree (src/include)
#   --no-devpkg          do not try the distribution's FreeSWITCH dev package for headers
#   --uninstall          unload, remove the module and the autoload line (config kept)
#   -h, --help
set -euo pipefail

MOD_VERSION="1.0.1"
PAYLOAD_SHA256="3748276065098bd271ecd978f03466237c521dfd6b01b9ba47a7ba9af814d3ba"
LOG=/var/log/mod_amd_ws-install.log

YES=0 DRYRUN=0 BUILDONLY=0 NOCONFIG=0 NOAUTOLOAD=0 NOLOAD=0 UNINSTALL=0 NODEVPKG=0
CFG_HOST="api.amdy.io" CFG_PORT="2700" CFG_TLS=0
OPT_MODDIR="" OPT_CONFDIR="" OPT_FSSRC=""
WORK=""

say()  { printf '%s\n' "$*"; [ -w "$(dirname "$LOG")" ] && printf '%s %s\n' "$(date '+%F %T')" "$*" >>"$LOG" 2>/dev/null || true; }
ok()   { say "[OK]   $*"; }
info() { say "[..]   $*"; }
warn() { say "[WARN] $*"; }
die()  { say "[FAIL] $*"; say "Log: $LOG"; exit 1; }

usage() { sed -n '2,28p' "${BASH_SOURCE[0]:-/dev/null}" 2>/dev/null | sed 's/^# \{0,1\}//' || true; }

parse_args() {
	while [ $# -gt 0 ]; do
		case "$1" in
			-y|--yes) YES=1 ;;
			--dry-run) DRYRUN=1 ;;
			--build-only) BUILDONLY=1 ;;
			--host) CFG_HOST=${2:?--host needs a value}; shift ;;
			--port) CFG_PORT=${2:?--port needs a value}; shift ;;
			--tls) CFG_TLS=1 ;;
			--no-config) NOCONFIG=1 ;;
			--no-autoload) NOAUTOLOAD=1 ;;
			--no-load) NOLOAD=1 ;;
			--moddir) OPT_MODDIR=${2:?}; shift ;;
			--confdir) OPT_CONFDIR=${2:?}; shift ;;
			--fs-src) OPT_FSSRC=${2:?}; shift ;;
			--uninstall) UNINSTALL=1 ;;
			--no-devpkg) NODEVPKG=1 ;;
			-h|--help) usage; exit 0 ;;
			*) die "unknown option: $1 (see --help)" ;;
		esac
		shift
	done
	case "$CFG_PORT" in ''|*[!0-9]*) die "--port must be a number" ;; esac
	case "$CFG_HOST" in *[\"\<\>\&\ ]*) die "--host contains invalid characters" ;; esac
}

cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; }

# ---------------------------------------------------------------------------
# embedded sources
extract_payload() {
	local got
	WORK=$(mktemp -d /tmp/mod_amd_ws.XXXXXX)
	trap cleanup EXIT
	payload | base64 -d >"$WORK/src.tgz" || die "embedded payload is corrupt"
	got=$(sha256sum "$WORK/src.tgz" | awk '{print $1}')
	[ "$got" = "$PAYLOAD_SHA256" ] || die "embedded payload checksum mismatch ($got)"
	mkdir -p "$WORK/src"
	tar -xzf "$WORK/src.tgz" -C "$WORK/src"
	ok "sources mod_amd_ws $MOD_VERSION (sha256 ${PAYLOAD_SHA256:0:12}...)"
	# shellcheck source=/dev/null
	. "$WORK/src/fs-detect.sh"
}

# ---------------------------------------------------------------------------
# packages
PM=""
detect_pm() {
	if command -v apt-get >/dev/null 2>&1; then PM=apt
	elif command -v dnf >/dev/null 2>&1; then PM=dnf
	elif command -v yum >/dev/null 2>&1; then PM=yum
	elif command -v zypper >/dev/null 2>&1; then PM=zypper
	elif command -v apk >/dev/null 2>&1; then PM=apk
	fi
}

have_openssl_headers() {
	printf '#include <openssl/ssl.h>\n#include <openssl/x509v3.h>\n' | gcc -E -x c - >/dev/null 2>&1
}

# Version of the package that installed the running switch's binary (FS_PKG
# was detected from the same owner), for pinning the dev package to it.
installed_fs_pkg_version() {
	local owner
	case "$FS_PKG" in
		deb) owner=$(dpkg -S "$FS_BIN" 2>/dev/null | head -1 | cut -d: -f1)
		     [ -n "$owner" ] && dpkg-query -W -f='${Version}' "$owner" 2>/dev/null ;;
		rpm) rpm -qf --qf '%{VERSION}-%{RELEASE}' "$FS_BIN" 2>/dev/null ;;
		apk) apk info --who-owns "$FS_BIN" 2>/dev/null | sed -n 's/.* is owned by //p' \
		       | sed -nE 's/^.*-([0-9][^-]*-r[0-9]+)$/\1/p' ;;
	esac
}

need_packages() {
	local miss=()
	command -v gcc >/dev/null 2>&1 || miss+=(gcc)
	command -v make >/dev/null 2>&1 || miss+=(make)
	tar --version 2>/dev/null | grep -q 'GNU tar' || miss+=(tar)          # busybox tar lacks --wildcards
	command -v base64 >/dev/null 2>&1 || miss+=(coreutils)
	command -v nm >/dev/null 2>&1 || miss+=(binutils)
	command -v gcc >/dev/null 2>&1 && ! have_openssl_headers && miss+=(openssl-dev)
	command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || miss+=(curl)
	printf '%s\n' ${miss[@]+"${miss[@]}"}   # bash < 4.4 (CentOS 7): an empty "${a[@]}" is unbound under set -u
}

install_packages() {
	local miss list
	miss=$(need_packages)
	[ -z "$miss" ] && { ok "build tools present (gcc, make, OpenSSL headers)"; return 0; }
	detect_pm
	case "$PM" in
		apt) list="build-essential pkg-config libssl-dev binutils curl ca-certificates" ;;
		dnf|yum) list="gcc make pkgconfig openssl-devel binutils curl tar" ;;
		zypper) list="gcc make pkg-config libopenssl-devel binutils curl tar" ;;
		apk) list="build-base pkgconf openssl-dev binutils curl tar coreutils" ;;
		*) die "missing: $(echo "$miss" | tr '\n' ' ') and no known package manager; install them and rerun" ;;
	esac
	if [ "$DRYRUN" = 1 ]; then info "would install packages ($PM): $list"; return 0; fi
	[ "$(id -u)" = 0 ] || die "missing $(echo "$miss" | tr '\n' ' '); run as root to install: $list"
	if [ "$YES" != 1 ] && [ -t 0 ]; then
		read -r -p "Install build packages with $PM: $list ? [y/N] " a
		case "$a" in y|Y|yes) ;; *) die "cancelled" ;; esac
	fi
	info "installing $list"
	local pkgs
	read -r -a pkgs <<<"$list"
	case "$PM" in
		apt) DEBIAN_FRONTEND=noninteractive apt-get update -qq >>"$LOG" 2>&1 || true
		     DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${pkgs[@]}" >>"$LOG" 2>&1 ;;
		dnf|yum) "$PM" install -y "${pkgs[@]}" >>"$LOG" 2>&1 ;;
		zypper) zypper --non-interactive install "${pkgs[@]}" >>"$LOG" 2>&1 ;;
		apk) apk add --no-cache "${pkgs[@]}" >>"$LOG" 2>&1 ;;
	esac || die "package installation failed (see $LOG)"
	[ -z "$(need_packages)" ] || die "still missing after install: $(need_packages | tr '\n' ' ')"
	ok "build packages installed"
}

# ---------------------------------------------------------------------------
# headers
INCDIR=""
fetch() {  # url dest
	if command -v curl >/dev/null 2>&1; then curl -fsSL --retry 2 -o "$2" "$1"; else wget -q -O "$2" "$1"; fi
}

gen_am_config() {  # dir
	local bits
	bits=$(getconf LONG_BIT 2>/dev/null || echo 64)
	if [ "$bits" = 64 ]; then
		cat >"$1/switch_am_config.h" <<'EOF'
#ifndef SWITCH_AM_CONFIG_H
#define SWITCH_AM_CONFIG_H
#define SWITCH_INT_16  short
#define SWITCH_INT_32  int
#define SWITCH_INT_64  long
#define SWITCH_SIZE_T  size_t
#define SWITCH_SSIZE_T ssize_t
#define SWITCH_SIZEOF_VOIDP 8
#define SWITCH_PREFIX_DIR "/usr"
#define SWITCH_SSIZE_T_FMT "ld"
#define SWITCH_SIZE_T_FMT "ld"
#define SWITCH_INT64_T_FMT "ld"
#define SWITCH_UINT64_T_FMT "lu"
#endif
EOF
	else
		cat >"$1/switch_am_config.h" <<'EOF'
#ifndef SWITCH_AM_CONFIG_H
#define SWITCH_AM_CONFIG_H
#define SWITCH_INT_16  short
#define SWITCH_INT_32  int
#define SWITCH_INT_64  long long
#define SWITCH_SIZE_T  size_t
#define SWITCH_SSIZE_T ssize_t
#define SWITCH_SIZEOF_VOIDP 4
#define SWITCH_PREFIX_DIR "/usr"
#define SWITCH_SSIZE_T_FMT "d"
#define SWITCH_SIZE_T_FMT "d"
#define SWITCH_INT64_T_FMT "lld"
#define SWITCH_UINT64_T_FMT "llu"
#endif
EOF
	fi
}

headers_from_tree() {  # source tree -> $WORK/include
	local t=$1
	[ -f "$t/src/include/switch.h" ] || return 1
	mkdir -p "$WORK/include"
	cp "$t"/src/include/*.h "$WORK/include/"
	cp "$t"/libs/libteletone/src/*.h "$WORK/include/" 2>/dev/null || true
	[ -f "$WORK/include/switch_am_config.h" ] || gen_am_config "$WORK/include"
	INCDIR="$WORK/include"
}

find_headers() {
	if [ -n "$OPT_FSSRC" ]; then
		headers_from_tree "$OPT_FSSRC" || die "--fs-src $OPT_FSSRC has no src/include/switch.h"
		ok "headers from source tree $OPT_FSSRC"
		return 0
	fi
	if [ -n "$FS_INCDIR" ]; then
		INCDIR=$FS_INCDIR
		ok "headers $INCDIR"
		return 0
	fi
	# the distribution's dev package, only when the switch itself came from that
	# package manager, and only at the exact version of the installed switch: an
	# unpinned install can pull a newer core under the running switch.
	local pkg="" ver=""
	case "$FS_PKG" in
		deb) pkg=libfreeswitch-dev ;;
		rpm) pkg=freeswitch-devel ;;
		apk) pkg=freeswitch-dev ;;
	esac
	[ "$NODEVPKG" = 1 ] && pkg=""
	if [ -n "$pkg" ]; then
		ver=$(installed_fs_pkg_version || true)
		if [ -z "$ver" ]; then
			warn "cannot tell the installed FreeSWITCH package version; not installing $pkg (it could upgrade the core)"
			pkg=""
		fi
	fi
	if [ -n "$pkg" ]; then
		if [ "$DRYRUN" = 1 ]; then
			info "would try the $pkg package at version $ver, else the v$FS_VERSION source tarball"
			return 0
		fi
		info "trying the $pkg package at version $ver (no upgrades)"
		case "$FS_PKG" in
			deb) DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-upgrade "$pkg=$ver" >>"$LOG" 2>&1 || true ;;
			rpm) { command -v dnf >/dev/null && dnf install -y "$pkg-$ver" || yum install -y "$pkg-$ver"; } >>"$LOG" 2>&1 || true ;;
			apk) apk add --no-cache "$pkg=$ver" >>"$LOG" 2>&1 || true ;;
		esac
		detect_headers || true
		if [ -n "$FS_INCDIR" ]; then INCDIR=$FS_INCDIR; ok "headers $INCDIR ($pkg)"; return 0; fi
		warn "$pkg is not available from the configured repositories"
	fi
	# the matching source tarball
	[ -n "$FS_VERSION" ] || die "no FreeSWITCH headers and the version is unknown; pass --fs-src /path/to/freeswitch-source"
	local url="https://github.com/signalwire/freeswitch/archive/refs/tags/v${FS_VERSION}.tar.gz"
	if [ "$DRYRUN" = 1 ]; then info "would fetch headers from $url"; return 0; fi
	info "no development headers installed; fetching them from $url"
	fetch "$url" "$WORK/fs.tgz" || die "cannot download $url (no network?); install the dev package or pass --fs-src"
	mkdir -p "$WORK/fs"
	tar -xzf "$WORK/fs.tgz" -C "$WORK/fs" --strip-components=1 --wildcards '*/src/include/*' '*/libs/libteletone/src/*' 2>>"$LOG" \
		|| die "unexpected tarball layout ($url)"
	headers_from_tree "$WORK/fs" || die "tarball has no src/include/switch.h"
	ok "headers from FreeSWITCH v$FS_VERSION source"
}

# ---------------------------------------------------------------------------
# build
build() {
	local libs lib_dir out
	[ -n "$FS_LIB" ] || die "cannot find libfreeswitch.so used by $FS_BIN (ldd)"
	lib_dir=$(dirname "$FS_LIB")
	libs="$FS_LIB"
	info "building against $INCDIR, linking $FS_LIB"
	out=$(make -C "$WORK/src" FS_INCLUDES="-I$INCDIR" FS_LIBS="$libs" FS_MODDIR="${FS_MODDIR:-/nonexistent}" 2>&1) \
		|| { printf '%s\n' "$out" >>"$LOG"; printf '%s\n' "$out" | tail -20; die "build failed"; }
	printf '%s\n' "$out" >>"$LOG"
	if printf '%s\n' "$out" | grep -qi 'warning:'; then warn "compiler warnings (see $LOG)"; fi
	ok "built mod_amd_ws.so"

	# gates
	local undef
	undef=$(LD_LIBRARY_PATH="$lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd -r "$WORK/src/mod_amd_ws.so" 2>&1 | grep -i 'undefined symbol' || true)
	[ -z "$undef" ] || { printf '%s\n' "$undef" >>"$LOG"; die "undefined symbols against $FS_LIB (headers do not match the running switch?)"; }
	ok "no undefined symbols against $(basename "$FS_LIB")"
	local exp
	exp=$(nm -D --defined-only "$WORK/src/mod_amd_ws.so" | awk '$2 !~ /^[AUw]$/ {print $3}' | sort -u | tr '\n' ' ')
	[ "$exp" = "mod_amd_ws_module_interface " ] || die "unexpected exported symbols: $exp"
	ok "exports only mod_amd_ws_module_interface"
	# OpenSSL: the module and the switch must use the same libssl
	local fs_ssl mod_ssl
	fs_ssl=$(ldd "$FS_LIB" 2>/dev/null | awk '/libssl\.so/ && !f {print $3; f=1}' || true)
	mod_ssl=$(LD_LIBRARY_PATH="$lib_dir" ldd "$WORK/src/mod_amd_ws.so" 2>/dev/null | awk '/libssl\.so/ && !f {print $3; f=1}' || true)
	if [ -n "$fs_ssl" ] && [ -n "$mod_ssl" ] && [ "$(readlink -f "$fs_ssl")" != "$(readlink -f "$mod_ssl")" ]; then
		die "FreeSWITCH uses $fs_ssl but the module links $mod_ssl; build with the same OpenSSL (SSL_CFLAGS/SSL_LIBS)"
	fi
	ok "OpenSSL ${mod_ssl:-(static)} matches the switch"
}

# ---------------------------------------------------------------------------
# install
install_module() {
	local dst="$FS_MODDIR/mod_amd_ws.so"
	[ -d "$FS_MODDIR" ] || die "module directory $FS_MODDIR does not exist"
	if [ -f "$dst" ]; then
		cp -p "$dst" "$dst.bak.$(date +%Y%m%d%H%M%S)"
		ok "backed up the old module to $dst.bak.*"
	fi
	install -m 0755 "$WORK/src/mod_amd_ws.so" "$dst.new"
	mv -f "$dst.new" "$dst"          # atomic replace: a loaded copy keeps its old inode
	ok "installed $dst"
}

install_config() {
	local d="$FS_CONFDIR/autoload_configs" f
	f="$d/amd_ws.conf.xml"
	[ "$NOCONFIG" = 1 ] && return 0
	[ -d "$d" ] || { warn "no $d; skipping amd_ws.conf.xml"; return 0; }
	if [ -f "$f" ]; then ok "kept existing $f"; return 0; fi
	sed -e "s|<param name=\"host\" value=\"[^\"]*\"/>|<param name=\"host\" value=\"$CFG_HOST\"/>|" \
	    -e "s|<param name=\"port\" value=\"[^\"]*\"/>|<param name=\"port\" value=\"$CFG_PORT\"/>|" \
	    "$WORK/src/conf/amd_ws.conf.xml" >"$f"
	[ "$CFG_TLS" = 1 ] && sed -i 's|<param name="tls" value="false"/>|<param name="tls" value="true"/>|' "$f"
	chmod 0644 "$f"
	# match the owner of the other config files (freeswitch user on packaged installs)
	chown --reference="$d" "$f" 2>/dev/null || true
	ok "created $f (host $CFG_HOST:$CFG_PORT)"
}

install_autoload() {
	local f="$FS_CONFDIR/autoload_configs/modules.conf.xml"
	[ "$NOAUTOLOAD" = 1 ] && return 0
	[ -f "$f" ] || { warn "no $f; add <load module=\"mod_amd_ws\"/> to your modules configuration yourself"; return 0; }
	if grep -q 'module="mod_amd_ws"' "$f"; then
		if grep -q '<!--[^>]*module="mod_amd_ws"' "$f"; then warn "mod_amd_ws is commented out in $f; left as is"; else ok "mod_amd_ws already in $f"; fi
		return 0
	fi
	grep -q '</modules>' "$f" || { warn "no </modules> in $f; add the load line yourself"; return 0; }
	cp -p "$f" "$f.bak.$(date +%Y%m%d%H%M%S)"
	sed -i 's|</modules>|    <load module="mod_amd_ws"/>\n  </modules>|' "$f"
	ok "added mod_amd_ws to $f (loads at startup)"
}

load_module() {
	[ "$NOLOAD" = 1 ] && return 0
	if [ "$FS_RUNNING" != 1 ]; then info "FreeSWITCH is not running; mod_amd_ws loads at the next start"; return 0; fi
	if [ -z "$FS_CLI" ]; then warn "no fs_cli; load it yourself: load mod_amd_ws"; return 0; fi
	local r
	if [ "$(fs_x 'module_exists mod_amd_ws' | tr -d '\r\n ')" = "true" ]; then
		r=$(fs_x 'reload mod_amd_ws' || true)
		case "$r" in
			*+OK*) ok "reloaded mod_amd_ws in the running switch" ;;
			*"in use"*) warn "mod_amd_ws is in use by calls; the new file is installed, run 'fs_cli -x \"reload mod_amd_ws\"' when idle" ;;
			*) warn "reload said: $(echo "$r" | head -2 | tr '\n' ' ')" ;;
		esac
	else
		r=$(fs_x 'load mod_amd_ws' || true)
		case "$r" in
			*+OK*) ok "loaded mod_amd_ws in the running switch" ;;
			*) die "load failed: $(echo "$r" | head -3 | tr '\n' ' ') (see the FreeSWITCH log)" ;;
		esac
	fi
	fs_x 'amd_ws status' | sed -n '1,4p' | sed 's/^/       /' || true
}

uninstall() {
	[ "$(id -u)" = 0 ] || die "run as root"
	[ -n "$FS_MODDIR" ] || die "cannot find the module directory (--moddir)"
	if [ "$FS_RUNNING" = 1 ] && [ -n "$FS_CLI" ] && [ "$(fs_x 'module_exists mod_amd_ws' | tr -d '\r\n ')" = "true" ]; then
		case "$(fs_x 'unload mod_amd_ws' || true)" in
			*+OK*) ok "unloaded mod_amd_ws" ;;
			*) die "cannot unload mod_amd_ws (in use by calls?); retry when idle" ;;
		esac
	fi
	rm -f "$FS_MODDIR/mod_amd_ws.so" && ok "removed $FS_MODDIR/mod_amd_ws.so"
	local f="$FS_CONFDIR/autoload_configs/modules.conf.xml"
	if [ -f "$f" ] && grep -q '<load module="mod_amd_ws"/>' "$f"; then
		cp -p "$f" "$f.bak.$(date +%Y%m%d%H%M%S)"
		sed -i '/<load module="mod_amd_ws"\/>/d' "$f"
		ok "removed the autoload line from $f"
	fi
	info "kept $FS_CONFDIR/autoload_configs/amd_ws.conf.xml"
}

# ---------------------------------------------------------------------------
main() {
	parse_args "$@"
	if [ "$DRYRUN" = 1 ] || [ "$BUILDONLY" = 1 ] || [ "$(id -u)" != 0 ]; then LOG=/tmp/mod_amd_ws-install.log; fi
	: >"$LOG" 2>/dev/null || LOG=/dev/null
	say "mod_amd_ws $MOD_VERSION installer $( [ "$DRYRUN" = 1 ] && echo '(dry run: nothing is changed)')"
	[ "$DRYRUN" = 1 ] || [ "$BUILDONLY" = 1 ] || [ "$(id -u)" = 0 ] || die "run as root (or use --dry-run / --build-only)"

	extract_payload
	[ -n "$OPT_MODDIR" ] && FS_MODDIR=$OPT_MODDIR
	[ -n "$OPT_CONFDIR" ] && FS_CONFDIR=$OPT_CONFDIR
	detect_all || true
	[ -n "$OPT_MODDIR" ] && FS_MODDIR=$OPT_MODDIR
	[ -n "$OPT_CONFDIR" ] && FS_CONFDIR=$OPT_CONFDIR

	[ -n "$FS_BIN" ] || die "FreeSWITCH not found (no running freeswitch, none in PATH or the usual places)"
	ok "FreeSWITCH ${FS_VERSION:-(version unknown)} at $FS_BIN ($FS_PKG install$( [ "$FS_RUNNING" = 1 ] && echo ", running, pid $FS_PID" || echo ", not running"))"
	ok "module dir ${FS_MODDIR:-(unknown)}   conf dir ${FS_CONFDIR:-(unknown)}"
	case "$FS_VERSION" in
		1.10.*|1.11.*|"") ;;
		*) warn "tested with FreeSWITCH 1.10.x/1.11.x; $FS_VERSION may work" ;;
	esac

	if [ "$UNINSTALL" = 1 ]; then uninstall; exit 0; fi

	install_packages
	find_headers
	if [ "$DRYRUN" = 1 ]; then
		info "would build mod_amd_ws.so and install it into ${FS_MODDIR:-?}"
		[ "$NOCONFIG" = 1 ] || info "would create ${FS_CONFDIR:-?}/autoload_configs/amd_ws.conf.xml if absent (host $CFG_HOST:$CFG_PORT)"
		[ "$NOAUTOLOAD" = 1 ] || info "would add <load module=\"mod_amd_ws\"/> to modules.conf.xml if absent"
		[ "$NOLOAD" = 1 ] || info "would load/reload mod_amd_ws in the running switch"
		exit 0
	fi
	build
	if [ "$BUILDONLY" = 1 ]; then
		cp "$WORK/src/mod_amd_ws.so" ./mod_amd_ws.so
		ok "wrote $(pwd)/mod_amd_ws.so (nothing installed)"
		exit 0
	fi
	[ -n "$FS_MODDIR" ] || die "cannot determine the module directory; pass --moddir"
	install_module
	[ -n "$FS_CONFDIR" ] && install_config && install_autoload
	load_module
	say ""
	say "Done.  Dialplan example:"
	say '  <action application="amd_ws" data=""/>          (endpoint from amd_ws.conf.xml)'
	say '  <action application="log" data="INFO AMD ${AMDSTATUS}/${AMDCAUSE} ${AMDSTATS}"/>'
	say "Check:  fs_cli -x 'amd_ws status'      Docs: https://github.com/nikvb/mod_amd_ws"
}

payload() {
	cat <<'__MOD_AMD_WS_PAYLOAD__'
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
__MOD_AMD_WS_PAYLOAD__
}

main "$@"
