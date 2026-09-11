#pragma once

#include "DefaultSceneRenderer.h"
#include "World/ECS/Registry.h"
#include <algorithm>
#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/EngineSettings.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Windows/Window.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Renderer/RendererUtils.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RenderManager.h"
#include "Renderer/TextureStreamingManager.h"
#include "TaskSystem/TaskGraph.h"
#include "TaskSystem/TaskSystem.h"
#include "Tools/Import/ImportHelpers.h"
#include "UI/RmlUiBridge.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/BillboardComponent.h"
#include "World/Entity/Components/WidgetComponent.h"
#include "World/Entity/Components/Sprite3DComponent.h"
#include "World/Entity/Components/TextComponent.h"
#include "Tools/FontManager/FontManager.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/ExponentialHeightFogComponent.h"
#include "World/Entity/Components/LocalFogVolumeComponent.h"
#include "World/Entity/Components/LocalFogVolumeComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/LineBatcherComponent.h"
#include "World/Entity/Components/TriangleBatcherComponent.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/DecalComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/ReflectionProbeComponent.h"
#include "World/Entity/Components/SplineComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/GrassComponent.h"
#include "Assets/AssetTypes/Foliage/GrassType.h"
#include "World/Scene/RenderScene/EnvironmentRenderTypes.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "World/Scene/RenderScene/TerrainMeshletBuilder.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"
#include "Renderer/SMAA/AreaTex.h"
#include "Renderer/SMAA/SearchTex.h"
#include "TaskSystem/FiberSync.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    inline constexpr uint32 GFroxelGridX = 160;
    inline constexpr uint32 GFroxelGridY = 90;
    inline constexpr uint32 GFroxelGridZ = 128;

    // Fixed by XeGTAO; the prefilter writes exactly these mips and the main pass clamps its lookup to them.
    inline constexpr uint32 GTAODepthMipLevels = 5;

    // Slices and steps per slice for quality levels low, medium, high and ultra.
    inline constexpr uint32 GGTAOSliceCounts[4]   = { 1, 2, 3, 9 };
    inline constexpr uint32 GGTAOStepsPerSlice[4] = { 2, 2, 3, 3 };

    // A value this large collapses the denoise weights onto the center tap, which is how it is disabled.
    inline constexpr float GGTAODenoiseDisabledBeta = 1e4f;
    inline constexpr float GGTAODenoiseBeta         = 1.2f;




    // One grain for the whole gather, since all primitive types share a single dense array.
    inline constexpr uint32 GPrimitiveGrain = 256;

    // Finer because these are candidates rather than a sparse scan, so every entry works.
    inline constexpr uint32 GSkinnedEmitGrain = 16;

    // A multiple of the 8-wide batch, so a range never splits one mid-way into the scalar tail.
    inline constexpr uint32 GSkinnedCullGrain = 256;

    // Layout does far less per candidate than emit, so it wants wider chunks to stay worth splitting.
    inline constexpr uint32 GSkinnedLayoutGrain = 256;


    // A ceiling reserves nothing, so it scales with the card rather than pinning one number that risks
    // an allocation failure on a small GPU and wastes headroom on a large one.
    inline uint64 AutoPreSkinnedBudgetMiB()
    {
        // Resolved once: a ceiling that drifted with another process's VRAM use would resize the
        // buffer for reasons the scene cannot see.
        static const uint64 Resolved = []
        {
            RHI::FGPUMemoryStats Stats;
            RHI::GetGPUMemoryStats(Stats);

            uint64 DeviceLocalBytes = 0;
            for (const RHI::FGPUMemoryHeapStats& Heap : Stats.Heaps)
            {
                if (Heap.bDeviceLocal)
                {
                    DeviceLocalBytes = Math::Max(DeviceLocalBytes, Heap.BudgetBytes);
                }
            }

            const uint64 MiB = Math::Clamp((DeviceLocalBytes >> 20) * 8ull / 100ull, 64ull, 2048ull);
            LOG_INFO("RenderScene: pre-skinned vertex budget auto-set to {} MiB, 8% of {} MiB device-local VRAM.",
                     MiB, DeviceLocalBytes >> 20);
            return MiB;
        }();

        return Resolved;
    }

    // The whole margin for a demand-fed buffer, both its spike headroom and its reallocation spacing.
    inline constexpr float kSceneBufferGrowth = 1.5f;

    // Bounds VRAM, not correctness, since the claim and store both clamp and losers skin inline.
    inline uint32 GetMaxPreSkinnedVertices()
    {
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        const int32 BudgetMiB = Settings != nullptr ? Settings->PreSkinnedVertexBudgetMiB : 0;

        const uint64 ResolvedMiB = BudgetMiB > 0 ? (uint64)BudgetMiB : AutoPreSkinnedBudgetMiB();
        const uint64 Vertices    = (ResolvedMiB * 1024ull * 1024ull) / sizeof(FPreSkinnedVertex);
        return (uint32)Math::Min<uint64>(Vertices, 0xFFFFFFFFull);
    }

    // Read live from the project settings CDO, like SSR and the froxel grid; there is no per-world copy.
    inline ESMAAMode GetSMAAMode()
    {
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        return Settings != nullptr ? Settings->SMAAMode : ESMAAMode::Off;
    }

    // Quality is only the edge-detection preset now, so a legacy Off here means the default, not disabled.
    inline ESMAAQuality GetSMAAQuality()
    {
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        if (Settings == nullptr || Settings->SMAAQuality == ESMAAQuality::Off)
        {
            return ESMAAQuality::High;
        }
        return Settings->SMAAQuality;
    }

    inline bool IsGTAOEnabled()
    {
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        return Settings != nullptr && Settings->bEnableGTAO;
    }

    // A zero grain would ask for an unbounded task split; floor it.

    struct FScopedGPUMarker
    {
        RHI::FCmdListH CL;
        FScopedGPUMarker(RHI::FCmdListH InCL, const char* Name) : CL(InCL) { RHI::CmdBeginMarker(CL, Name); }
        ~FScopedGPUMarker() { RHI::CmdEndMarker(CL); }
    };
    #define SCENE_MARKER_CONCAT_INNER(A, B) A##B
    #define SCENE_MARKER_CONCAT(A, B) SCENE_MARKER_CONCAT_INNER(A, B)
    #define SCENE_GPU_SCOPE(InCL, Name) FScopedGPUMarker SCENE_MARKER_CONCAT(GpuMarker_, __LINE__)(InCL, Name)
    
    namespace Barriers = RHI::Barriers;

}
