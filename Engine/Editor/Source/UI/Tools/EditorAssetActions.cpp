#include "EditorPCH.h"
#include "Tools/Import/ImportPaths.h"
#include "EditorAssetActions.h"

#include "EditorToolContext.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/Factories/Factory.h"
#include "Assets/Factories/AnimationMontageFactory/AnimationMontageFactory.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
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
        // Accumulates rather than stopping at the first match, since an asset can contribute at several levels.
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
        Name.append(Suffix);

        // Free of loaded packages and in-flight imports as well as files, since CreatePackage refuses all three.
        FFixedString Path = Import::PathReservations::FindFree(Paths::Combine(Parent, Name));
        if (!Path.empty())
        {
            CPackage::AddPackageExt(Path);
        }
        return Path;
    }

    CObject* DuplicateAssetPackage(CObject* PrimaryAsset, FStringView DestPath)
    {
        CPackage* SourcePackage = PrimaryAsset != nullptr ? PrimaryAsset->GetPackage() : nullptr;
        if (SourcePackage == nullptr)
        {
            return nullptr;
        }

        // FullyLoad is what the saver uses, since a lazily-loaded sibling export is missed otherwise.
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

        // The primary object shares its package's name, while sub-objects are found by name instead.
        const FName DestAssetName(VFS::FileName(DestPath, /*bRemoveExtension*/ true));
        const THashMap<CObject*, CObject*> Remap = CopyObjectsInto(SourceObjects, DestPackage, [&](CObject* Source)
        {
            return Source == PrimaryAsset ? DestAssetName : Source->GetName();
        });
        if (Remap.empty() || !CPackage::SavePackage(DestPackage, DestPath))
        {
            return nullptr;
        }

        return Remap.find(PrimaryAsset)->second;
    }

    CObject* DuplicateAsset(FStringView SourcePath, FFixedString& OutPath)
    {
        const FAssetData* Data   = FAssetRegistry::Get().GetAssetByPath(SourcePath);
        CObject*          Source = Data != nullptr ? LoadObject<CObject>(Data->AssetGUID) : nullptr;
        if (Source == nullptr)
        {
            return nullptr;
        }

        // A material keeps its node graph as a second export, so the whole package is copied and rewritten.
        OutPath = MakeSiblingAssetPath(SourcePath, "_Copy");
        CObject* Copy = DuplicateAssetPackage(Source, FStringView(OutPath.c_str(), OutPath.size()));
        if (Copy != nullptr)
        {
            FAssetRegistry::Get().AssetCreated(Copy);
        }
        return Copy;
    }

    THashMap<CObject*, CObject*> CopyObjectsInto(TSpan<CObject* const> Sources, CPackage* Dest, const TFunction<FName(CObject*)>& NameFor)
    {
        // Pass 1 constructs every copy first, so pass 2 can resolve a reference to any sibling.
        THashMap<CObject*, CObject*> Remap;
        Remap.reserve(Sources.size());

        for (CObject* Source : Sources)
        {
            CObject* Copy = NewObject(Source->GetClass(), Dest, NameFor(Source), FGuid::New(), OF_Public);
            if (Copy == nullptr)
            {
                return {};
            }

            // The saver rebuilds ExportTable, so being outered to Dest is what makes a copy an export.
            Remap.emplace(Source, Copy);
        }

        // Copying reflected properties alone loses state a class serializes by hand, such as a mesh resource.
        for (CObject* Source : Sources)
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
        }

        // A PostLoad that reaches for a sibling needs the whole set present and remapped first.
        for (CObject* Source : Sources)
        {
            Remap[Source]->PostLoad();
        }
        return Remap;
    }

    namespace
    {
        void CreateMaterialInstanceFrom(const FAssetActionContext& Context)
        {
            // The interface, since instancing an instance just parents to it and inherits everything.
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

            // Populates Parameters so the instance is editable immediately rather than only after a reload.
            Instance->PostLoad();

            if (!CPackage::SavePackage(Instance->GetPackage(), NewPath))
            {
                ImGuiX::Notifications::NotifyError("Failed to save '{0}'.", NewPath);
                return;
            }

            // Without this the asset exists on disk but not in the registry, so nothing can find it.
            FAssetRegistry::Get().AssetCreated(Instance);

            ImGuiX::Notifications::NotifySuccess("Created '{0}'.", NewPath);

            // Opened, since creating an instance is nearly always a prelude to tweaking its parameters.
            if (Context.ToolContext != nullptr)
            {
                if (const FAssetData* NewData = FAssetRegistry::Get().GetAssetByPath(FStringView(NewPath.c_str(), NewPath.size())))
                {
                    Context.ToolContext->OpenAssetEditor(NewData->AssetGUID);
                }
            }
        }

        void CreateMontageFrom(const FAssetActionContext& Context)
        {
            CAnimation* Clip = Cast<CAnimation>(LoadObject<CObject>(Context.Asset->AssetGUID));
            if (Clip == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Could not load '{0}' to build a montage from it.", Context.Asset->Path);
                return;
            }

            const FFixedString NewPath = MakeSiblingAssetPath(
                FStringView(Context.Asset->Path.c_str(), Context.Asset->Path.size()), "_Montage");

            CAnimationMontage* Montage = CFactory::CreateNewOf<CAnimationMontage>(NewPath);
            if (Montage == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Failed to create a montage at '{0}'.", NewPath);
                return;
            }

            CAnimationMontageFactory::InitializeMontage(*Montage, Clip->Skeleton.Get(), Clip);

            if (!CPackage::SavePackage(Montage->GetPackage(), NewPath))
            {
                ImGuiX::Notifications::NotifyError("Failed to save '{0}'.", NewPath);
                return;
            }

            FAssetRegistry::Get().AssetCreated(Montage);

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

            // Held as the interface, so instance-of-instance would follow the chain without changing this.
            CMaterialInterface* Parent = Instance->Material.Get();
            if (Parent == nullptr || Parent->GetPackage() == nullptr)
            {
                // Not an error, since an instance with no parent yet is a legitimate if useless state.
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

        Registry.RegisterAction(CAnimation::StaticClass(), FAssetAction
        {
            .Label   = LE_ICON_ANIMATION " Create Animation Montage",
            .Execute = &CreateMontageFrom,
        });

        // Answering whether it has a parent means loading it, and the menu redraws every frame.
        Registry.RegisterAction(CMaterialInstance::StaticClass(), FAssetAction
        {
            .Label   = LE_ICON_ARROW_UP_BOLD_BOX " Browse to Parent Material",
            .Execute = &BrowseToParentMaterial,
        });
    }
}
