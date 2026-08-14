#!/usr/bin/env bash
#
# Regenerates this project's IDE files, and the Unix counterpart to GenerateProject.bat. The engine
# is built from source alongside the project, so its own output stays in the engine tree and is
# shared across projects.

set -uo pipefail

ProjectDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "${LUMINA_DIR:-}" ]; then
    echo "LUMINA_DIR is not set. Run the engine's Setup.sh first, or export it yourself:" >&2
    echo "  export LUMINA_DIR=\"/path/to/LuminaEngine\"" >&2
    exit 1
fi

if [ ! -f "$LUMINA_DIR/LuminaBuild.sh" ]; then
    echo "LuminaBuild.sh not found under \"$LUMINA_DIR\"." >&2
    echo "LUMINA_DIR does not point at a Lumina engine root." >&2
    exit 1
fi

# Extra arguments pass straight through.
if ! "$LUMINA_DIR/LuminaBuild.sh" GenerateProjectFiles -Project="$ProjectDir" "$@"; then
    echo
    echo "Project generation failed." >&2
    exit 1
fi
