#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

banner()  { echo -e "\n${CYAN}═══ $* ═══${NC}"; }
ok()      { echo -e "  ${GREEN}✓${NC} $*"; }
warn()    { echo -e "  ${YELLOW}⚠${NC} $*"; }
die()     { echo -e "${RED}✗ $*${NC}" >&2; exit 1; }

OS=$(uname -s)
ARCH=$(uname -m)

case "$OS" in
    Darwin)
        JOBS=$(sysctl -n hw.ncpu)
        OS_NAME="macOS"
        ;;
    Linux)
        JOBS=$(nproc)
        OS_NAME="Linux"
        ;;
    *)
        die "Unsupported OS: $OS"
        ;;
esac

echo -e "${CYAN}╔══════════════════════════════════════════╗"
printf "\033[0;36m║  %-8s %-29s║\n" "build:" "${OS_NAME} ${ARCH}"
printf "║  %-8s %-29s║\n" "jobs:" "${JOBS}"
echo -e "╚══════════════════════════════════════════╝${NC}"

# ── 1. Check system dependencies ────────────────────────────────
banner "[1/4] Checking system dependencies"

# Compiler
CC=$(which gcc 2>/dev/null || which cc 2>/dev/null || echo "")
if [ -z "$CC" ]; then
    case "$OS" in
        Darwin) die "No C compiler found. Install Xcode Command Line Tools:  xcode-select --install" ;;
        Linux)  die "No C compiler found. Install:  sudo apt install build-essential   (or equivalent)" ;;
    esac
fi
ok "Compiler: $($CC --version | head -1)"

# make
if ! command -v make &>/dev/null; then
    die "make not found"
fi
ok "make: $(make --version | head -1)"

# libcurl
check_curl() {
    if ld -lcurl -o /dev/null 2>/dev/null; then
        return 0
    fi
    if [ "$OS" = "Darwin" ]; then
        local sdk
        sdk=$(xcrun --sdk macosx --show-sdk-path 2>/dev/null) || return 1
        [ -f "$sdk/usr/lib/libcurl.tbd" ] && return 0
        [ -f "$sdk/usr/lib/libcurl.4.tbd" ] && return 0
    fi
    return 1
}
if check_curl; then
    ok "libcurl: available"
else
    case "$OS" in
        Darwin) die "libcurl not found. Install:  brew install curl   or   xcode-select --install" ;;
        Linux)  die "libcurl not found. Install:  sudo apt install libcurl4-openssl-dev   (or equivalent)" ;;
    esac
fi

# macOS-specific: SDK check
if [ "$OS" = "Darwin" ]; then
    SDK_PATH=$(xcrun --sdk macosx --show-sdk-path 2>/dev/null) || {
        die "macOS SDK not found. Install:  xcode-select --install"
    }
    ok "macOS SDK: $SDK_PATH"
fi

# ── 2. Build ncurses from source ─────────────────────────────────
banner "[2/4] Building ncurses"

NCURSES_SRC="vendor/ncurses-src"
NCURSES_INSTALL="vendor/ncurses-install"

if [ -f "$NCURSES_INSTALL/lib/libncursesw.a" ] && [ -f "$NCURSES_INSTALL/lib/libtinfow.a" ]; then
    ok "ncurses already built, skipping"
else
    if [ ! -d "$NCURSES_SRC" ]; then
        die "ncurses source not found at $NCURSES_SRC"
    fi

    echo "  Configuring ncurses for ${OS_NAME} ${ARCH}..."
    (
        cd "$NCURSES_SRC"

        # Wipe artefacts from other platforms
        if [ -f Makefile ] && grep -q 'Makefile' Makefile 2>/dev/null; then
            make distclean 2>/dev/null || true
        fi

        # Ensure install directory exists so configure can resolve absolute prefix
        NCURSES_PREFIX="$(cd ../.. && pwd)/vendor/ncurses-install"
        mkdir -p "$NCURSES_PREFIX"

        ./configure \
            --enable-widec \
            --with-termlib \
            --without-tests \
            --without-shared \
            --without-cxx-binding \
            --prefix="$NCURSES_PREFIX"
    )

    echo "  Compiling ncurses (${JOBS} jobs)..."
    make -C "$NCURSES_SRC" -j"$JOBS"

    echo "  Installing ncurses to vendor/ncurses-install..."
    make -C "$NCURSES_SRC" install

    ok "ncurses build complete"
fi

# Verify the right libs exist
for lib in libncursesw.a libtinfow.a; do
    [ -f "$NCURSES_INSTALL/lib/$lib" ] || die "Missing: $NCURSES_INSTALL/lib/$lib"
done
ok "ncurses libraries verified"

# ── 3. Build Lua ─────────────────────────────────────────────────
banner "[3/4] Building Lua"

LUA_DIR="vendor/lua-5.5.0"
if [ ! -d "$LUA_DIR" ]; then
    echo "  Downloading Lua 5.5.0..."
    (
        cd vendor
        curl -L -R -O https://www.lua.org/ftp/lua-5.5.0.tar.gz
        tar zxf lua-5.5.0.tar.gz
    )
fi

echo "  Building Lua (${JOBS} jobs)..."
make -C "$LUA_DIR" clean 2>/dev/null || true
make -C "$LUA_DIR" -j"$JOBS" all CC="$CC"
# test can be racy with -j; run single-threaded after all
make -C "$LUA_DIR" test CC="$CC" 2>/dev/null || true

[ -f "$LUA_DIR/src/liblua.a" ] || die "Lua library not built"
ok "Lua build complete"

# ── 4. Build tui-agent ───────────────────────────────────────────
banner "[4/4] Building tui-agent"

rm -rf build
make -j"$JOBS"

if [ ! -f "build/termai" ]; then
    die "Build failed — no binary produced"
fi

# ── Done ─────────────────────────────────────────────────────────
SIZE=$(ls -lh build/termai | awk '{print $5}')
BIN_ARCH=$(file build/termai | grep -o 'arm64\|x86_64' || echo "$ARCH")

echo ""
echo -e "${GREEN}╔══════════════════════════════════════════╗"
echo -e "║  Build successful!                      ║"
echo -e "╠══════════════════════════════════════════╣"
printf "\033[0;32m║  %-10s %-28s║\n" "binary:" "build/termai"
printf "║  %-10s %-28s║\n" "size:" "$SIZE"
printf "║  %-10s %-28s║\n" "arch:" "$BIN_ARCH"
echo -e "╚══════════════════════════════════════════╝${NC}"
echo ""
echo -e "  Run:  ${GREEN}./build/termai${NC}"
echo ""
