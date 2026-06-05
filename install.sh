#!/usr/bin/env bash
# tmux-animated installer.
#
# clones upstream tmux at the version this patch was made against,
# applies the animation patch, builds, and installs as `tmux-animated`.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/jonaburg/tmux-animated/animations/install.sh | bash
#
# Override the install prefix:
#   PREFIX=/opt/local curl ... | bash
set -euo pipefail

REPO_RAW="https://raw.githubusercontent.com/jonaburg/tmux-animated/animations"
PATCH_NAME="tmux-animated-next-3.7.patch"
TMUX_REPO="https://github.com/tmux/tmux.git"
# The upstream commit the patch was generated against. TODO: Update this when
# the patch is rebased onto a new upstream tmux.
TMUX_REF="f43adc36"

PREFIX="${PREFIX:-/usr/local}"
BIN_NAME="tmux-animated"

WORKDIR="$(mktemp -d -t tmux-animated.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
err() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }


missing=()
for cmd in cc make patch git curl pkg-config autoconf automake bison; do
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
done

# libevent and ncurses are libraries, not commands; check via pkg-config if available.
if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --exists libevent 2>/dev/null || missing+=("libevent (dev headers)")
    pkg-config --exists ncurses 2>/dev/null || missing+=("ncurses (dev headers)")
fi

if [ "${#missing[@]}" -gt 0 ]; then
    printf '\033[1;31merror:\033[0m missing required tools/libraries: %s\n' "${missing[*]}" >&2
    echo >&2
    echo "Install hints:" >&2
    case "$(uname -s)" in
        Linux)
            if [ -r /etc/os-release ]; then . /etc/os-release; fi
            case "${ID:-}${ID_LIKE:-}" in
                *debian*|*ubuntu*)
                    echo "  sudo apt-get install -y build-essential pkg-config autoconf automake libevent-dev libncurses-dev bison patch git curl" >&2
                    ;;
                *fedora*|*rhel*|*centos*)
                    echo "  sudo dnf install -y gcc make patch git curl pkgconf-pkg-config autoconf automake libevent-devel ncurses-devel bison" >&2
                    ;;
                *arch*)
                    echo "  sudo pacman -S --needed base-devel pkgconf autoconf automake libevent ncurses bison patch git curl" >&2
                    ;;
                *alpine*)
                    echo "  sudo apk add build-base pkgconf autoconf automake libevent-dev ncurses-dev bison patch git curl" >&2
                    ;;
                *)
                    echo "  Install: build tools, pkg-config, autoconf, automake, libevent-dev, ncurses-dev, bison" >&2
                    ;;
            esac
            ;;
        Darwin)
            echo "  brew install pkg-config autoconf automake libevent ncurses" >&2
            ;;
    esac
    exit 1
fi

if [ "$(uname -s)" = "Darwin" ] && command -v brew >/dev/null 2>&1; then
    export PKG_CONFIG_PATH="$(brew --prefix libevent)/lib/pkgconfig:$(brew --prefix ncurses)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
fi


say "Cloning tmux at $TMUX_REF"
git clone --quiet "$TMUX_REPO" "$WORKDIR/tmux"
cd "$WORKDIR/tmux"
git checkout --quiet "$TMUX_REF"

say "Downloading patch"
curl -fsSL "$REPO_RAW/patches/$PATCH_NAME" -o "$WORKDIR/$PATCH_NAME"

say "Applying patch"
patch -p1 -s < "$WORKDIR/$PATCH_NAME"


sed -i.bak 's/^bin_PROGRAMS = tmux$/bin_PROGRAMS = '"$BIN_NAME"'/' Makefile.am
sed -i.bak 's/^tmux_SOURCES/'"$BIN_NAME"'_SOURCES/g' Makefile.am
sed -i.bak 's/^tmux_LDADD/'"$BIN_NAME"'_LDADD/g' Makefile.am
sed -i.bak 's/^dist_tmux_SOURCES/dist_'"$BIN_NAME"'_SOURCES/g' Makefile.am
rm -f Makefile.am.bak


say "Bootstrapping"
./autogen.sh >/dev/null

say "Configuring (prefix=$PREFIX)"
EXTRA_CONFIGURE=""
if [ "$(uname -s)" = "Darwin" ]; then
    EXTRA_CONFIGURE="--disable-utf8proc"
fi
./configure --prefix="$PREFIX" $EXTRA_CONFIGURE >/dev/null

say "Building"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null

say "Installing to $PREFIX/bin/$BIN_NAME"
if [ -w "$PREFIX/bin" ]; then
    make install >/dev/null
else
    sudo make install >/dev/null
fi

say "Done. Run: $BIN_NAME ($PREFIX/bin/$BIN_NAME)"
