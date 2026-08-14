#!/usr/bin/env bash
#
# Regenerate IDE project files after adding or removing modules, plugins or sources, and the Unix
# counterpart to GenerateProjectFiles.bat. First-time setup is Setup.sh. Source files themselves
# are discovered at build time, so this is only needed to refresh what the editor sees.
#
# On this platform the artefact is compile_commands.json at the repository root, which is what
# clangd, VS Code and Rider read. No solution is generated: nothing here can open one.
#
#   ./GenerateProjectFiles.sh
#   ./GenerateProjectFiles.sh -Tracy=off      persistent defaults live in Engine/Build/BuildConfiguration.json

set -uo pipefail

LuminaRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LUMINA_DIR="$LuminaRoot"

# dotnet-install.sh drops .NET in $HOME/.dotnet and persists nothing, so a new shell loses it and
# every script here fails claiming the SDK is missing when it is sitting right there.
if ! command -v dotnet >/dev/null 2>&1 && [ -x "$HOME/.dotnet/dotnet" ]; then
    export PATH="$HOME/.dotnet:$PATH"
fi

if ! command -v dotnet >/dev/null 2>&1; then
    echo "error: the .NET SDK is required to run LuminaBuildTool. Install .NET 10 or newer." >&2
    exit 1
fi

# Extra arguments pass straight through.
# The tool reports what it wrote and where, so there is nothing to add here on success.
if ! "$LuminaRoot/LuminaBuild.sh" GenerateProjectFiles "$@"; then
    echo
    echo "Project generation failed." >&2
    exit 1
fi
