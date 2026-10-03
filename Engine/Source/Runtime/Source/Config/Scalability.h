#pragma once

#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "Scalability.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EQualityLevel : uint8
    {
        // Untouched by the player, so the project's own renderer configuration applies.
        Default,
        Low,
        Medium,
        High,
        Ultra,

        // Only ever reported, for an overall level whose groups disagree.
        Custom,
    };

    REFLECT()
    enum class EScalabilityGroup : uint8
    {
        ViewDistance,
        Shadows,
        Effects,
        PostProcess,
        Textures,
        AntiAliasing,
    };

    // A project replaces any group of the engine's table by naming it in /Config/Scalability.json.
    namespace Scalability
    {
        inline constexpr int32 NumGroups = 6;

        RUNTIME_API void ApplyGroup(EScalabilityGroup Group, EQualityLevel Level);

        // Puts back every setting a group touched, which in the editor ends a play session's changes.
        RUNTIME_API void RestoreProjectValues();

        RUNTIME_API FString GetGroupName(EScalabilityGroup Group);
    }
}
