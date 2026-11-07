#pragma once

#include "Containers/String.h"
#include "Core/Threading/Thread.h"

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

        FStandaloneLauncher() = default;
        ~FStandaloneLauncher() { Shutdown(); }
        LE_NO_COPYMOVE(FStandaloneLauncher);

        void Request(const FStandaloneLaunchOptions& Options);

        /** Drives a pending build to its launch. Call once per frame from the editor. */
        void Tick();

        // Kills a build in flight and joins its worker, so nothing it started outlives the editor.
        void Shutdown();

        NODISCARD bool IsBuilding() const { return Worker.Joinable(); }

        /** Whether the project's Game-target binaries are on disk, so the UI can say what a click will do. */
        NODISCARD static bool HasGameBinaries();

        /** The build that produces them, for a message that tells the user exactly what to run. */
        NODISCARD static FString GetBuildCommandLine();

    private:

        FThread                     Worker;
        FStandaloneLaunchOptions    PendingOptions;
        std::atomic<bool>           bBuildDone{false};
        int32                       BuildExitCode = 0;
    };
}
