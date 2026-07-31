#include "pch.h"
#include "EditorAssetActions.h"

#include "EditorToolContext.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
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

    namespace
    {
        // "<Dir>/<Base><Suffix>.lasset", deduplicated. Follows the content browser's own new-asset
        // recipe exactly -- combine, AddPackageExt, MakeUniqueFilePath. The extension is not optional:
        // a package saved to an extension-less path is not a loadable asset, and the dedupe check has
        // to run against the real filename or it never finds the collision it is looking for.
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

        void CreateMaterialInstanceFrom(const FAssetActionContext& Context)
        {
            CMaterial* Parent = Cast<CMaterial>(LoadObject<CObject>(Context.Asset->AssetGUID));
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

            Instance->Material = Parent;
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

        // On CMaterial rather than CMaterialInterface: instancing an instance is a different operation
        // (it would want to inherit the override set), so it is deliberately not offered here.
        Registry.RegisterAction(CMaterial::StaticClass(), FAssetAction
        {
            .Label   = LE_ICON_CONTENT_DUPLICATE " Create Material Instance",
            .Execute = &CreateMaterialInstanceFrom,
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
