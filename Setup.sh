#!/usr/bin/env bash
#
# First-time setup on Linux, and the counterpart to Setup.bat: check prerequisites, build
# LuminaBuildTool, then fetch the external dependency bundle.
#
#   ./Setup.sh            interactive
#   ./Setup.sh -Yes       non-interactive
#
# SKIP_PREREQ_CHECKS=1 skips the prerequisite check.

set -uo pipefail

LuminaDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo
echo "============================================================"
echo " LUMINA ENGINE SETUP"
echo "============================================================"
echo " Working directory: $LuminaDir"
echo

# --- prerequisites --------------------------------------------------------------------------
#
# Checked here rather than left to the first confusing compiler error. Everything below is needed
# to get as far as a linked editor; the X11 packages are GLFW's, and the build fails at link
# without them rather than at configure time (see GLFW.Build.cs).

# dotnet-install.sh drops .NET in $HOME/.dotnet and persists nothing, so a new shell loses it and
# every script here fails claiming the SDK is missing when it is sitting right there.
if ! command -v dotnet >/dev/null 2>&1 && [ -x "$HOME/.dotnet/dotnet" ]; then
    export PATH="$HOME/.dotnet:$PATH"
fi

if [ -z "${SKIP_PREREQ_CHECKS:-}" ]; then
    Missing=()

    command -v dotnet >/dev/null 2>&1 || Missing+=("dotnet (.NET 10 SDK)")

    # The tree needs <format>, which means GCC 13 / libstdc++ 13 or Clang with a libstdc++ that new.
    if command -v g++ >/dev/null 2>&1; then
        GccMajor="$(g++ -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)"
        if [ -n "$GccMajor" ] && [ "$GccMajor" -lt 13 ]; then
            echo "warning: g++ $GccMajor is too old for this tree (<format> needs 13+)."
            echo "         Install g++-13 or newer, or set CXX to a newer compiler."
            echo
        elif [ -n "$GccMajor" ] && [ "$GccMajor" -gt 15 ]; then
            echo "warning: g++ $GccMajor is newer than anything this tree has been built with (13-15)."
            echo "         Pre-release compilers reject old third-party code for reasons that are not"
            echo "         bugs in this engine. If the build fails inside External/ or ThirdParty/,"
            echo "         pin a stable compiler first:"
            echo "           sudo apt-get install -y g++-15 && export CXX=g++-15 CC=gcc-15"
            echo
        fi
    elif ! command -v clang++ >/dev/null 2>&1; then
        Missing+=("g++ 13+ or clang++")
    fi

    # GLFW links these directly; without the headers the build gets a long way and then fails.
    for Package in x11 xrandr xinerama xcursor xi xkbcommon; do
        if command -v pkg-config >/dev/null 2>&1 && ! pkg-config --exists "$Package" 2>/dev/null; then
            Missing+=("lib${Package}-dev")
        fi
    done

    if [ ${#Missing[@]} -gt 0 ]; then
        echo "error: missing prerequisites:"
        printf '  - %s\n' "${Missing[@]}"
        echo
        echo "On Debian or Ubuntu:"
        echo "  sudo apt-get install -y g++-13 libx11-dev libxrandr-dev libxinerama-dev \\"
        echo "      libxcursor-dev libxi-dev libxkbcommon-dev pkg-config"
        echo "  # .NET: https://dotnet.microsoft.com/download/dotnet/10.0"
        echo
        echo "Set SKIP_PREREQ_CHECKS=1 to bypass this check."
        exit 1
    fi
fi

# --- LUMINA_DIR -----------------------------------------------------------------------------
#
# There is no user environment store to persist this to, unlike the Windows registry, so it is
# exported for this process and the user is told how to make it stick.

export LUMINA_DIR="$LuminaDir"

if ! grep -qs "LUMINA_DIR=" "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc" 2>/dev/null; then
    echo "note: LUMINA_DIR is set for this run only. Standalone game projects locate the engine"
    echo "      through it, so add this to your shell profile to make it permanent:"
    echo
    echo "        export LUMINA_DIR=\"$LuminaDir\""
    echo
fi

# --- dependencies and hooks -----------------------------------------------------------------

"$LuminaDir/LuminaBuild.sh" Setup "$@"
Status=$?

if [ $Status -ne 0 ]; then
    echo
    echo "Setup failed."
    exit $Status
fi

echo
echo "============================================================"
echo " Setup complete."
echo
echo " Next, in order:"
echo "   ./LuminaBuild.sh Build Reflector -TargetType=Program"
echo "   ./LuminaBuild.sh Build Lumina -TargetType=Editor"
echo "============================================================"
