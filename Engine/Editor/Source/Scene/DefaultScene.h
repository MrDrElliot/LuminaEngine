#pragma once

#include "Core/Math/Math.h"

namespace Lumina
{
    class CWorld;
}

namespace Lumina::DefaultScene
{
    struct FCameraPose
    {
        FVector3 Location;
        FVector3 Target;
    };

    // Sky, sun, ambient, ground, fog and grading, which is what a newly created level asset starts from.
    void PopulateStarterLevel(CWorld* World);

    // The starter level plus the showcase the editor opens on when the project names no startup map.
    void PopulateWelcomeScene(CWorld* World);

    // Kept beside the scene it frames, so moving the showcase cannot leave the camera aimed at nothing.
    FCameraPose GetWelcomeCameraPose();
}
