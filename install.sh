#!/bin/sh
# Build beige and a static HTTPS stack on an old Linux system.
#
# Downloads OpenSSL, curl, and a current CA bundle, builds the libraries
# static (no libpsl, nghttp2, brotli, zstd, or zlib), then builds beige
# from this directory and installs it.
#
# Needs gcc, make, and perl already, or root so the script can ask the
# distro for them. OpenSSL is built with --with-rand-seed=devrandom,os so
# it does not call getrandom (that syscall arrived in Linux 3.17).
#
# If this machine's wget or curl cannot speak to current HTTPS sites, copy
# these files into the cache directory the script prints and run it again:
#   openssl-3.5.9.tar.gz
#   curl-8.23.0.tar.gz
#   cacert.pem
#
#   PREFIX=/usr/local DEPS=/opt/beige-deps sh install.sh
#   BUILD=/tmp/beige-build JOBS=1 sh install.sh
#   BEIGE_REBUILD=1 sh install.sh
set -eu

OPENSSL_VER=3.5.9
CURL_VER=8.23.0
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"
OPENSSL_SUM_URL="${OPENSSL_URL}.sha256"
CURL_URL="https://curl.se/download/curl-${CURL_VER}.tar.gz"
CA_URL="https://curl.se/ca/cacert.pem"

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
umask 022

say() {
    printf '%s\n' "$*"
}

die() {
    printf 'install.sh: %s\n' "$*" >&2
    exit 1
}

have() {
    command -v "$1" >/dev/null 2>&1
}

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif have sudo; then
        sudo "$@"
    else
        return 1
    fi
}

ensure_toolchain() {
    if have gcc || have cc; then
        if have make && have perl; then
            return 0
        fi
    fi
    say "gcc, make, and perl are required"
    if [ "$(id -u)" -ne 0 ] && ! have sudo; then
        die "install gcc, make, and perl, then run this script again"
    fi
    if have apt-get; then
        as_root apt-get update
        as_root apt-get install -y gcc make perl libc6-dev
    elif have dnf; then
        as_root dnf install -y gcc make perl glibc-devel
    elif have yum; then
        as_root yum install -y gcc make perl glibc-devel
    elif have zypper; then
        as_root zypper --non-interactive install gcc make perl glibc-devel
    elif have apk; then
        as_root apk add gcc make perl musl-dev
    elif have pacman; then
        as_root pacman -Sy --noconfirm gcc make perl
    else
        die "no known package manager; install gcc, make, and perl"
    fi
    have gcc || have cc || die "gcc is still missing"
    have make || die "make is still missing"
    have perl || die "perl is still missing"
}

pick_prefix() {
    if [ -n "${PREFIX:-}" ]; then
        return 0
    fi
    if [ -w /usr/local ] || [ "$(id -u)" -eq 0 ]; then
        PREFIX=/usr/local
    else
        PREFIX=$HOME/.local
    fi
}

pick_deps() {
    if [ -n "${DEPS:-}" ]; then
        return 0
    fi
    if [ -w /opt ] || [ "$(id -u)" -eq 0 ]; then
        DEPS=/opt/beige-deps
    else
        DEPS=$HOME/.local/beige-deps
    fi
}

job_count() {
    if [ -n "${JOBS:-}" ]; then
        return 0
    fi
    JOBS=1
    if [ -r /proc/cpuinfo ]; then
        n=$(grep -c '^processor' /proc/cpuinfo || true)
        if [ -n "$n" ] && [ "$n" -gt 0 ]; then
            JOBS=$n
        fi
    fi
}

fetch() {
    url=$1
    dest=$2
    if [ -f "$dest" ]; then
        say "using $dest"
        return 0
    fi
    say "downloading $url"
    if have wget; then
        wget -O "$dest.partial" "$url"
    elif have curl; then
        curl -fsSL -o "$dest.partial" "$url"
    else
        die "need wget or curl. Place the file at $dest and run again"
    fi
    mv "$dest.partial" "$dest"
}

fetch_optional() {
    url=$1
    dest=$2
    if [ -f "$dest" ]; then
        return 0
    fi
    if have wget; then
        wget -O "$dest.partial" "$url" || return 1
    elif have curl; then
        curl -fsSL -o "$dest.partial" "$url" || return 1
    else
        return 1
    fi
    mv "$dest.partial" "$dest"
}

verify_sha256() {
    file=$1
    sumfile=$2
    hash=""
    got=""
    if [ ! -f "$sumfile" ]; then
        say "no checksum for $(basename "$file"); continuing"
        return 0
    fi
    hash=$(awk '{
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^[0-9A-Fa-f]{64}$/) {
                print tolower($i)
                exit
            }
        }
    }' "$sumfile")
    if [ -z "$hash" ]; then
        say "could not read a SHA-256 from $sumfile; continuing"
        return 0
    fi
    if have sha256sum; then
        printf '%s  %s\n' "$hash" "$(basename "$file")" > "$CACHE/check.sha256"
        (cd "$(dirname "$file")" && sha256sum -c "$CACHE/check.sha256")
        say "checksum ok $(basename "$file")"
    elif have openssl; then
        got=$(openssl dgst -sha256 "$file" | awk '{ print tolower($NF) }')
        if [ "$got" != "$hash" ]; then
            die "checksum mismatch for $file"
        fi
        say "checksum ok $(basename "$file")"
    else
        say "no sha256sum; skipped checksum for $(basename "$file")"
    fi
}

unpack() {
    archive=$1
    dir=$2
    rm -rf "$dir"
    gzip -dc "$archive" | tar -xf -
    [ -d "$dir" ] || die "archive did not contain $dir"
}

build_openssl() {
    if [ -f "$DEPS/lib/libssl.a" ] && [ -f "$DEPS/lib/libcrypto.a" ] && [ "${BEIGE_REBUILD:-0}" != 1 ]; then
        say "OpenSSL already built in $DEPS"
        return 0
    fi
    fetch "$OPENSSL_URL" "$CACHE/openssl-${OPENSSL_VER}.tar.gz"
    fetch_optional "$OPENSSL_SUM_URL" "$CACHE/openssl-${OPENSSL_VER}.tar.gz.sha256" || true
    verify_sha256 "$CACHE/openssl-${OPENSSL_VER}.tar.gz" "$CACHE/openssl-${OPENSSL_VER}.tar.gz.sha256"
    cd "$BUILD"
    unpack "$CACHE/openssl-${OPENSSL_VER}.tar.gz" "openssl-${OPENSSL_VER}"
    cd "openssl-${OPENSSL_VER}"
    # devrandom avoids the getrandom syscall, which old kernels do not have.
    ./config --prefix="$DEPS" --libdir=lib --openssldir="$DEPS/ssl" \
        --with-rand-seed=devrandom,os no-shared
    make -j"$JOBS"
    make install_sw
}

build_curl() {
    if [ -f "$DEPS/lib/libcurl.a" ] && [ "${BEIGE_REBUILD:-0}" != 1 ]; then
        say "curl already built in $DEPS"
        return 0
    fi
    fetch "$CURL_URL" "$CACHE/curl-${CURL_VER}.tar.gz"
    cd "$BUILD"
    unpack "$CACHE/curl-${CURL_VER}.tar.gz" "curl-${CURL_VER}"
    cd "curl-${CURL_VER}"
    ./configure --prefix="$DEPS" --with-openssl="$DEPS" \
        --disable-shared --enable-static \
        --without-libpsl --without-nghttp2 --without-brotli --without-zstd \
        --without-zlib --without-libidn2 --without-librtmp --without-libssh2 \
        --disable-ldap --disable-manual --disable-alt-svc
    make -j"$JOBS"
    make install
}

# System libraries the static curl and OpenSSL need, as curl's configure found them. On glibc before 2.17
# that includes -lrt for clock_gettime.
static_extra_libs() {
    [ -x "$DEPS/bin/curl-config" ] || return 0
    for word in $("$DEPS/bin/curl-config" --static-libs 2>/dev/null); do
        case "$word" in
            -lcurl|-lssl|-lcrypto) ;;
            -l*|-pthread) printf '%s ' "$word" ;;
        esac
    done
}

build_beige() {
    [ -f "$ROOT/Makefile" ] || die "run install.sh from the beige source tree"
    cd "$ROOT"
    extra=$(static_extra_libs)
    make clean
    make \
        CC="${CC:-gcc}" \
        CURL_CFLAGS="-I$DEPS/include" \
        CURL_LIBS="$DEPS/lib/libcurl.a $DEPS/lib/libssl.a $DEPS/lib/libcrypto.a $extra -ldl -lpthread -lm"
    [ -x "$ROOT/beige" ] || die "beige did not link"
}

install_tree() {
    bindir=$PREFIX/bin
    sharedir=$PREFIX/share/beige
    if [ ! -d "$bindir" ]; then
        mkdir -p "$bindir" || as_root mkdir -p "$bindir" || die "cannot create $bindir"
    fi
    if [ -w "$bindir" ]; then
        cp "$ROOT/beige" "$bindir/beige"
        chmod 755 "$bindir/beige"
        mkdir -p "$sharedir"
        cp "$CACHE/cacert.pem" "$sharedir/cacert.pem"
    else
        as_root cp "$ROOT/beige" "$bindir/beige"
        as_root chmod 755 "$bindir/beige"
        as_root mkdir -p "$sharedir"
        as_root cp "$CACHE/cacert.pem" "$sharedir/cacert.pem"
    fi
}

install_ca_home() {
    home=$1
    who=$2
    [ -n "$home" ] || return 0
    [ -d "$home" ] || return 0
    dest=$home/.beige
    mkdir -p "$dest"
    cp "$CACHE/cacert.pem" "$dest/cacert.pem"
    chmod 700 "$dest"
    chmod 644 "$dest/cacert.pem"
    if [ "$(id -u)" -eq 0 ] && [ -n "$who" ]; then
        chown "$who" "$dest" "$dest/cacert.pem" || true
    fi
}

main() {
    ensure_toolchain
    if have gcc; then
        CC=${CC:-gcc}
    else
        CC=${CC:-cc}
    fi
    export CC
    pick_prefix
    pick_deps
    job_count
    BUILD=${BUILD:-$DEPS/src}
    CACHE=${CACHE:-$BUILD/cache}
    mkdir -p "$DEPS" "$BUILD" "$CACHE" || die "cannot create $DEPS (try DEPS=\$HOME/.local/beige-deps)"
    [ -f "$ROOT/Makefile" ] || die "install.sh must sit next to the beige Makefile"
    say "compiler: $CC"
    say "prefix:   $PREFIX"
    say "libs:     $DEPS"
    say "cache:    $CACHE"
    say "jobs:     $JOBS"
    build_openssl
    build_curl
    fetch "$CA_URL" "$CACHE/cacert.pem"
    build_beige
    install_tree
    owner=${SUDO_USER:-}
    home=${HOME:-}
    if [ -n "$owner" ]; then
        looked=$(getent passwd "$owner" 2>/dev/null | awk -F: '{ print $6; exit }' || true)
        if [ -n "$looked" ]; then
            home=$looked
        fi
    fi
    install_ca_home "$home" "$owner"
    "$PREFIX/bin/beige" -v
    say "installed $PREFIX/bin/beige"
    say "CA bundle is also in $PREFIX/share/beige/cacert.pem"
    say "beige reads ~/.beige/cacert.pem when the config does not set ca_bundle"
}

main "$@"
