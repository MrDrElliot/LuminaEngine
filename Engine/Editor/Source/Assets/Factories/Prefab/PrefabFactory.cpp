#include "EditorPCH.h"
#include "PrefabFactory.h"
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "GUID/GUID.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    CClass* CPrefabFactory::GetAssetClass() const
    {
        return CPrefab::StaticClass();
    }

    CObject* CPrefabFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CPrefab* Prefab = NewObject<CPrefab>(Package, Name);

        // Seeded with one root so a new prefab is instantiable, since Instantiate refuses an empty registry.
        ECS::FEntity Root = Prefab->Registry.Create();
        Prefab->Registry.Emplace<SNameComponent>(Root).Name = FName("Root");
        Prefab->Registry.Emplace<STransformComponent>(Root);
        Prefab->Registry.Emplace<SPrefabComponent>(Root).StableID = FName(FGuid::New().ToShortString());

        return Prefab;
    }
}
