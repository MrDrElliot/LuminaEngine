#include "EditorToolRegistry.h"

#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "EditorTool.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        // Stored lowercase with a leading dot, so lookups ignore how the path was cased on disk.
        FString NormalizeExtension(FStringView Extension)
        {
            FString Result;
            if (!Extension.empty() && Extension.front() != '.')
            {
                Result.push_back('.');
            }

            for (char Ch : Extension)
            {
                Result.push_back((Ch >= 'A' && Ch <= 'Z') ? char(Ch - 'A' + 'a') : Ch);
            }

            return Result;
        }

        // FName::c_str() is only good for a few more calls, and every message below names two owners.
        FString DescribeOwner(FName Owner)
        {
            return Owner.IsNone() ? FString("<unnamed>") : Owner.ToString();
        }
    }

    FName FEditorToolRegistry::BuiltInOwner()
    {
        // Not a file-scope FName, since the pool is not up during static initialization.
        static const FName Owner("Editor");
        return Owner;
    }

    FEditorToolRegistry& FEditorToolRegistry::Get()
    {
        static FEditorToolRegistry Instance;
        return Instance;
    }

    void FEditorToolRegistry::RegisterAssetEditor(CClass* AssetClass, FAssetEditorFactory Factory, FName Owner)
    {
        if (AssetClass == nullptr || !Factory)
        {
            LOG_WARN("[EditorToolRegistry] Ignoring an asset editor registration from {} with no class or no factory.",
                DescribeOwner(Owner));
            return;
        }

        // Not an error, but logged because two plugins claiming one type resolve by load order.
        if (auto Existing = AssetEditors.find(AssetClass); Existing != AssetEditors.end())
        {
            LOG_WARN("[EditorToolRegistry] {} replaces the '{}' asset editor previously registered by {}.",
                DescribeOwner(Owner), AssetClass->GetName(), DescribeOwner(Existing->second.Owner));
        }

        AssetEditors.insert_or_assign(AssetClass, FAssetRegistration{ Move(Factory), Owner });
    }

    void FEditorToolRegistry::RegisterFileEditor(FStringView Extension, FFileEditorFactory Factory, FName Owner)
    {
        if (Extension.empty() || !Factory)
        {
            LOG_WARN("[EditorToolRegistry] Ignoring a file editor registration from {} with no extension or no factory.",
                DescribeOwner(Owner));
            return;
        }

        FString Normalized = NormalizeExtension(Extension);

        if (auto Existing = FileEditors.find(Normalized); Existing != FileEditors.end())
        {
            LOG_WARN("[EditorToolRegistry] {} replaces the '{}' file editor previously registered by {}.",
                DescribeOwner(Owner), Normalized, DescribeOwner(Existing->second.Owner));
        }

        FileEditors.insert_or_assign(Move(Normalized), FFileRegistration{ Move(Factory), Owner });
    }

    bool FEditorToolRegistry::UnregisterAssetEditor(CClass* AssetClass)
    {
        if (AssetClass == nullptr)
        {
            return false;
        }

        return AssetEditors.erase(AssetClass) > 0;
    }

    bool FEditorToolRegistry::UnregisterFileEditor(FStringView Extension)
    {
        if (Extension.empty())
        {
            return false;
        }

        return FileEditors.erase(NormalizeExtension(Extension)) > 0;
    }

    void FEditorToolRegistry::UnregisterFileEditors(std::initializer_list<FStringView> Extensions)
    {
        for (FStringView Extension : Extensions)
        {
            UnregisterFileEditor(Extension);
        }
    }

    int32 FEditorToolRegistry::UnregisterAll(FName Owner)
    {
        if (Owner.IsNone())
        {
            // Silently doing nothing would read as success to a caller that has just failed to clean up.
            LOG_ERROR("[EditorToolRegistry] UnregisterAll needs the owner the registrations were made under. "
                      "An unnamed owner is the editor's own built-ins, which are not a plugin's to remove.");
            return 0;
        }

        int32 Removed = 0;

        for (auto Itr = AssetEditors.begin(); Itr != AssetEditors.end();)
        {
            if (Itr->second.Owner == Owner)
            {
                Itr = AssetEditors.erase(Itr);
                ++Removed;
            }
            else
            {
                ++Itr;
            }
        }

        for (auto Itr = FileEditors.begin(); Itr != FileEditors.end();)
        {
            if (Itr->second.Owner == Owner)
            {
                Itr = FileEditors.erase(Itr);
                ++Removed;
            }
            else
            {
                ++Itr;
            }
        }

        LOG_INFO("[EditorToolRegistry] Unregistered {} editor(s) owned by {}.", Removed, Owner);

        return Removed;
    }

    FEditorToolPtr FEditorToolRegistry::CreateAssetEditor(IEditorToolContext* Context, CObject* Asset) const
    {
        if (Asset == nullptr)
        {
            return nullptr;
        }

        // Walks up the class chain, so a concrete registration wins over one for a base type.
        for (CClass* Class = Asset->GetClass(); Class != nullptr; Class = Class->GetSuperClass())
        {
            auto Itr = AssetEditors.find(Class);
            if (Itr != AssetEditors.end())
            {
                return Itr->second.Factory(Context, Asset);
            }
        }

        return nullptr;
    }

    FEditorToolPtr FEditorToolRegistry::CreateFileEditor(IEditorToolContext* Context, FStringView VirtualPath) const
    {
        const FString Ext = NormalizeExtension(VFS::Extension(VirtualPath));

        auto Itr = FileEditors.find(Ext);
        if (Itr != FileEditors.end())
        {
            return Itr->second.Factory(Context, VirtualPath);
        }

        return nullptr;
    }

    bool FEditorToolRegistry::HasAssetEditor(CClass* AssetClass) const
    {
        return AssetClass != nullptr && AssetEditors.find(AssetClass) != AssetEditors.end();
    }

    bool FEditorToolRegistry::HasFileEditor(FStringView Extension) const
    {
        return FileEditors.find(NormalizeExtension(Extension)) != FileEditors.end();
    }

    FName FEditorToolRegistry::GetAssetEditorOwner(CClass* AssetClass) const
    {
        if (AssetClass == nullptr)
        {
            return FName();
        }

        auto Itr = AssetEditors.find(AssetClass);
        return Itr != AssetEditors.end() ? Itr->second.Owner : FName();
    }
}
