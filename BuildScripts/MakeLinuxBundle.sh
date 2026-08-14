#!/usr/bin/env bash
#
# Assembles External-Linux64.tar.gz, the prebuilt dependency bundle LuminaBuildTool's Setup mode
# downloads on a Linux host. Run this on Linux (or WSL): the payload carries symlinks and
# executable bits, and neither survives being staged from Windows.
#
# Usage:
#   BuildScripts/MakeLinuxBundle.sh [output-directory]
#
# Then publish it and pin the hash it prints:
#   gh release upload external-deps External-Linux64.tar.gz --clobber
#   # paste the SHA-256 into SetupMode.Bundles (the Linux64 entry's Sha256)
#
# A tarball rather than a zip, deliberately. Slang's Linux libraries use versioned filenames with
# the plain name symlinked onto them, and libclang is packaged the same way; the SONAME recorded in
# the binaries is the versioned one, so flattening the links into copies produces a loader error
# naming a library that is visibly present. Zip also drops the executable bit, which slangc, the
# Tracy tools and the .NET host all need. See SetupMode.ArchiveFormat.

set -euo pipefail

# ---------------------------------------------------------------------------------------------
# Pinned upstreams. The versions live in BuildScripts/DependencyVersions.txt so this script and
# SLang.Build.cs cannot drift apart -- they did, and the build then looked for version-stamped
# Slang libraries the bundle had never fetched.
#
# The engine pins LLVM because the Reflector's libclang parse is sensitive to the version.
# Slang asset naming is slang-<version>-linux-x86_64.tar.gz on the GitHub release.
# ---------------------------------------------------------------------------------------------

VersionsFile="$(dirname "${BASH_SOURCE[0]}")/../Engine/Build/DependencyVersions.txt"
[ -f "$VersionsFile" ] || { echo "error: $VersionsFile not found" >&2; exit 1; }
# shellcheck source=../Engine/Build/DependencyVersions.txt
source "$VersionsFile"

OUTPUT_DIR="${1:-$(pwd)}"
BUNDLE_NAME="External-Linux64.tar.gz"

WORK="$(mktemp -d)"
STAGE="$WORK/External"
trap 'rm -rf "$WORK"' EXIT

Note() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
Fail() { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

for Tool in curl tar; do
    command -v "$Tool" >/dev/null || Fail "$Tool is required."
done

mkdir -p "$STAGE"

# ---------------------------------------------------------------------------------------------
# LLVM / libclang.
#
# Taken from the distribution's own packages rather than an llvm.org tarball. The asset naming on
# the LLVM releases has moved around (clang+llvm-<v>-x86_64-linux-gnu-ubuntu-<rel> on the older
# releases, LLVM-<v>-Linux-X64 more recently) and not every one of them ships libclang.so with the
# clang-c headers. The packages reliably do, and apt.llvm.org carries every version on every
# release, which is what makes this reproducible rather than dependent on the host's default clang.
# ---------------------------------------------------------------------------------------------

Note "LLVM $LLVM_MAJOR (libclang)"

LlvmRoot="/usr/lib/llvm-$LLVM_MAJOR"

if [ ! -d "$LlvmRoot" ]; then
    Fail "$LlvmRoot not found. Install it first:
    wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh $LLVM_MAJOR
    sudo apt-get install -y libclang-$LLVM_MAJOR-dev
  Or set LlvmRoot in this script to an existing LLVM $LLVM_MAJOR installation."
fi

mkdir -p "$STAGE/LLVM/lib" "$STAGE/LLVM/include"

# Three libraries, matching what Reflector.Build.cs links on Linux: the C API, the C++ AST API the
# visitors use directly, and LLVM support (which owns llvm::DisableABIBreakingChecks, referenced by
# the clang C++ headers -- omit it and the link fails on exactly that name).
#
# NOT a plain `cp -a libfoo.so*`. The distribution's lib/libclang.so is a symlink pointing OUT of
# the llvm tree, at ../../x86_64-linux-gnu/libclang-19.so.19; copying it verbatim stages a dangling
# link. Each one is resolved to its real object, copied under the name the SONAME records, and the
# plain -l name recreated as a LOCAL relative symlink.
StageLibrary()
{
    local Stem="$1"
    local LinkName="$2"

    # Searched rather than assumed. Which of these exists depends on which -dev packages are
    # installed: the unversioned .so is a -dev symlink and is often absent even when the runtime
    # library is present, and Debian puts some of these under the multiarch directory instead of the
    # versioned llvm tree.
    local Resolved=""

    for Candidate in \
        "$LlvmRoot/lib/$Stem.so" \
        "$LlvmRoot/lib/$Stem.so.$LLVM_MAJOR" \
        "$LlvmRoot/lib/$Stem.so.$LLVM_MAJOR".* \
        "/usr/lib/x86_64-linux-gnu/$Stem.so" \
        "/usr/lib/x86_64-linux-gnu/$Stem.so.$LLVM_MAJOR" \
        "/usr/lib/x86_64-linux-gnu/$Stem.so.$LLVM_MAJOR".*
    do
        if [ -e "$Candidate" ]; then
            Resolved="$(readlink -f "$Candidate" 2>/dev/null)"
            [ -n "$Resolved" ] && [ -f "$Resolved" ] && break
            Resolved=""
        fi
    done

    if [ -z "$Resolved" ]; then
        Fail "cannot find $Stem for LLVM $LLVM_MAJOR. Install it with:
    sudo apt-get install -y libclang-$LLVM_MAJOR-dev libclang-cpp$LLVM_MAJOR llvm-$LLVM_MAJOR-dev"
    fi

    local Base
    Base="$(basename "$Resolved")"

    cp -f "$Resolved" "$STAGE/LLVM/lib/$Base"
    ln -sf "$Base" "$STAGE/LLVM/lib/$LinkName"

    echo "  $LinkName -> $Base"
}

StageLibrary "libclang"     "libclang.so"
StageLibrary "libclang-cpp" "libclang-cpp.so"
StageLibrary "libLLVM"      "libLLVM-$LLVM_MAJOR.so"

# clang-c alone is not enough. The Reflector's visitors include <clang/AST/Decl.h> directly, and
# those headers include llvm/ ones in turn, so the C++ trees have to come along or the bundle builds
# only on a host that happens to have libclang-dev installed.
#
# Resolved and dereferenced, for the same reason libclang.so is: include/llvm and include/llvm-c are
# themselves symlinks out of the tree (../../../include/llvm-19/llvm), so cp -a stages dangling
# links that only resolve on a machine with the -dev packages installed.
for HeaderTree in clang-c clang llvm-c llvm; do
    Resolved="$(readlink -f "$LlvmRoot/include/$HeaderTree" 2>/dev/null || true)"
    [ -n "$Resolved" ] && [ -d "$Resolved" ] \
        || Fail "$LlvmRoot/include/$HeaderTree is missing. Install libclang-$LLVM_MAJOR-dev and llvm-$LLVM_MAJOR-dev."
    cp -RL "$Resolved" "$STAGE/LLVM/include/$HeaderTree"
done

# Everything libLLVM and libclang link against, staged beside them. Without this the bundle is only
# self-contained on a machine that already has LLVM's dependencies -- which is any machine with the
# -dev packages, i.e. exactly the machine that builds the bundle and never the one that consumes it.
# The core runtime is deliberately excluded: shipping libstdc++/libgcc/glibc alongside a host
# toolchain is an ABI hazard, and every distribution has them anyway.
CoreRuntime='^(libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s|ld-linux-x86-64)\.so'

for Library in "$STAGE/LLVM/lib"/libclang-*.so.* "$STAGE/LLVM/lib"/libLLVM.so.*; do
    [ -f "$Library" ] || continue
    ldd "$Library" 2>/dev/null | awk '/=>/ {print $1}'
done | sort -u | while read -r SoName; do
    echo "$SoName" | grep -qE "$CoreRuntime" && continue
    case "$SoName" in libclang*|libLLVM*) continue;; esac

    Found="$(ldconfig -p 2>/dev/null | awk -v N="$SoName" '$1==N{print $NF; exit}')"
    Real="$(readlink -f "$Found" 2>/dev/null)"
    if [ -n "$Real" ] && [ -f "$Real" ]; then
        cp -f "$Real" "$STAGE/LLVM/lib/$SoName"
        echo "  dep $SoName"
    else
        echo "  WARNING: $SoName not found on this host; the bundle will need it installed."
    fi
done

"$LlvmRoot/bin/llvm-config" --version > "$STAGE/LLVM/Version.txt" 2>/dev/null \
    || echo "$LLVM_MAJOR" > "$STAGE/LLVM/Version.txt"

# ---------------------------------------------------------------------------------------------
# Slang.
# ---------------------------------------------------------------------------------------------

Note "Slang $SLANG_VERSION"

SlangUrl="https://github.com/shader-slang/slang/releases/download/v$SLANG_VERSION/slang-$SLANG_VERSION-linux-x86_64.tar.gz"

mkdir -p "$WORK/slang" "$STAGE/SLang"
curl -fsSL "$SlangUrl" | tar xz -C "$WORK/slang"

# Slang's tarball is a lib/bin tree; take both wholesale so the versioned .so names and their
# symlinks arrive intact, along with slangc.
cp -a "$WORK/slang/lib" "$STAGE/SLang/lib"
cp -a "$WORK/slang/bin" "$STAGE/SLang/bin"

# ---------------------------------------------------------------------------------------------
# .NET runtime.
#
# RID-scoped, matching DotNetHost's RuntimeRid(), which is why the Windows and Linux bundles can
# share one External/ tree without either overwriting the other.
# ---------------------------------------------------------------------------------------------

Note ".NET $DOTNET_VERSION runtime (linux-x64)"

mkdir -p "$STAGE/DotNet/runtime" "$STAGE/DotNet/include"

curl -fsSL https://dot.net/v1/dotnet-install.sh -o "$WORK/dotnet-install.sh"
chmod +x "$WORK/dotnet-install.sh"
"$WORK/dotnet-install.sh" \
    --version "$DOTNET_VERSION" \
    --runtime dotnet \
    --architecture x64 \
    --install-dir "$STAGE/DotNet/runtime/linux-x64" \
    --no-path

# Hosting headers are platform neutral and already in the Windows bundle; staged again so a
# Linux-only checkout is self-sufficient. They come from the runtime SOURCE tree: the runtime
# package ships no headers, so searching the install found nothing and silently left
# DotNetHost.Build.cs with a missing include path.
HeaderBase="https://raw.githubusercontent.com/dotnet/runtime/v$DOTNET_VERSION/src/native/corehost"
for Header in hostfxr.h coreclr_delegates.h nethost/nethost.h; do
    curl -fsSL "$HeaderBase/$Header" -o "$STAGE/DotNet/include/${Header##*/}" \
        || Fail "could not fetch $Header for .NET $DOTNET_VERSION"
done

# ---------------------------------------------------------------------------------------------
# RenderDoc and Tracy. Both optional: RenderDoc is loaded by name at run time and its absence only
# disables in-app capture, and the Tracy binaries are the profiler UI rather than anything the
# engine links.
# ---------------------------------------------------------------------------------------------

Note "RenderDoc $RENDERDOC_VERSION (optional)"

mkdir -p "$WORK/rd" "$STAGE/RenderDoc"

if curl -fsSL "https://renderdoc.org/stable/$RENDERDOC_VERSION/renderdoc_$RENDERDOC_VERSION.tar.gz" \
    | tar xz -C "$WORK/rd" --strip-components=1 2>/dev/null; then
    find "$WORK/rd" -name 'librenderdoc.so*' -exec cp -a {} "$STAGE/RenderDoc/" \;
else
    echo "  skipped: download failed; in-app GPU capture will be unavailable."
fi

# ---------------------------------------------------------------------------------------------
# Verify the layout before packing.
#
# This is the part worth having. Every path below is one the build resolves by name, and a missing
# or renamed file otherwise surfaces long after setup -- as a link failure, or as a shader compile
# that cannot start. Checking here means the failure lands on the machine that has the files.
# ---------------------------------------------------------------------------------------------

Note "Verifying layout"

Missing=0

Require()
{
    if [ -e "$STAGE/$1" ]; then
        printf '  ok       %s\n' "$1"
    else
        printf '  MISSING  %s   (%s)\n' "$1" "$2"
        Missing=1
    fi
}

Optional()
{
    [ -e "$STAGE/$1" ] && printf '  ok       %s\n' "$1" || printf '  absent   %s   (optional)\n' "$1"
}

Require "LLVM/lib/libclang.so"        "Reflector links -lclang and stages this; declared non-optional"
Require "LLVM/include/clang-c/Index.h" "Reflector's parse headers"
Require "LLVM/include/clang/AST/Decl.h" "Reflector's visitors use the C++ AST API, not just clang-c"
Require "LLVM/include/llvm/Support/Casting.h" "pulled in by the clang C++ headers"
Require "SLang/lib/libslang.so"       "linked as -lslang"
Require "SLang/bin/slangc"            "shader compilation"
Require "DotNet/runtime/linux-x64"    "DotNetHost resolves runtime/<rid> from RuntimeRid()"
Require "DotNet/include/hostfxr.h"    "DotNetHost.Build.cs include path"

# Slang splits the compiler out of the main library, and glslang is the downstream SPIR-V
# optimiser. Optional because their absence degrades rather than breaks: see SLang.Build.cs.
Optional "SLang/lib/libslang-compiler.so"
Optional "SLang/lib/libslang-glslang-$SLANG_VERSION.so"
Optional "SLang/lib/libslang-rt.so"
Optional "RenderDoc/librenderdoc.so"

if [ -n "$(find "$STAGE/DotNet/runtime/linux-x64" -name 'libhostfxr.so' -print -quit 2>/dev/null)" ]; then
    printf '  ok       DotNet/runtime/linux-x64/host/fxr/*/libhostfxr.so\n'
else
    printf '  MISSING  libhostfxr.so under DotNet/runtime/linux-x64   (C# scripting host)\n'
    Missing=1
fi

[ "$Missing" -eq 0 ] || Fail "The bundle is incomplete; not packing it. Fix the entries above."

# ---------------------------------------------------------------------------------------------
# Pack.
# ---------------------------------------------------------------------------------------------

Note "Packing"

mkdir -p "$OUTPUT_DIR"
Bundle="$OUTPUT_DIR/$BUNDLE_NAME"

# Paths are relative to the archive root so it unpacks over the engine root, matching the Windows
# bundle. No --dereference: the symlinks are the point.
tar czf "$Bundle" -C "$WORK" External

Size="$(du -h "$Bundle" | cut -f1)"
Hash="$(sha256sum "$Bundle" | cut -d' ' -f1 | tr '[:lower:]' '[:upper:]')"

Note "Done"
printf '  %s  (%s)\n\n' "$Bundle" "$Size"
printf '  SHA-256: %s\n\n' "$Hash"
printf '  Upload it and pin that hash:\n'
printf '    gh release upload external-deps %s --clobber\n' "$Bundle"
printf '    # SetupMode.Bundles -> the Linux64 entry -> Sha256\n\n'
