#include "EcsRegistrySnapshotCommand.h"
#include "World/ECS/Registry.h"

#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/SingletonEntityComponent.h"

namespace Lumina
{
    FEcsRegistrySnapshotCommand::FEcsRegistrySnapshotCommand(CWorld* InWorld)
        : World(InWorld)
    {
        Capture(Before);
    }

    void FEcsRegistrySnapshotCommand::Finalize()
    {
        Capture(After);
    }

    void FEcsRegistrySnapshotCommand::Capture(TVector<uint8>& Out) const
    {
        // It used to run inline with no zone, so a long gizmo-grab stall showed as unattributed self time.
        LUMINA_PROFILE_SCOPE();

        Out.clear();
        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        FMemoryWriter Writer(Out);
        FObjectProxyArchiver Ar(Writer, false);
        ECS::Utils::SerializeRegistry(Ar, ECS::GetWorldRegistry(*W));
    }

    void FEcsRegistrySnapshotCommand::Restore(const TVector<uint8>& In) const
    {
        LUMINA_PROFILE_SCOPE();

        CWorld* W = World.Get();   // null once the map is closed/swapped -> a stale undo safely no-ops
        if (W == nullptr || In.empty())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*W);

        // Editor entities and the settings singleton stay, since their unreflected components would dangle.
        TVector<ECS::FEntity> ToDestroy;
        Registry.ForEachEntityExcept<FEditorComponent, FSingletonEntityTag>([&](ECS::FEntity E) { ToDestroy.push_back(E); });
        for (ECS::FEntity E : ToDestroy)
        {
            Registry.Destroy(E);
        }

        FMemoryReader Reader(In);
        FObjectProxyArchiver Ar(Reader, true);
        ECS::Utils::SerializeRegistry(Ar, Registry);

        if (W->GetPackage())
        {
            W->GetPackage()->MarkDirty();
        }
    }

    void FEcsRegistrySnapshotCommand::Undo() { Restore(Before); }
    void FEcsRegistrySnapshotCommand::Redo() { Restore(After); }
}
