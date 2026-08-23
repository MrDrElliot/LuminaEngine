#pragma once

#include "Containers/String.h"

namespace Lumina
{
    struct FStandaloneLaunchOptions
    {
        // Virtual path of the map to open. Empty runs the project's configured startup map.
        FString MapPath;

        // The game process loads the project's Game-target binaries, which the editor's own are not.
        bool bBuildIfMissing = true;
    };

    /** Runs the loaded project as a separate game process against the uncooked project tree. */
    class EDITOR_API FStandaloneLauncher
    {
    public:

        static void Request(const FStandaloneLaunchOptions& Options);

        /** Drives a pending build to its launch. Call once per frame from the editor. */
        static void Tick();

        NODISCARD static bool IsBuilding();

        /** Whether the project's Game-target binaries are on disk, so the UI can say what a click will do. */
        NODISCARD static bool HasGameBinaries();

        /** The build that produces them, for a message that tells the user exactly what to run. */
        NODISCARD static FString GetBuildCommandLine();
    };
}
