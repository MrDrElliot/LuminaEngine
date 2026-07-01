#include "EcsRegistrySnapshotCommand.h"

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
        CWorld* W = World.Get();   // null once the map is closed/swapped -> a stale undo safely no-ops
        if (W == nullptr || In.empty())
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*W);

        // True restore: destroy transactable entities so those absent from the snapshot are removed (this is what makes undo-of-add / redo-of-remove correct); freed slots let the deserialize reclaim exact handles. Keep editor entities and the world-settings singleton alive -- their non-reflected components (the singleton's cached line/triangle batchers) aren't in the snapshot and must not dangle.
        TVector<entt::entity> ToDestroy;
        Registry.view<entt::entity>(entt::exclude<FEditorComponent, FSingletonEntityTag>).each([&](entt::entity E) { ToDestroy.push_back(E); });
        for (entt::entity E : ToDestroy)
        {
            Registry.destroy(E);
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
