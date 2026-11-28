#pragma once

#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Core/Threading/Mutex.h"
#include "Platform/GenericPlatform.h"
#include "World/ECS/Entity.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina
{
    namespace ECS
    {
        class FRegistry;
    }

    struct SPointLightComponent;
    struct SSpotLightComponent;
    struct SAreaLightComponent;
    struct STransformComponent;

    enum class ESceneLightKind : uint8
    {
        Point,
        Spot,
        Area,
        Num,
    };

    // Every local light of a world in a stable slot, so the GPU keeps the whole set and a frame uploads only what changed.
    class RUNTIME_API FSceneLightSet
    {
    public:

        // Must match FLightUpload in LightUpload.slang.
        struct FUpload
        {
            FLight Light;
            uint32 Slot   = 0;
            uint32 Layers = 0;
            uint32 _Pad0  = 0;
            uint32 _Pad1  = 0;
        };

        // A light whose shadow or light function the CPU resolves each frame, which every other light never needs.
        struct FCPULight
        {
            ECS::FEntity    Entity;
            ESceneLightKind Kind = ESceneLightKind::Point;
            uint32          Slot = Constants::kIndexNoneU32;
        };

        FSceneLightSet() = default;
        ~FSceneLightSet();
        LE_NO_COPYMOVE(FSceneLightSet);

        // Hooks the registry's light signals and its moved transforms, and starts with a full rescan.
        void Attach(ECS::FRegistry& InRegistry);
        void Detach();

        // Game thread. Folds this frame's adds, removes, edits and moves into the slots and their pending uploads.
        void Sync();

        // A CPU light's record for this frame, with its shadow slot and light function folded in.
        void Patch(uint32 Slot, const FLight& Light);

        // Appends the records that differ from what the GPU last received.
        void TakeUploads(TVector<FUpload>& Out);

        NODISCARD uint32 GetCapacity() const { return Capacity; }
        NODISCARD uint32 GetNumLights() const { return NumLive; }
        NODISCARD TSpan<const FCPULight> GetCPULights() const { return TSpan<const FCPULight>(CPULights.GetData(), CPULights.Num()); }
        NODISCARD bool HasVolumetricLights() const { return NumVolumetric != 0; }
        NODISCARD bool HasContactShadowLights() const { return NumContact != 0; }

        // A switched-off light keeps its slot at zero radius, which every view's cull skips.
        static FLight MakePointLight(const SPointLightComponent& Light, const STransformComponent& Transform);
        static FLight MakeSpotLight(const SSpotLightComponent& Light, const STransformComponent& Transform);
        static FLight MakeAreaLight(const SAreaLightComponent& Light, const STransformComponent& Transform);

    private:

        struct FSlot
        {
            ECS::FEntity    Entity;
            ESceneLightKind Kind      = ESceneLightKind::Point;
            uint32          CPUIndex  = Constants::kIndexNoneU32;
            bool            bLive     = false;
            bool            bDirty    = false;
        };

        void OnMembershipChanged(ECS::FRegistry& InRegistry, ECS::FEntity Entity);
        void RescanAll();
        void ScanEdits(ESceneLightKind Kind, uint32 Since);
        void Refresh(ECS::FEntity Entity, ESceneLightKind Kind);
        void Rebuild(uint32 Slot);
        void Release(uint32 Slot);
        uint32 Allocate(ECS::FEntity Entity, ESceneLightKind Kind);
        void SetCPU(uint32 Slot, bool bCPU);
        void MarkDirty(uint32 Slot);
        void CountFlags(const FLight& Light, int32 Sign);

        ECS::FRegistry*              Registry = nullptr;
        TVector<FLight>              Records;
        TVector<uint32>              Layers;
        // What each slot last uploaded, so a caster rebuilt every frame goes up only when its record really changed.
        TVector<FUpload>             Sent;
        TVector<FSlot>               Slots;
        TVector<uint32>              FreeSlots;
        THashMap<uint64, uint32>     SlotOf;
        TVector<uint32>              Dirty;
        TVector<FCPULight>           CPULights;
        // Signals can fire off the game thread, so they only queue the entity for the next sync.
        TMutex<TVector<ECS::FEntity>> Pending;
        TVector<ECS::FEntity>        PendingScratch;
        TVector<ECS::FEntity>        MovedScratch;
        uint64                       Epochs[(uint32)ESceneLightKind::Num] = {};
        uint32                       LastTick       = 0;
        uint32                       MovedConsumer  = Constants::kIndexNoneU32;
        uint32                       Capacity       = 0;
        uint32                       NumLive        = 0;
        uint32                       NumVolumetric  = 0;
        uint32                       NumContact     = 0;
        bool                         bRescan        = true;
        bool                         bResendAll     = false;
    };
}
