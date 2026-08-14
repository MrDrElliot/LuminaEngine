#!/usr/bin/env bash
#
# Front end for LuminaBuildTool, and the Unix counterpart to LuminaBuild.bat. Builds the tool if
# needed, then forwards every argument to it.
#
#   ./LuminaBuild.sh Build Reflector -TargetType=Program
#   ./LuminaBuild.sh Build Lumina -TargetType=Editor
#
# LUMINA_SKIP_TOOL_BUILD=1 skips rebuilding the tool itself.

set -uo pipefail

LuminaRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BuildToolProject="$LuminaRoot/Engine/Tools/LuminaBuildTool/LuminaBuildTool.csproj"
BuildToolDll="$LuminaRoot/Binaries/DotNet/BuildTool/LuminaBuildTool.dll"

if ! command -v dotnet >/dev/null 2>&1; then
    echo "error: the .NET SDK is required to run LuminaBuildTool. Install .NET 10 or newer." >&2
    exit 1
fi

# Reached when this script is not sitting in an engine root, which is what happens when it is copied
# somewhere on its own or LUMINA_DIR points at the wrong tree. Checked here so the failure names the
# tree it looked in, rather than surfacing as an MSBuild error about a missing project.
if [ ! -f "$BuildToolProject" ]; then
    echo "error: LuminaBuildTool source is missing at \"$BuildToolProject\"." >&2
    echo "\"$LuminaRoot\" does not look like a Lumina engine root." >&2
    exit 1
fi

# Always rebuild: the no-op case is well under a second, and a stale tool silently produces stale
# rules behaviour that is very hard to diagnose.
if [ -z "${LUMINA_SKIP_TOOL_BUILD:-}" ]; then
    if ! dotnet build "$BuildToolProject" -v quiet --nologo; then
        exit 1
    fi
fi

# Two different failures, and saying "did not produce" for the second one sends the reader off
# looking at a build that never ran.
if [ ! -f "$BuildToolDll" ]; then
    if [ -n "${LUMINA_SKIP_TOOL_BUILD:-}" ]; then
        echo "error: LuminaBuildTool is missing at \"$BuildToolDll\" and LUMINA_SKIP_TOOL_BUILD is set." >&2
        echo "Clear LUMINA_SKIP_TOOL_BUILD and run this script again to build it." >&2
    else
        echo "error: LuminaBuildTool did not produce \"$BuildToolDll\"." >&2
    fi
    exit 1
fi

exec dotnet "$BuildToolDll" "$@" -EngineRoot="$LuminaRoot"
