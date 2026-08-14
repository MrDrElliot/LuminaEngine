#include "EditorPCH.h"
#include "EditorAssetActions.h"

#include "EditorToolContext.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Physics/CollisionShapeGen.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    FAssetActionRegistry& FAssetActionRegistry::Get()
    {
        static FAssetActionRegistry Instance;
        return Instance;
    }

    void FAssetActionRegistry::RegisterAction(CClass* AssetClass, FAssetAction Action)
    {
        if (AssetClass == nullptr || Action.Execute == nullptr)
        {
            return;
        }

        Actions[AssetClass].push_back(Move(Action));
    }

    void FAssetActionRegistry::GatherActions(CClass* AssetClass, TVector<const FAssetAction*>& Out) const
    {
        // Walk base-ward from the concrete class, so a CMaterial action lists above one registered on
        // CMaterialInterface. Accumulates rather than stopping at the first match -- an asset can
        // legitimately have actions contributed at several levels of its hierarchy.
        for (CClass* Class = AssetClass; Class != nullptr; Class = Class->GetSuperClass())
        {
            const auto Itr = Actions.find(Class);
            if (Itr == Actions.end())
            {
                continue;
            }

            for (const FAssetAction& Action : Itr->second)
            {
                Out.push_back(&Action);
            }
        }
    }

    FFixedString MakeSiblingAssetPath(FStringView SourceVirtualPath, const char* Suffix)
    {
        const FStringView NoExt  = VFS::RemoveExtension(SourceVirtualPath);
        const FStringView Parent = VFS::Parent(NoExt);
        const FStringView Base   = VFS::FileName(NoExt, true);

        FFixedString Name(Base.data(), Base.size());
        Name.append_convert(Suffix);

        FFixedString Path = Paths::Combine(Parent, Name);
        CPackage::AddPackageExt(Path);
        return VFS::MakeUniqueFilePath(Path);
    }

    CObject* DuplicateAssetPackage(CObject* PrimaryAsset, FStringView DestPath)
    {
        CPackage* SourcePackage = PrimaryAsset != nullptr ? PrimaryAsset->GetPackage() : nullptr;
        if (SourcePackage == nullptr)
        {
            return nullptr;
        }

        // Every export has to be RESIDENT before it can be copied. Opening a material only brings in the
        // material itself -- its node graph is a lazily-loaded sibling export, so a copy made without
        // this found one object, and the duplicate opened with an empty graph. FullyLoad is what the
        // package saver itself uses for exactly this reason; pulling exports in by hand missed the ones
        // whose weak pointer was never populated.
        if (!SourcePackage->FullyLoad())
        {
            return nullptr;
        }

        TVector<CObject*> SourceObjects;
        SourceObjects.reserve(SourcePackage->ExportTable.size());
        for (const FObjectExport& Export : SourcePackage->ExportTable)
        {
            if (CObject* Object = Export.Object.Get())
            {
                SourceObjects.push_back(Object);
            }
        }

        if (SourceObjects.empty())
        {
            return nullptr;
        }

        CPackage* DestPackage = CPackage::CreatePackage(DestPath);
        if (DestPackage == nullptr)
        {
            return nullptr;
        }

        // The PRIMARY object is identified by having the same name as its package file -- that is how
        // the factory creates assets and how loading finds them again. Carrying the source's name over
        // produces a "NewMaterial" inside NewMaterial_Copy.lasset, which nothing can resolve: the asset
        // shows in the browser but cannot be opened or deleted. Sub-objects are the opposite case --
        // they are found by name (LoadObjectByName), so renaming THEM would break the copy's wiring.
        const FStringView DestAssetName = VFS::FileName(DestPath, /*bRemoveExtension*/ true);

        // Pass 1: construct every copy first, so pass 2 can resolve a reference to ANY sibling,
        // including one later in the table.
        THashMap<CObject*, CObject*> Remap;
        Remap.reserve(SourceObjects.size());

        for (CObject* Source : SourceObjects)
        {
            const FName CopyName = (Source == PrimaryAsset) ? FName(DestAssetName) : Source->GetName();

            CObject* Copy = NewObject(Source->GetClass(), DestPackage, CopyName, FGuid::New(), OF_Public);
            if (Copy == nullptr)
            {
                return nullptr;
            }

            // No ExportTable bookkeeping: the saver clears and rebuilds it from what the package
            // actually owns, so being outered to DestPackage is what makes a copy an export.
            Remap.emplace(Source, Copy);
        }

        // Pass 2: round-trip each object through its own Serialize, so the copy gets everything the asset
        // actually owns. Copying reflected properties alone loses state a class serializes by hand -- a
        // mesh's FMeshResource, an anim graph's bytecode -- and the copy then dereferences a resource that
        // was never created. Remapping happens on the READ, once each GUID has resolved to an object.
        for (CObject* Source : SourceObjects)
        {
            CObject* Copy = Remap[Source];

            TVector<uint8> Bytes;
            {
                FMemoryWriter Writer(Bytes);
                FObjectProxyArchiver Proxy(Writer, true);
                Source->Serialize(Proxy);
            }
            {
                FMemoryReader Reader(Bytes);
                FObjectRemapArchiver Proxy(Reader, Remap);
                Copy->Serialize(Proxy);
            }

            // Serialize has no concept of DuplicateTransient, so anything marked that way arrives copied
            // and has to be put back to its default. Node graphs rely on this to drop per-instance editor
            // state that must not survive a duplicate.
            if (CClass* Class = Source->GetClass())
            {
                const CObject* Defaults = Class->GetDefaultObject<CObject>();
                if (Defaults != nullptr)
                {
                    for (FProperty* Current = Class->LinkedProperty; Current; Current = (FProperty*)Current->Next)
                    {
                        if (Current->HasMetadata("DuplicateTransient"))
                        {
                            Current->CopyCompleteValue_InContainer(Copy, Defaults);
                        }
                    }
                }
            }
        }

        // PostLoad after everything is wired, not per object: a PostLoad that reaches for a sibling
        // (the material graph rebuilding its node links) needs the whole set present and remapped.
        for (CObject* Source : SourceObjects)
        {
            Remap[Source]->PostLoad();
        }

        if (!CPackage::SavePackage(DestPackage, DestPath))
        {
            return nullptr;
        }

        return Remap[PrimaryAsset];
    }

    namespace
    {
        void CreateMaterialInstanceFrom(const FAssetActionContext& Context)
        {
            // Interface, not CMaterial: instancing an instance just parents to it and inherits everything.
            CMaterialInterface* Parent = Cast<CMaterialInterface>(LoadObject<CObject>(Context.Asset->AssetGUID));
            if (Parent == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Could not load '{0}' to instance it.", Context.Asset->Path);
                return;
            }

            const FFixedString NewPath = MakeSiblingAssetPath(
                FStringView(Context.Asset->Path.c_str(), Context.Asset->Path.size()), "_Inst");

            CMaterialInstance* Instance = CFactory::CreateNewOf<CMaterialInstance>(NewPath);
            if (Instance == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Failed to create a material instance at '{0}'.", NewPath);
                return;
            }

            // Through the setter so instancing at the depth limit is rejected rather than silently allowed.
            if (!Instance->SetParentMaterial(Parent))
            {
                ImGuiX::Notifications::NotifyError("'{0}' cannot be instanced any deeper.", Context.Asset->AssetName);
                Instance->ConditionalBeginDestroy();
                return;
            }

            // Mirrors what the factory does after assigning a parent: populates Parameters/MaterialIndex
            // so the instance is editable immediately instead of only after a reload.
            Instance->PostLoad();

            if (!CPackage::SavePackage(Instance->GetPackage(), NewPath))
            {
                ImGuiX::Notifications::NotifyError("Failed to save '{0}'.", NewPath);
                return;
            }

            // Without this the asset exists on disk but not in the registry, so it does not appear in
            // the content browser and the lookup below finds nothing to open.
            FAssetRegistry::Get().AssetCreated(Instance);

            ImGuiX::Notifications::NotifySuccess("Created '{0}'.", NewPath);

            // Open it: creating an instance is nearly always a prelude to tweaking its parameters.
            if (Context.ToolContext != nullptr)
            {
                if (const FAssetData* NewData = FAssetRegistry::Get().GetAssetByPath(FStringView(NewPath.c_str(), NewPath.size())))
                {
                    Context.ToolContext->OpenAssetEditor(NewData->AssetGUID);
                }
            }
        }

        void CreateCollisionShapeFrom(const FAssetActionContext& Context)
        {
            CStaticMesh* Mesh = Cast<CStaticMesh>(LoadObject<CObject>(Context.Asset->AssetGUID));
            if (Mesh == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Could not load '{0}' to build collision for it.", Context.Asset->Path);
                return;
            }

            const FFixedString NewPath = MakeSiblingAssetPath(
                FStringView(Context.Asset->Path.c_str(), Context.Asset->Path.size()), "_Collision");

            CCollisionShape* Shape = CFactory::CreateNewOf<CCollisionShape>(NewPath);
            if (Shape == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Failed to create a collision shape at '{0}'.", NewPath);
                return;
            }

            // The only place SourceMesh is ever assigned: it is ReadOnly on the asset, because hulls baked
            // against one mesh are meaningless on another.
            Shape->SourceMesh = Mesh;

            // Seed with a single hull so the asset is usable the moment it opens rather than empty. It is
            // the safest default -- always convex, so it works on dynamic bodies -- and the editor offers
            // the per-surface and triangle-mesh bakes from there.
            if (!Physics::CollisionGen::GenerateSingleHull(Mesh, Shape))
            {
                ImGuiX::Notifications::NotifyWarning("'{0}' produced no hull; the collision shape was created empty.",
                                                     Context.Asset->AssetName);
            }

            if (!CPackage::SavePackage(Shape->GetPackage(), NewPath))
            {
                ImGuiX::Notifications::NotifyError("Failed to save '{0}'.", NewPath);
                return;
            }

            FAssetRegistry::Get().AssetCreated(Shape);

            ImGuiX::Notifications::NotifySuccess("Created '{0}'.", NewPath);

            if (Context.ToolContext != nullptr)
            {
                if (const FAssetData* NewData = FAssetRegistry::Get().GetAssetByPath(FStringView(NewPath.c_str(), NewPath.size())))
                {
                    Context.ToolContext->OpenAssetEditor(NewData->AssetGUID);
                }
            }
        }

        void BrowseToParentMaterial(const FAssetActionContext& Context)
        {
            CMaterialInstance* Instance = Cast<CMaterialInstance>(LoadObject<CObject>(Context.Asset->AssetGUID));
            if (Instance == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Could not load '{0}'.", Context.Asset->Path);
                return;
            }

            // Held as CMaterialInterface, not CMaterial: the parent field is typed to CMaterial today,
            // so the parent is always a base material -- but nothing below depends on that, so if
            // instance-of-instance ever lands this follows the chain one link without changing.
            CMaterialInterface* Parent = Instance->Material.Get();
            if (Parent == nullptr || Parent->GetPackage() == nullptr)
            {
                // Not an error: an instance with no parent yet is a legitimate (if useless) state.
                ImGuiX::Notifications::NotifyWarning("'{0}' has no parent material.", Context.Asset->AssetName);
                return;
            }

            if (Context.ToolContext != nullptr)
            {
                const FString PackageName = Parent->GetPackage()->GetName().ToString();
                Context.ToolContext->BrowseToAsset(FStringView(PackageName.c_str(), PackageName.size()));
            }
        }
    }

    void RegisterBuiltinAssetActions()
    {
        FAssetActionRegistry& Registry = FAssetActionRegistry::Get();

        // On the interface, so an instance can be instanced too and the child simply inherits its values.
        Registry.RegisterAction(CMaterialInterface::StaticClass(), FAssetAction
        {
            .Label   = LE_ICON_CONTENT_DUPLICATE " Create Material Instance",
            .Execute = &CreateMaterialInstanceFrom,
        });

        Registry.RegisterAction(CStaticMesh::StaticClass(), FAssetAction
        {
            .Label   = LE_ICON_CUBE_OUTLINE " Create Collision Shape",
            .Execute = &CreateCollisionShapeFrom,
        });

        // No CanExecute: answering "does this have a parent?" means loading the instance, and the menu
        // is drawn every frame it is open. Enabled always, with a notification when there is nothing to
        // navigate to -- cheaper than loading every listed asset to decide whether to grey one entry.
        Registry.RegisterAction(CMaterialInstance::StaticClass(), FAssetAction
        {
            .Label   = LE_ICON_ARROW_UP_BOLD_BOX " Browse to Parent Material",
            .Execute = &BrowseToParentMaterial,
        });
    }
}
