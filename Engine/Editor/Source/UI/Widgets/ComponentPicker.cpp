#include "ComponentPicker.h"

#include "EntityScriptPicker.h"
#include "Containers/HashTable.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Scripting/ScriptStruct.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "World/Entity/Components/Component.h"

namespace Lumina::ComponentPicker
{
    namespace
    {
        constexpr FStringView kTypesPackagePrefix = "/Types/";

        // The engine's own types all share one package, so anything else came from a plugin or the game's module.
        constexpr FStringView kEnginePackageModule = "Engine";

        FString NativeOrigin(const CStruct* Struct, const THashMap<FString, FString>& PluginByModule)
        {
            const CPackage* Package = Struct->GetPackage();
            const FString PackageName = Package != nullptr ? Package->GetName().ToString() : FString();
            const FStringView View(PackageName);
            const FStringView Module = View.StartsWith(kTypesPackagePrefix) ? View.Mid(kTypesPackagePrefix.Len()) : FStringView();
            if (Module.IsEmpty() || Module == kEnginePackageModule)
            {
                return FString(kEnginePackageModule);
            }

            const FString ModuleName(Module.GetData(), Module.Len());
            auto Plugin = PluginByModule.Find(ModuleName);
            return Plugin != nullptr ? (*Plugin) : ModuleName;
        }

        THashMap<FString, FString> GatherPluginModules()
        {
            THashMap<FString, FString> PluginByModule;
            for (FPlugin* Plugin : FPluginManager::Get().GetEnabledPlugins())
            {
                for (const FPluginModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
                {
                    PluginByModule.TryAdd(Module.Name, FString(Plugin->GetName()));
                }
            }
            return PluginByModule;
        }
    }

    void GatherItems(TVector<FPickerItem>& Out, const TFunction<bool(CStruct*)>& IsAlreadyPresent)
    {
        Out.Reset();
        const EntityScriptPicker::FScriptSourceIndex SourceIndex;
        const THashMap<FString, FString> PluginByModule = GatherPluginModules();

        ForEachComponentStruct([&](CStruct* Struct)
        {
            if (Struct->HasMeta("HideInComponentList"))
            {
                return;
            }

            const FString FullName = Struct->GetName().ToString();
            const FStringView Short = EntityScriptPicker::ShortTypeName(FullName);
            const bool bManaged = Cast<CScriptStruct>(Struct) != nullptr;
            const FString Declared = Struct->HasMeta("Category") ? Struct->GetMeta("Category") : FString();

            FPickerItem& Item = Out.EmplaceGetRef();
            Item.Name      = FString(*Struct->MakeDisplayName());
            Item.Key       = FullName;
            Item.Icon      = bManaged ? LE_ICON_LANGUAGE_CSHARP : LE_ICON_LANGUAGE_CPP;
            Item.bDisabled = IsAlreadyPresent && IsAlreadyPresent(Struct);

            if (bManaged)
            {
                const DotNet::FScriptSourceFolder* Source = SourceIndex.Find(Struct, Short);
                Item.Category = Source != nullptr ? Source->Folder : Declared;
                Item.Tooltip  = Source != nullptr ? Lumina::Format("{}\n{}", FullName, Source->Path) : FullName;
            }
            else
            {
                const FString Origin = NativeOrigin(Struct, PluginByModule);
                Item.Category = Declared.IsEmpty()
                    ? Lumina::Format("{}/{}", EntityScriptPicker::NativeCategory, Origin)
                    : Lumina::Format("{}/{}/{}", EntityScriptPicker::NativeCategory, Origin, Declared);
                Item.Tooltip  = FullName;
            }

            if (Struct->HasMeta("ToolTip"))
            {
                Item.Tooltip = Lumina::Format("{}\n{}", Struct->GetMeta("ToolTip"), Item.Tooltip);
            }
            if (Item.bDisabled)
            {
                Item.Tooltip = Lumina::Format("Already on the entity.\n{}", Item.Tooltip);
            }
        });
    }

    CStruct* FindComponent(FStringView Key)
    {
        CStruct* Found = nullptr;
        ForEachComponentStruct([&](CStruct* Struct)
        {
            if (Found == nullptr && Struct->GetName().ToString() == Key)
            {
                Found = Struct;
            }
        });
        return Found;
    }
}
