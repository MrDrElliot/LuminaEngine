#include "EntityComponentSnapshotCommand.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace
    {
        // Marks an entity that did not carry the component when the image was taken.
        constexpr int32 AbsentComponent = -1;
    }

    bool FEntityComponentSnapshotCommand::CanSnapshotComponent(const CStruct* ComponentType)
    {
        return ComponentType != nullptr && FindComponentOps(ComponentType->GetName().c_str()) != nullptr;
    }

    FEntityComponentSnapshotCommand::FEntityComponentSnapshotCommand(CWorld* InWorld, TVector<entt::entity> InEntities, CStruct* InComponentType)
        : World(InWorld)
        , Entities(Move(InEntities))
        , ComponentType(InComponentType)
    {
        if (ComponentType != nullptr)
        {
            Ops = FindComponentOps(ComponentType->GetName().c_str());
        }

        Capture(Before);
    }

    void FEntityComponentSnapshotCommand::Finalize()
    {
        Capture(After);
    }

    void FEntityComponentSnapshotCommand::Capture(TVector<uint8>& Out) const
    {
        LUMINA_PROFILE_SCOPE();

        Out.clear();

        CWorld* W = World.Get();
        if (W == nullptr || ComponentType == nullptr || Ops == nullptr)
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        FMemoryWriter Writer(Out);
        FObjectProxyArchiver Ar(Writer, false);

        for (entt::entity Entity : Entities)
        {
            const bool bPresent = Registry.valid(Entity) && Ops->Has(Registry, Entity) != 0;

            int32 Size = AbsentComponent;
            const int64 SizePosition = Ar.Tell();
            Ar << Size;

            if (!bPresent)
            {
                continue;
            }

            // Null for a tag, whose presence still has to survive the round trip even with no data.
            void* Component = Ops->Get(Registry, Entity);

            const int64 DataStart = Ar.Tell();
            if (Component != nullptr)
            {
                ComponentType->SerializeTaggedProperties(Ar, Component);
            }
            const int64 DataEnd = Ar.Tell();

            Size = (int32)(DataEnd - DataStart);
            Ar.Seek(SizePosition);
            Ar << Size;
            Ar.Seek(DataEnd);
        }
    }

    void FEntityComponentSnapshotCommand::Restore(const TVector<uint8>& In) const
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* W = World.Get();   // null once the map is closed/swapped -> a stale undo safely no-ops
        if (W == nullptr || ComponentType == nullptr || Ops == nullptr || In.empty())
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        FMemoryReader Reader(In);
        FObjectProxyArchiver Ar(Reader, true);

        // Same reason FEntityTransformCommand re-tags, a restored local transform needs a resolve.
        const bool bTransformRestored = (ComponentType == STransformComponent::StaticStruct());

        for (entt::entity Entity : Entities)
        {
            int32 Size = AbsentComponent;
            Ar << Size;

            const int64 DataStart = Ar.Tell();
            const bool bValid = Registry.valid(Entity);

            if (Size < 0)
            {
                // Absent when the image was taken, so a component added inside the transaction comes off.
                if (bValid && Ops->Has(Registry, Entity) != 0)
                {
                    Ops->Remove(Registry, Entity);
                }
                continue;
            }

            if (bValid)
            {
                // Get-or-emplace, so one removed inside the transaction is back before it gets filled.
                void* Component = Ops->Emplace(Registry, Entity);
                if (Component != nullptr)
                {
                    ComponentType->SerializeTaggedProperties(Ar, Component);

                    // The write above is a raw store, so the registry signal has to be raised by hand.
                    Ops->Patch(Registry, Entity);

                    if (bTransformRestored)
                    {
                        Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
                    }

                    // An undo back to the prefab value leaves zero divergent leaves, clearing the override.
                    CPrefab::RecaptureComponentOverrides(Registry, Entity, ComponentType);
                }
            }

            // Unconditional, so a component that went away since the capture skips its bytes cleanly.
            Ar.Seek(DataStart + Size);
        }

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }

    void FEntityComponentSnapshotCommand::Undo() { Restore(Before); }
    void FEntityComponentSnapshotCommand::Redo() { Restore(After); }
}
