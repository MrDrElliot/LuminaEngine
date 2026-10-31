#include "EntityComponentSnapshotCommand.h"
#include "World/ECS/Registry.h"

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

    FEntityComponentSnapshotCommand::FEntityComponentSnapshotCommand(CWorld* InWorld, TVector<ECS::FEntity> InEntities, CStruct* InComponentType)
        : World(InWorld)
        , Entities(Move(InEntities))
        , ComponentType(InComponentType)
    {
        if (ComponentType != nullptr)
        {
            Ops = FindComponentOps(ComponentType->GetName().c_str());
        }

        Capture(Before, OverridesBefore);

        PresentBefore.assign(Entities.size(), 0u);
        if (CWorld* W = World.Get(); W != nullptr && Ops != nullptr)
        {
            ECS::FRegistry& Registry = W->GetEntityRegistry();
            for (SIZE_T i = 0; i < Entities.size(); ++i)
            {
                PresentBefore[i] = Registry.IsValid(Entities[i]) && Ops->Has(Registry, Entities[i]) != 0;
            }
        }
    }

    CStruct* FEntityComponentSnapshotCommand::ResolveType() const
    {
        if (Ops != nullptr && Ops->GetTypeID() != ECS::InvalidComponentTypeID)
        {
            if (CStruct* Current = ECS::FComponentTypeRegistry::Get().GetInfo(Ops->GetTypeID()).GetStruct())
            {
                return Current;
            }
        }
        return ComponentType;
    }

    void FEntityComponentSnapshotCommand::Finalize()
    {
        TVector<TVector<FName>> Unused;
        Capture(After, Unused);

        CWorld* W = World.Get();
        CStruct* Type = ResolveType();
        if (W == nullptr || Ops == nullptr || Type == nullptr)
        {
            return;
        }

        // Every component edit records its prefab override here, so no caller can forget and lose it on the next refresh.
        ECS::FRegistry& Registry = W->GetEntityRegistry();
        for (SIZE_T i = 0; i < Entities.size(); ++i)
        {
            const ECS::FEntity Entity = Entities[i];
            if (!Registry.IsValid(Entity))
            {
                continue;
            }

            const bool bPresentNow = Ops->Has(Registry, Entity) != 0;
            if (PresentBefore[i] && !bPresentNow)
            {
                CPrefab::NoteComponentRemoved(Registry, Entity, Type);
            }
            else if (!PresentBefore[i] && bPresentNow)
            {
                CPrefab::NoteComponentAdded(Registry, Entity, Type);
            }

            if (bPresentNow)
            {
                CPrefab::RecaptureComponentOverrides(Registry, Entity, Type);
            }
        }

        OverridesAfter.clear();
        for (ECS::FEntity Entity : Entities)
        {
            OverridesAfter.push_back(CPrefab::GetComponentOverrides(Registry, Entity, Type));
        }
    }

    void FEntityComponentSnapshotCommand::Capture(TVector<uint8>& Out, TVector<TVector<FName>>& OutOverrides) const
    {
        LUMINA_PROFILE_SCOPE();

        Out.clear();
        OutOverrides.clear();

        CWorld* W = World.Get();
        CStruct* Type = ResolveType();
        if (W == nullptr || Type == nullptr || Ops == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        FMemoryWriter Writer(Out);
        FObjectProxyArchiver Ar(Writer, false);

        for (ECS::FEntity Entity : Entities)
        {
            OutOverrides.push_back(Registry.IsValid(Entity) ? CPrefab::GetComponentOverrides(Registry, Entity, Type) : TVector<FName>());
            const bool bPresent = Registry.IsValid(Entity) && Ops->Has(Registry, Entity) != 0;

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
                Type->SerializeProperties(Ar, Component);
            }
            const int64 DataEnd = Ar.Tell();

            Size = (int32)(DataEnd - DataStart);
            Ar.Seek(SizePosition);
            Ar << Size;
            Ar.Seek(DataEnd);
        }
    }

    void FEntityComponentSnapshotCommand::Restore(const TVector<uint8>& In, const TVector<TVector<FName>>& Overrides) const
    {
        LUMINA_PROFILE_SCOPE();

        // Null once the map is closed or swapped, so a stale undo no-ops.
        CWorld* W = World.Get();
        CStruct* Type = ResolveType();
        if (W == nullptr || Type == nullptr || Ops == nullptr || In.empty())
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();

        FMemoryReader Reader(In);
        FObjectProxyArchiver Ar(Reader, true);

        // Same reason FEntityTransformCommand re-tags, a restored local transform needs a resolve.
        const bool bTransformRestored = (Type == STransformComponent::StaticStruct());

        for (SIZE_T Index = 0; Index < Entities.size(); ++Index)
        {
            const ECS::FEntity Entity = Entities[Index];
            int32 Size = AbsentComponent;
            Ar << Size;

            const int64 DataStart = Ar.Tell();
            const bool bValid = Registry.IsValid(Entity);

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
                    Type->SerializeProperties(Ar, Component);

                    // The write above is a raw store, so the registry signal has to be raised by hand.
                    Ops->Patch(Registry, Entity);

                    if (bTransformRestored)
                    {
                        Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
                    }

                    if (Index < Overrides.size())
                    {
                        CPrefab::RestoreComponentOverrides(Registry, Entity, Type, Overrides[Index]);
                    }
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

    void FEntityComponentSnapshotCommand::Undo() { Restore(Before, OverridesBefore); }
    void FEntityComponentSnapshotCommand::Redo() { Restore(After, OverridesAfter); }
}
