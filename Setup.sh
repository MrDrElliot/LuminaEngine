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

# --- Vulkan at run time ---------------------------------------------------------------------
#
# Warnings, never errors. None of this is needed to compile: the Vulkan headers are vendored under
# Engine/Source/ThirdParty/vulkan and volk resolves the entry points with dlopen at startup, so a
# machine with no driver at all still builds a working editor it cannot launch. The point of
# checking now is that the alternative is finding out after a full build.

if [ -z "${SKIP_PREREQ_CHECKS:-}" ]; then
    VulkanNotes=()

    # Nothing below may be written as `producer | grep -q`. This script runs under pipefail, and
    # grep -q exits at the first match, which SIGPIPEs the producer and fails the whole pipeline --
    # so a successful match reads as a failed check and every probe here reports the opposite of
    # what it found. Capture first, match against the variable.

    # ldconfig is an administrative tool and lives in sbin, which is not on a normal user's PATH.
    # Looked up by absolute path as well, or this check answers "no Vulkan loader" on every machine
    # that has one.
    LdConfigBin=""
    for Candidate in ldconfig /usr/sbin/ldconfig /sbin/ldconfig; do
        if command -v "$Candidate" >/dev/null 2>&1; then
            LdConfigBin="$Candidate"
            break
        fi
    done

    # dlopen'd by SONAME, so what matters is that the dynamic linker can find that exact name --
    # not that a -dev symlink or a header exists. Skipped rather than guessed where there is no
    # ldconfig to ask, which is most non-glibc systems.
    if [ -n "$LdConfigBin" ]; then
        LibraryCache="$("$LdConfigBin" -p 2>/dev/null)"

        case "$LibraryCache" in
            *libvulkan.so.1*) ;;
            *) VulkanNotes+=("No libvulkan.so.1 on the library path. Install the Vulkan loader (Debian/Ubuntu: libvulkan1).") ;;
        esac
    fi

    # A loader with no ICD manifest enumerates zero devices and the editor exits reporting no
    # suitable GPU, which reads as an engine fault rather than a missing driver.
    if ! ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1 \
        && ! ls /etc/vulkan/icd.d/*.json >/dev/null 2>&1 \
        && [ -z "${VK_ICD_FILENAMES:-}" ] && [ -z "${VK_DRIVER_FILES:-}" ]; then
        VulkanNotes+=("No Vulkan driver manifest found in /usr/share/vulkan/icd.d. Install your GPU vendor's Vulkan driver (mesa-vulkan-drivers, or the proprietary NVIDIA driver).")
    fi

    # The renderer's hard floor. The device is picked at startup and a machine whose only GPU lacks
    # mesh shaders is refused outright, so it is worth saying before the build rather than after.
    if command -v vulkaninfo >/dev/null 2>&1; then
        VulkanReport="$(vulkaninfo 2>/dev/null)"

        case "$VulkanReport" in
            *VK_EXT_mesh_shader*) ;;
            *) VulkanNotes+=("No installed GPU reports VK_EXT_mesh_shader. The renderer requires it: NVIDIA Turing (GTX 16 / RTX 20) or newer, AMD RDNA2 (RX 6000) or newer, or Intel Arc.") ;;
        esac
    else
        VulkanNotes+=("vulkaninfo is not installed, so the GPU could not be checked. The renderer requires VK_EXT_mesh_shader: NVIDIA Turing or newer, AMD RDNA2 or newer, or Intel Arc. (Debian/Ubuntu: vulkan-tools)")
    fi

    if [ ${#VulkanNotes[@]} -gt 0 ]; then
        echo "warning: the editor needs a working Vulkan runtime to start. The build does not."
        printf '  - %s\n' "${VulkanNotes[@]}"
        echo
    fi
fi

# --- LUMINA_DIR -----------------------------------------------------------------------------
#
# There is no user environment store to persist this to, unlike the Windows registry, so the
# closest equivalent is the login shell's profile. Offered rather than done: a setup script that
# edits a shell profile unasked is not a trade most people would accept for one variable.

export LUMINA_DIR="$LuminaDir"

# Whichever file the user's own shell actually reads, so the export takes effect on next login
# instead of sitting in a file that shell never sources.
case "$(basename "${SHELL:-}")" in
    zsh)  ShellProfile="$HOME/.zshrc" ;;
    bash) ShellProfile="$HOME/.bashrc" ;;
    *)    ShellProfile="$HOME/.profile" ;;
esac

if grep -qs "LUMINA_DIR=" "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc" 2>/dev/null; then
    echo "LUMINA_DIR is already exported from a shell profile; leaving it alone."
else
    PersistLuminaDir=""

    # Prompted only when there is somebody to answer. Under -Yes, in CI, or with stdin redirected,
    # printing the line to add is the right answer: an unattended run has no business rewriting the
    # profile of whatever account it happens to be running as.
    if [ -t 0 ] && [ -z "${LUMINA_SETUP_YES:-}" ] && [[ " $* " != *" -Yes "* ]] && [[ " $* " != *" --yes "* ]]; then
        echo "Standalone game projects locate the engine through LUMINA_DIR, which is set for this"
        echo "run only unless it goes into a shell profile."
        echo
        read -r -p "Add 'export LUMINA_DIR=\"$LuminaDir\"' to $ShellProfile? [y/N] " Answer
        case "$Answer" in
            [Yy]*) PersistLuminaDir="1" ;;
        esac
    fi

    if [ -n "$PersistLuminaDir" ]; then
        {
            echo
            echo "# Lumina Engine root, added by Setup.sh"
            echo "export LUMINA_DIR=\"$LuminaDir\""
        } >> "$ShellProfile"

        echo "Added to $ShellProfile. It applies to shells started from now on."
    else
        echo "note: LUMINA_DIR is set for this run only. Standalone game projects locate the engine"
        echo "      through it, so add this to your shell profile to make it permanent:"
        echo
        echo "        export LUMINA_DIR=\"$LuminaDir\""
    fi

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
echo " Build and run the editor:"
echo "   ./LuminaBuild.sh Build Lumina -TargetType=Editor"
echo "   ./LuminaBuild.sh Run Lumina -TargetType=Editor"
echo
echo " The Reflector is a prerequisite of that target and builds itself."
echo
echo " For code completion, run ./GenerateProjectFiles.sh and point your"
echo " editor at the compile_commands.json it writes here."
echo "============================================================"
