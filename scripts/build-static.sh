#!/bin/sh
# Static beige binaries for old x86 Linux, built with zig cc against musl.
#
#   sh scripts/build-static.sh build NAME                    OpenSSL, curl, and beige, packed into dist/
#   sh scripts/build-static.sh check NAME                    the tests under qemu-user, then an instruction scan
#   sh scripts/build-static.sh smoke NAME IMAGE PLATFORM     the packed binary inside an old distro container
#
# NAME is the oldest CPU the binary runs on:
#   x86_64      any 64-bit x86
#   i686-sse2   Pentium 4, Pentium M, Athlon 64, and later in 32-bit mode
#   i686        Pentium Pro, Pentium II and III, Athlon, Geode LX, VIA C3-2. No SSE.
#   i486        486, Pentium, Pentium MMX, K6, VIA C3. No SSE, MMX, or CMOV.
#
# There is no 80386 build: musl's atomics need cmpxchg, which the 486 introduced, and Linux dropped
# the 386 in 3.8. musl itself needs Linux 2.6 or later.
#
# build needs zig 0.15, make, perl, and curl or wget. check also needs qemu-user, objdump, and
# python3; smoke needs docker (or DOCKER=podman) and python3. Downloads and dependency builds live
# in WORK (default ~/.cache/beige-static). The dependencies rebuild when their versions or flags change.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
WORK=${WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/beige-static}
DIST=${DIST:-$ROOT/dist}
ZIG=${ZIG:-zig}
DOCKER=${DOCKER:-docker}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

say() {
    printf '%s\n' "$*"
}

die() {
    printf 'build-static.sh: %s\n' "$*" >&2
    exit 1
}

have() {
    command -v "$1" >/dev/null 2>&1
}

# The dependency versions are install.sh's, so the two builds cannot drift. The checksums are for those
# versions; a version bump there fails here until they are updated.
install_var() {
    sed -n "s/^$1=\([^ ]*\)\$/\1/p" "$ROOT/install.sh"
}

OPENSSL_VER=$(install_var OPENSSL_VER)
CURL_VER=$(install_var CURL_VER)
[ -n "$OPENSSL_VER" ] && [ -n "$CURL_VER" ] || die "cannot read OPENSSL_VER and CURL_VER from install.sh"
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
CURL_SHA256=e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"
CURL_URL="https://curl.se/download/curl-${CURL_VER}.tar.gz"
CA_URL="https://curl.se/ca/cacert.pem"
MUSL_COPYRIGHT_URL="https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v1.2.5"

# devrandom first: the getrandom syscall arrived in Linux 3.17. /etc/ssl is only a default; beige
# passes its CA bundle to curl, and curl does not load an openssl.cnf.
OPENSSL_FLAGS="no-shared no-module no-tests no-apps no-docs --with-rand-seed=devrandom,os --openssldir=/etc/ssl"

# Only http and https, and nothing from the build machine: no CA path, no system openssl.cnf.
CURL_FLAGS="--disable-shared --enable-static --disable-docs --disable-manual
    --without-ca-bundle --without-ca-path --disable-openssl-auto-load-config
    --without-libpsl --without-nghttp2 --without-brotli --without-zstd --without-zlib
    --without-libidn2 --without-libssh2 --disable-ldap --disable-alt-svc --disable-ipfs
    --disable-ftp --disable-file --disable-dict --disable-gopher --disable-imap --disable-pop3
    --disable-smtp --disable-telnet --disable-tftp --disable-rtsp --disable-mqtt"

# Instructions a CPU lacks, as they appear in objdump -d. Multi-byte NOPs are missing on the Geode
# and early VIA chips even though they are i686-class.
SIMD='%[xyz]mm[0-9]|%mm[0-7]|mxcsr|[lms]fence|prefetch|movnt|clflush|emms'
NOPL='nop[lw]'
P6='cmov|fcomi|fucomi|fcmov|endbr'
P5='cmpxchg8b'

# On 32-bit x86 clang cannot prove a uint64_t is 8-byte aligned, so OpenSSL's 64-bit atomics become
# libatomic calls (__atomic_is_lock_free, __atomic_compare_exchange) that zig does not provide.
# This switches OpenSSL to its mutex fallback.
X86_ATOMICS=-DBROKEN_CLANG_ATOMICS

pick() {
    NAME=$1
    case "$NAME" in
        x86_64)
            ZTARGET=x86_64-linux-musl CPU=x86_64 HOST=x86_64-pc-linux-musl
            OSSL_TARGET=linux-x86_64 OSSL_OPTS=
            QEMU="qemu-x86_64 -cpu qemu64"
            FORBID=
            ;;
        i686-sse2)
            # OpenSSL's assembly picks SSE2 and later paths at run time from CPUID.
            ZTARGET=x86-linux-musl CPU=pentium4 HOST=i686-pc-linux-musl
            OSSL_TARGET=linux-x86 OSSL_OPTS=$X86_ATOMICS
            QEMU="qemu-i386 -cpu pentium3,sse2=on"
            FORBID=
            ;;
        i686)
            # i686 rather than pentiumpro: LLVM's pentiumpro may emit multi-byte NOPs.
            ZTARGET=x86-linux-musl CPU=i686 HOST=i686-pc-linux-musl
            OSSL_TARGET=linux-x86 OSSL_OPTS="no-asm $X86_ATOMICS"
            QEMU="qemu-i386 -cpu pentium2"
            FORBID="$SIMD|$NOPL"
            ;;
        i486)
            ZTARGET=x86-linux-musl CPU=i486 HOST=i486-pc-linux-musl
            OSSL_TARGET=linux-x86 OSSL_OPTS="no-asm $X86_ATOMICS"
            QEMU="qemu-i386 -cpu 486"
            FORBID="$SIMD|$NOPL|$P6|$P5"
            ;;
        *)
            die "unknown build '$NAME' (x86_64, i686-sse2, i686, i486)"
            ;;
    esac
    TREE=$WORK/$NAME
    TOOLS=$TREE/bin
    DEPS=$TREE/deps
    B=$TREE/beige
    VERSION=$(sed -n 's/^#define BEIGE_VERSION "\(.*\)"$/\1/p' "$ROOT/src/main.c")
    [ -n "$VERSION" ] || die "cannot read BEIGE_VERSION from src/main.c"
    PKG=beige-$VERSION-linux-$NAME
}

fetch() {
    url=$1
    dest=$2
    if [ -f "$dest" ]; then
        return 0
    fi
    say "downloading $url"
    if have curl; then
        curl -fsSL --retry 3 -o "$dest.partial" "$url"
    elif have wget; then
        wget -q -O "$dest.partial" "$url"
    else
        die "need curl or wget to download $url"
    fi
    mv "$dest.partial" "$dest"
}

sha256_of() {
    if have sha256sum; then
        sha256sum "$1" | awk '{ print $1 }'
    else
        openssl dgst -sha256 "$1" | awk '{ print $NF }'
    fi
}

fetch_checked() {
    fetch "$1" "$2"
    got=$(sha256_of "$2")
    if [ "$got" != "$3" ]; then
        rm -f "$2"
        die "checksum mismatch for $(basename "$2"): got $got. If install.sh moved to a new version, update the SHA256 here"
    fi
}

unpack() {
    rm -rf "$2/$3"
    gzip -dc "$1" | (cd "$2" && tar -xf -)
    [ -d "$2/$3" ] || die "$1 did not contain $3"
}

# zig cc, ar, and ranlib as single commands, which configure scripts handle best. UBSan is off
# because zig cc turns it on, with trapping, for any compile without -O.
make_tools() {
    zig_path=$(command -v "$ZIG") || die "zig not found (set ZIG or add it to PATH)"
    mkdir -p "$TOOLS"
    printf '#!/bin/sh\nexec "%s" cc -target %s -mcpu=%s -fno-sanitize=undefined "$@"\n' \
        "$zig_path" "$ZTARGET" "$CPU" > "$TOOLS/cc"
    printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$zig_path" > "$TOOLS/ar"
    printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$zig_path" > "$TOOLS/ranlib"
    chmod 755 "$TOOLS/cc" "$TOOLS/ar" "$TOOLS/ranlib"
}

deps_stamp() {
    printf '%s\n' \
        "zig $("$ZIG" version) $ZTARGET $CPU" \
        "openssl $OPENSSL_VER $OSSL_TARGET $OSSL_OPTS $OPENSSL_FLAGS" \
        "curl $CURL_VER $CURL_FLAGS"
}

build_openssl() {
    fetch_checked "$OPENSSL_URL" "$WORK/cache/openssl-$OPENSSL_VER.tar.gz" "$OPENSSL_SHA256"
    unpack "$WORK/cache/openssl-$OPENSSL_VER.tar.gz" "$TREE" "openssl-$OPENSSL_VER"
    (
        cd "$TREE/openssl-$OPENSSL_VER"
        # shellcheck disable=SC2086 # the option lists split on purpose
        perl ./Configure "$OSSL_TARGET" $OSSL_OPTS $OPENSSL_FLAGS --prefix="$DEPS" --libdir=lib \
            CC="$TOOLS/cc" AR="$TOOLS/ar" RANLIB="$TOOLS/ranlib"
        make -j"$JOBS" build_libs
        make install_dev
    )
    rm -rf "$TREE/openssl-$OPENSSL_VER"
}

# eventfd with flags needs Linux 2.6.27 and curl has no fallback for it. Without it curl wakes its
# resolver thread through a pipe, which musl creates on any 2.6 kernel.
build_curl() {
    fetch_checked "$CURL_URL" "$WORK/cache/curl-$CURL_VER.tar.gz" "$CURL_SHA256"
    unpack "$WORK/cache/curl-$CURL_VER.tar.gz" "$TREE" "curl-$CURL_VER"
    (
        cd "$TREE/curl-$CURL_VER"
        # shellcheck disable=SC2086
        PKG_CONFIG_LIBDIR=$DEPS/lib/pkgconfig PKG_CONFIG_PATH= \
            ./configure --host="$HOST" --prefix="$DEPS" --with-openssl="$DEPS" $CURL_FLAGS \
            CC="$TOOLS/cc" AR="$TOOLS/ar" RANLIB="$TOOLS/ranlib" CFLAGS=-O2 LDFLAGS=-static \
            ac_cv_func_eventfd=no || {
            # configure's own message rarely names the cause; the compiler and linker errors are in here.
            grep -i 'error' config.log | tail -n 30
            exit 1
        }
        make -j"$JOBS" -C lib
        make -C lib install
        make -C include install
    )
    rm -rf "$TREE/curl-$CURL_VER"
}

build_deps() {
    want=$(deps_stamp)
    if [ -f "$DEPS/stamp" ] && [ "$(cat "$DEPS/stamp")" = "$want" ] && [ -f "$DEPS/lib/libcurl.a" ]; then
        say "dependencies for $NAME are current"
        return 0
    fi
    rm -rf "$DEPS"
    mkdir -p "$DEPS" "$WORK/cache"
    build_openssl
    build_curl
    printf '%s\n' "$want" > "$DEPS/stamp"
}

beige_make() {
    make -C "$B" CC="$TOOLS/cc" CFLAGS="-O2 -Wall -Wextra" LDFLAGS=-static \
        CURL_CFLAGS="-I$DEPS/include" \
        CURL_LIBS="$DEPS/lib/libcurl.a $DEPS/lib/libssl.a $DEPS/lib/libcrypto.a -lpthread" "$@"
}

# A copy of the tree, so a native build in the checkout is left alone.
build_beige() {
    rm -rf "$B"
    mkdir -p "$B"
    cp -R "$ROOT/Makefile" "$ROOT/src" "$ROOT/third_party" "$ROOT/tests" "$B/"
    beige_make clean
    beige_make -j"$JOBS" beige
}

zig_lib_dir() {
    "$ZIG" env | sed -n 's/.*lib_dir"\{0,1\}[^"]*"\([^"]*\)".*/\1/p' | head -n 1
}

package() {
    stage=$TREE/$PKG
    rm -rf "$stage"
    mkdir -p "$stage/licenses" "$DIST"
    if have strip; then
        strip -o "$stage/beige" "$B/beige"
    else
        cp "$B/beige" "$stage/beige"
    fi
    chmod 755 "$stage/beige"
    fetch "$CA_URL" "$stage/cacert.pem"
    cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$stage/"
    gzip -dc "$WORK/cache/openssl-$OPENSSL_VER.tar.gz" | tar -xOf - "openssl-$OPENSSL_VER/LICENSE.txt" > "$stage/licenses/openssl.txt"
    gzip -dc "$WORK/cache/curl-$CURL_VER.tar.gz" | tar -xOf - "curl-$CURL_VER/COPYING" > "$stage/licenses/curl.txt"
    awk '/^\/\*/ { on = 1; next } /\*\// { exit } on' "$ROOT/third_party/cJSON.h" > "$stage/licenses/cjson.txt"
    # zig's compiler_rt is linked in too, under zig's license.
    lib=$(zig_lib_dir)
    if [ -n "$lib" ] && [ -f "$lib/libc/musl/COPYRIGHT" ]; then
        cp "$lib/libc/musl/COPYRIGHT" "$stage/licenses/musl.txt"
    else
        fetch "$MUSL_COPYRIGHT_URL" "$stage/licenses/musl.txt"
    fi
    if [ -n "$lib" ] && [ -f "$lib/../LICENSE" ]; then
        cp "$lib/../LICENSE" "$stage/licenses/zig.txt"
    else
        fetch "https://raw.githubusercontent.com/ziglang/zig/$("$ZIG" version)/LICENSE" "$stage/licenses/zig.txt"
    fi
    cat > "$stage/README" <<EOF
beige $VERSION, static for Linux 2.6 or later on $NAME.

    cp beige /usr/local/bin/
    mkdir -p ~/.beige && cp cacert.pem ~/.beige/

The CA bundle is curl's copy of Mozilla's. beige reads ~/.beige/cacert.pem unless
ca_bundle is set in ~/.beige/config.json. The licenses of the code linked into the
binary (OpenSSL, curl, cJSON, musl, and zig's compiler_rt) are in licenses/.
EOF
    (cd "$TREE" && tar -cf - "$PKG") | gzip -9 > "$DIST/$PKG.tar.gz"
    say "wrote $DIST/$PKG.tar.gz"
}

cmd_build() {
    pick "$1"
    have perl || die "perl is required to configure OpenSSL"
    mkdir -p "$TREE"
    make_tools
    build_deps
    build_beige
    package
}

# Every instruction in the binary, reachable or not, against the CPU's list. musl's fenv code
# touches mxcsr only after AT_HWCAP says the CPU has SSE, so those functions are let through.
scan() {
    if [ -z "$FORBID" ]; then
        say "no instruction scan for $NAME"
        return 0
    fi
    have objdump || die "objdump is required for the instruction scan"
    objdump -d --no-show-raw-insn "$B/beige" | awk -F '\t' -v forbid="$FORBID" -v name="$NAME" '
        /^[0-9a-f]+ <.*>:$/ {
            fn = $0
            sub(/^[0-9a-f]+ </, "", fn)
            sub(/>:$/, "", fn)
            next
        }
        NF >= 2 {
            ins = $2
            sub(/<.*/, "", ins)
            if (ins !~ forbid) {
                next
            }
            if (fn ~ /^(__)?fe[a-z]+$/ && ins ~ /mxcsr/) {
                next
            }
            print fn ": " $2
            bad++
        }
        END {
            if (bad) {
                print bad " instructions an " name " CPU does not have"
                exit 1
            }
            print "instruction scan clean for " name
        }'
}

cmd_check() {
    pick "$1"
    [ -x "$B/beige" ] || die "no $B/beige; run build $NAME first"
    emu=${QEMU%% *}
    have "$emu" || die "$emu not found (install qemu-user)"
    wrapper=$TREE/beige-qemu
    printf '#!/bin/sh\nexec %s "%s" "$@"\n' "$QEMU" "$B/beige" > "$wrapper"
    chmod 755 "$wrapper"
    beige_make test RUN="$QEMU" BEIGE_BIN="$wrapper"
    scan
}

# Runs inside the container with that distro's /bin/sh, which the bash tool uses.
SMOKE_SCRIPT='set -e
cat /etc/redhat-release /etc/debian_version 2>/dev/null || true
BEIGE_HOME=/tmp/beige-home
export BEIGE_HOME
mkdir -p "$BEIGE_HOME"
cd /tmp
/beige/beige -v
for provider in anthropic openai; do
    out=$(/beige/beige --provider $provider --model test --api-key test --base-url "http://127.0.0.1:$PORT" \
        --no-context --mode auto -p "say hi")
    printf "%s\n" "$out"
    printf "%s\n" "$out" | grep beige-ok >/dev/null
    printf "%s\n" "$out" | grep done >/dev/null
done
echo "smoke ok"'

cmd_smoke() {
    pick "$1"
    image=$2
    platform=$3
    tarball=$DIST/$PKG.tar.gz
    [ -f "$tarball" ] || die "no $tarball; run build $NAME first"
    have python3 || die "python3 is required for the mock server"
    tmp=$(mktemp -d)
    mock=
    trap 'if [ -n "$mock" ]; then kill "$mock" 2>/dev/null || true; fi; rm -rf "$tmp"' EXIT
    gzip -dc "$tarball" | (cd "$tmp" && tar -xf -)
    port=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
    python3 "$ROOT/tests/mock_sse.py" "$port" > "$tmp/mock.log" 2>&1 &
    mock=$!
    sleep 1
    "$DOCKER" run --rm --network host --platform "$platform" -e PORT="$port" \
        -v "$tmp/$PKG:/beige:ro,Z" "$image" /bin/sh -c "$SMOKE_SCRIPT"
}

case "${1:-}" in
    build)
        [ $# -eq 2 ] || die "usage: build NAME"
        cmd_build "$2"
        ;;
    check)
        [ $# -eq 2 ] || die "usage: check NAME"
        cmd_check "$2"
        ;;
    smoke)
        [ $# -eq 4 ] || die "usage: smoke NAME IMAGE PLATFORM"
        cmd_smoke "$2" "$3" "$4"
        ;;
    *)
        awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
        exit 2
        ;;
esac
