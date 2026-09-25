#include "MCPAssetObjectTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentObjectEdit.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Asset/AssetOps.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Session/SessionOps.h"
#include "MCPTextMatch.h"
#include "Paths/Paths.h"

namespace Lumina::MCP
{
    namespace
    {
        constexpr const char* GAssetSaveHint = " Save with assets.save to persist.";

        SAssetInfo DescribeAssetData(const FAssetData& Data)
        {
            SAssetInfo Info;
            Info.Name       = FString(Data.AssetName.ToString().c_str());
            Info.Path       = FString(Data.Path.c_str());
            Info.AssetClass = FString(Data.AssetClass.ToString().c_str());
            Info.Guid       = FString(Data.AssetGUID.ToString().c_str());
            return Info;
        }

        void CollectReferencers(const FGuid& Guid, TVector<SAssetInfo>& Out)
        {
            for (const FAssetData* Data : FAssetRegistry::Get().GetReferencersOf(Guid))
            {
                if (Data != nullptr)
                {
                    Out.push_back(DescribeAssetData(*Data));
                }
            }
        }

        // A class is creatable when a factory claims it; there is no abstract flag to ask instead.
        /** The factory for Class or its nearest ancestor: a CDataAsset subclass is minted through CDataAsset's. */
        CFactory* FindFactoryFor(CClass* Class)
        {
            for (CFactory* Factory : CFactoryRegistry::Get().GetFactories())
            {
                if (Factory != nullptr && Factory->GetAssetClass() != nullptr && Class->IsChildOf(Factory->GetAssetClass()))
                {
                    return Factory;
                }
            }

            return nullptr;
        }

        void RegisterListClasses(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListAssetClassesParams, SListAssetClassesResult>(
                Owner, "assets.list_classes",
                "List every asset class assets.create can make.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListAssetClassesParams& In, SListAssetClassesResult& Out)
                {
                    for (CFactory* Factory : CFactoryRegistry::Get().GetFactories())
                    {
                        CClass* Class = Factory != nullptr ? Factory->GetAssetClass() : nullptr;
                        if (Class == nullptr)
                        {
                            continue;
                        }

                        SAssetClassInfo Info;
                        Info.Name                 = FString(Class->GetName().ToString().c_str());
                        Info.Category             = Factory->GetCategory();
                        Info.bHasCreationDialogue = Factory->HasCreationDialogue();

                        if (!ContainsTextFold(FStringView(Info.Name), In.Contains)
                            && !ContainsTextFold(FStringView(Info.Category), In.Contains))
                        {
                            continue;
                        }

                        Out.Classes.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} creatable asset class(es).", Out.Classes.size()));
                });
        }

        void RegisterCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateAssetParams, SAssetInfo>(
                Owner, "assets.create",
                "Create an empty asset of any creatable class and save it to disk.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateAssetParams& In, SAssetInfo& Out)
                {
                    if (In.Name.empty())
                    {
                        return Agent::FToolResult::Error("An asset needs a name.");
                    }

                    FString Error;
                    CClass* Class = Agent::FindClassByName(FStringView(In.ClassName), nullptr, Error);
                    if (Class == nullptr)
                    {
                        return Agent::FToolResult::Error(Error + " Call assets.list_classes.");
                    }

                    if (FindFactoryFor(Class) == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "{} is not a creatable asset class. Call assets.list_classes.", In.ClassName));
                    }

                    if (!AssetOps::IsAssetLocation(FStringView(In.Folder)))
                    {
                        return Agent::FToolResult::Error(
                            "Assets belong under /Game/Content, since nothing scans for assets elsewhere.");
                    }

                    if (!VFS::IsDirectory(In.Folder))
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "{} is not a folder. Use assets.create_folder first.", In.Folder));
                    }

                    FFixedString Path = Paths::Combine(FStringView(In.Folder), FStringView(In.Name));
                    CPackage::AddPackageExt(Path);

                    if (VFS::Exists(Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} already exists.", Path));
                    }

                    CObject* Asset = CFactory::CreateNewOf(Class, FStringView(Path.c_str(), Path.size()));
                    if (Asset == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not create a {} at {}.", In.ClassName, Path));
                    }

                    if (!CPackage::SavePackage(Asset->GetPackage(), Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
                    }

                    FAssetRegistry::Get().AssetCreated(Asset);

                    Out.Name       = FString(Asset->GetName().ToString().c_str());
                    Out.Path       = FString(Path.c_str());
                    Out.AssetClass = In.ClassName;
                    Out.Guid       = FString(Asset->GetGUID().ToString().c_str());

                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        void RegisterDescribeAsset(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribeAssetParams, SDescribeAssetResult>(
                Owner, "assets.describe",
                "Report an asset's class, every reflected field, and what references it.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribeAssetParams& In, SDescribeAssetResult& Out)
                {
                    CObject* Asset = nullptr;
                    FString Error;
                    if (!Agent::ResolveAssetObject(FStringView(In.Asset), Asset, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    nlohmann::json Properties;
                    if (const Agent::FMarshalResult Written = Agent::WriteStruct(Asset->GetClass(), Asset, Properties);
                        !Written.IsValid())
                    {
                        return Agent::FToolResult::Error(Written.Error);
                    }

                    Out.Name       = FString(Asset->GetName().ToString().c_str());
                    Out.ClassName  = FString(Asset->GetClass()->GetName().ToString().c_str());
                    Out.Guid       = FString(Asset->GetGUID().ToString().c_str());
                    Out.Properties = FString(Properties.dump(2).c_str());

                    if (CPackage* Package = Asset->GetPackage())
                    {
                        Out.Path = FString(Package->GetPackagePath().c_str());
                    }

                    Agent::CollectUnwritableFields(Asset->GetClass(), FStringView(), Out.UnwritableFields);
                    CollectReferencers(Asset->GetGUID(), Out.Referencers);

                    return Agent::FToolResult::Ok(Lumina::Format("{} ({})\n{}", Out.Name, Out.ClassName, Out.Properties));
                });
        }

        void RegisterSetAssetProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetAssetPropertyParams, SAssetPropertyResult>(
                Owner, "assets.set_property",
                "Set one field on an asset. Undoable only while the asset is open in an editor.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetAssetPropertyParams& In, SAssetPropertyResult& Out)
                {
                    CObject* Asset = nullptr;
                    FString Error;
                    if (!Agent::ResolveAssetObject(FStringView(In.Asset), Asset, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    nlohmann::json Value = nlohmann::json::parse(In.Value.c_str(), nullptr, false);
                    if (Value.is_discarded())
                    {
                        return Agent::FToolResult::Error(
                            "Value is not JSON. Strings need quotes, so \"Torch\" rather than Torch.");
                    }

                    const Agent::FObjectEditResult Edit = Agent::SetObjectProperty(
                        Asset->GetClass(), Asset, Asset, FStringView(In.Path), Value, "Set Asset Property (agent)");

                    if (!Edit.IsValid())
                    {
                        return Agent::FToolResult::Error(Edit.Error);
                    }

                    Out.Previous  = Edit.Previous;
                    Out.Current   = Edit.Current;
                    Out.bUndoable = Edit.bUndoable;

                    return Agent::FToolResult::Ok(Lumina::Format("{}.{} is now {}.{}{}",
                        Asset->GetName(), In.Path, Out.Current, GAssetSaveHint,
                        Edit.bUndoable ? "" : " Applied without undo (no editor open for this asset)."));
                });
        }

        void RegisterDelete(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDeleteAssetParams, SDeleteAssetResult>(
                Owner, "assets.delete",
                "Delete an asset from disk. Not undoable. Refuses while other assets reference it unless bForce.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDeleteAssetParams& In, SDeleteAssetResult& Out)
                {
                    const TOptional<FGuid> Guid = FGuid::TryParse(FStringView(In.Asset));
                    if (!Guid.IsSet())
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' is not a GUID.", In.Asset));
                    }

                    // The registry knows the path without loading, which a file about to go should not need.
                    const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(*Guid);
                    if (Data == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No asset has GUID {}.", In.Asset));
                    }

                    Out.Path = FString(Data->Path.c_str());
                    CollectReferencers(*Guid, Out.Referencers);

                    if (!Out.Referencers.empty() && !In.bForce)
                    {
                        return Agent::FToolResult::Ok(Lumina::Format(
                            "{} asset(s) reference {}. Pass bForce to delete anyway.", Out.Referencers.size(), Out.Path));
                    }

                    const AssetOps::FDeleteAssetResult Deleted =
                        AssetOps::DeleteAsset(FStringView(Out.Path), SessionOps::GetToolContext());
                    if (!Deleted.bDeleted)
                    {
                        return Agent::FToolResult::Error(Deleted.Error);
                    }

                    Out.bDeleted = true;
                    return Agent::FToolResult::Ok(Lumina::Format("Deleted {}.", Out.Path));
                });
        }
    }

    void RegisterAssetObjectTools(FStringView Owner)
    {
        RegisterListClasses(Owner);
        RegisterCreate(Owner);
        RegisterDescribeAsset(Owner);
        RegisterSetAssetProperty(Owner);
        RegisterDelete(Owner);
    }
}
