#pragma once

#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"
#include "World/ECS/Entity.h"
#include "World/Entity/Systems/SystemResources.h"

#include <atomic>

namespace Lumina
{
    enum class EPrimitiveSource : uint8
    {
        StaticMesh,
        DynamicMesh,
        SkeletalMesh,
        Foliage,
        Num,

        AnySource = 0xFF,
    };

    enum class EPrimitiveDirty : uint32
    {
        None                = 0,
        // World matrix moved: refresh the transform and the world cull sphere. Nothing else.
        Transform           = 1u << 0,
        Data                = 1u << 1,
        // SDisabledTag added or removed.
        Visibility          = 1u << 2,
        // Component or entity added or destroyed.
        Membership          = 1u << 3,

        All                 = Transform | Data | Visibility | Membership,
    };
    ENUM_CLASS_FLAGS(EPrimitiveDirty);

    // Every render scene drawing a registry registers a consumer, and each mark fans out to all of them, so two renderers on one world never steal each other's entries.
    class RUNTIME_API FRenderDirtyTracker : public ECS::FResource
    {
    public:

        struct FEntry
        {
            ECS::FEntity        Entity;
            EPrimitiveSource    Source;
            EPrimitiveDirty     Flags;
        };

        // One painted foliage instance hidden or shown at runtime.
        struct FInstanceEdit
        {
            ECS::FEntity    Entity;
            uint32          Instance;
        };

        static constexpr uint32 kMaxConsumers = 8;

        // Creates the tracker in the registry context on first call. Game thread.
        static FRenderDirtyTracker& Ensure(ECS::FRegistry& Registry);
        static FRenderDirtyTracker* Find(ECS::FRegistry& Registry);

        FRenderDirtyTracker();
        ~FRenderDirtyTracker();
        LE_NO_COPYMOVE(FRenderDirtyTracker);

        // Game thread. The new consumer hears every mark from here on and starts with a full rescan; kIndexNoneU32 when all are taken.
        uint32 AddConsumer();
        void RemoveConsumer(uint32 Consumer);

        // Any thread, so a system marks through a TRes. A mark with no consumer registered is dropped, since a consumer added later rescans everything.
        void Mark(ECS::FEntity Entity, EPrimitiveSource Source, EPrimitiveDirty Flags) const;
        void MarkAllSources(ECS::FEntity Entity, EPrimitiveDirty Flags) const;
        void MarkInstanceVisibility(ECS::FEntity Entity, uint32 Instance) const;

        // One relaxed load on a frame where nothing changed. An unknown consumer always has work, which is a full rescan.
        bool HasPending(uint32 Consumer) const;

        // Game thread. Append to Out and return true if anything was drained.
        bool Drain(uint32 Consumer, TVector<FEntry>& Out);
        bool DrainInstanceEdits(uint32 Consumer, TVector<FInstanceEdit>& Out);

        // Every consumer, or one.
        void RequestFullRescan();
        void RequestFullRescan(uint32 Consumer);
        bool ConsumeFullRescan(uint32 Consumer);

    private:

        struct FImpl;
        FImpl*              Impl = nullptr;
        std::atomic<uint32> ConsumerMask{ 0 };
    };
}
