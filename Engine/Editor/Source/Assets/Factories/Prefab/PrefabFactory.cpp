#include "EditorPCH.h"
#include "PrefabFactory.h"

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

        // Seed a single root so a brand-new prefab is instantiable immediately (Instantiate
        // refuses an empty registry). Matches the prefab editor's canonical single-root shape.
        entt::entity Root = Prefab->Registry.create();
        Prefab->Registry.emplace<SNameComponent>(Root).Name = FName("Root");
        Prefab->Registry.emplace<STransformComponent>(Root);
        Prefab->Registry.emplace<SPrefabComponent>(Root).StableID = FName(FGuid::New().ToShortString());

        return Prefab;
    }
}
