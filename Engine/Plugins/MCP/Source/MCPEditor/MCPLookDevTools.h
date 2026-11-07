#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPSceneTools.h"
#include "World/ECS/Entity.h"

#include "MCPLookDevTools.generated.h"

namespace Lumina
{
    class CMaterial;

    REFLECT()
    struct MCPEDITOR_API SLookDevBuildParams
    {
        GENERATED_BODY()

        // Degrees above the horizon, above 0 and at most 90.
        PROPERTY()
        float SunElevationDegrees = 45.0f;

        // Bearing the sunlight comes from, in degrees clockwise from +Z toward +X.
        PROPERTY()
        float SunAzimuthDegrees = 120.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SLookDevBuildResult
    {
        GENERATED_BODY()

        // Every fixture entity, named by what it is for.
        PROPERTY()
        TVector<SEntityInfo> Entities;

        // Whether the terrain patch carries GPU grass; the text reply says why when it does not.
        PROPERTY()
        bool bGrass = false;
    };

    REFLECT()
    struct MCPEDITOR_API SLookDevCaptureParams
    {
        GENERATED_BODY()

        // Folder under Saved/LumiShots, such as the task id.
        PROPERTY()
        FString Label;

        // ContactClose, Overhang, GrassGrazing or FarVista, in capture order. Empty takes all four.
        PROPERTY()
        TVector<FString> Presets;

        // Frames rendered after each camera move before the first capture, so temporal passes settle.
        PROPERTY()
        int32 WarmupFrames = 30;

        // Consecutive frames written per preset, as <Preset>_<n>.png from 0.
        PROPERTY()
        int32 FrameCount = 3;
    };

    REFLECT()
    struct MCPEDITOR_API SLookDevCaptureResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<FString> Paths;
    };

    namespace MCP
    {
        // Owned by FMCPEditorModule, so a rebuild or lookdev.clear finds what the last build made.
        struct FLookDevFixtureState
        {
            TWeakObjectPtr<CWorld> World;
            TVector<ECS::FEntity> Entities;

            // Suns that were lit before the build, turned off so the renderer picks the fixture's own.
            TVector<ECS::FEntity> DisabledSuns;

            // Compiled once and kept across rebuilds, since a builtin material's name can be taken only once.
            TStrongObjectPtr<CMaterial> MippedLeaves;
            TStrongObjectPtr<CMaterial> CoverageLeaves;
        };

        MCPEDITOR_API void RegisterLookDevTools(FStringView Owner);
    }
}
