#include "RuntimePCH.h"
#include "PluginDescriptor.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Platform/Filesystem/FileHelper.h"

#include "nlohmann/json.hpp"

namespace Lumina
{
    FStringView LexToString(EPluginModuleType Type)
    {
        switch (Type)
        {
            case EPluginModuleType::Runtime:   return "Runtime";
            case EPluginModuleType::Editor:    return "Editor";
            case EPluginModuleType::Developer: return "Developer";
            default:                           return "Unknown";
        }
    }

    EPluginModuleType ParsePluginModuleType(FStringView Str, EPluginModuleType Default)
    {
        if (Str.empty())        return Default;
        if (Str == "Runtime")   return EPluginModuleType::Runtime;
        if (Str == "Editor")    return EPluginModuleType::Editor;
        if (Str == "Developer") return EPluginModuleType::Developer;
        LOG_WARN("[PluginManager] Unknown module Type '{}' in .lplugin; falling back to '{}'",
            Str, LexToString(Default));
        return Default;
    }

    namespace
    {
        // Tiny JSON helpers: tolerate missing fields rather than throwing.
        FString GetString(const nlohmann::json& J, const char* Key, const char* Default = "")
        {
            auto It = J.find(Key);
            if (It == J.end() || !It->is_string())
            {
                return FString(Default);
            }
            const std::string& V = It->get_ref<const std::string&>();
            return FString(V.c_str(), V.size());
        }

        int32 GetInt(const nlohmann::json& J, const char* Key, int32 Default = 0)
        {
            auto It = J.find(Key);
            if (It == J.end() || !It->is_number_integer())
            {
                return Default;
            }
            return It->get<int32>();
        }

        bool GetBool(const nlohmann::json& J, const char* Key, bool Default = false)
        {
            auto It = J.find(Key);
            if (It == J.end() || !It->is_boolean())
            {
                return Default;
            }
            return It->get<bool>();
        }

        TVector<FString> GetStringArray(const nlohmann::json& J, const char* Key)
        {
            TVector<FString> Out;
            auto It = J.find(Key);
            if (It == J.end() || !It->is_array())
            {
                return Out;
            }
            Out.reserve(It->size());
            for (const auto& V : *It)
            {
                if (V.is_string())
                {
                    const std::string& S = V.get_ref<const std::string&>();
                    Out.emplace_back(S.c_str(), S.size());
                }
            }
            return Out;
        }
    }

    bool FPluginDescriptor::LoadFromFile(FStringView FilePath, FPluginDescriptor& OutDescriptor, FString& OutError)
    {
        FString Json;
        if (!FileHelper::LoadFileIntoString(Json, FilePath))
        {
            OutError = "Failed to read plugin descriptor file: ";
            OutError.append(FilePath.data(), FilePath.size());
            return false;
        }
        return LoadFromString(Json, OutDescriptor, OutError);
    }

    bool FPluginDescriptor::LoadFromString(FStringView JsonText, FPluginDescriptor& OutDescriptor, FString& OutError)
    {
        nlohmann::json J;
        try
        {
            J = nlohmann::json::parse(JsonText.data(), JsonText.data() + JsonText.size());
        }
        catch (const std::exception& E)
        {
            OutError = "Malformed plugin descriptor JSON: ";
            OutError += E.what();
            return false;
        }

        if (!J.is_object())
        {
            OutError = "Plugin descriptor root must be an object.";
            return false;
        }

        OutDescriptor.FormatVersion      = GetInt(J, "FormatVersion", 1);
        OutDescriptor.Name               = GetString(J, "Name");
        OutDescriptor.Version            = GetInt(J, "Version", 1);
        OutDescriptor.VersionName        = GetString(J, "VersionName");
        OutDescriptor.Author             = GetString(J, "Author");
        OutDescriptor.Description        = GetString(J, "Description");
        OutDescriptor.Category           = GetString(J, "Category");
        OutDescriptor.bEnabledByDefault  = GetBool(J,   "EnabledByDefault", true);
        OutDescriptor.bEditorOnly        = GetBool(J,   "EditorOnly",       false);
        OutDescriptor.bContainsContent   = GetBool(J,   "ContainsContent",  true);
        OutDescriptor.SupportedPlatforms = GetStringArray(J, "SupportedPlatforms");

        if (OutDescriptor.Name.empty())
        {
            OutError = "Plugin descriptor missing required field 'Name'.";
            return false;
        }

        // Dependencies
        if (auto It = J.find("Dependencies"); It != J.end() && It->is_array())
        {
            OutDescriptor.Dependencies.reserve(It->size());
            for (const auto& D : *It)
            {
                if (!D.is_object()) continue;
                FPluginDependency Dep;
                Dep.Name      = GetString(D, "Name");
                Dep.Version   = GetInt(D,   "Version", 0);
                Dep.bOptional = GetBool(D,  "Optional", false);
                if (!Dep.Name.empty())
                {
                    OutDescriptor.Dependencies.emplace_back(Move(Dep));
                }
            }
        }

        // Cook roots (Phase 1: read + carry into engine; cooker iterates them)
        if (auto It = J.find("CookRoots"); It != J.end() && It->is_array())
        {
            OutDescriptor.CookRoots.reserve(It->size());
            for (const auto& R : *It)
            {
                FCookRoot Root;
                if (R.is_string())
                {
                    // Shorthand form: bare string is the asset path.
                    const std::string& S = R.get_ref<const std::string&>();
                    Root.Asset.assign(S.c_str(), S.size());
                }
                else if (R.is_object())
                {
                    Root.Asset = GetString(R, "Asset");
                    if (Root.Asset.empty())
                    {
                        LOG_WARN("[PluginManager] Plugin '{}' CookRoots entry has no 'Asset' field; skipping",
                            OutDescriptor.Name);
                    }
                    Root.Chunk = FName(GetString(R, "Chunk").c_str());
                }
                if (!Root.Asset.empty())
                {
                    OutDescriptor.CookRoots.emplace_back(Move(Root));
                }
            }
        }

        // Modules
        if (auto It = J.find("Modules"); It != J.end() && It->is_array())
        {
            OutDescriptor.Modules.reserve(It->size());
            for (const auto& M : *It)
            {
                if (!M.is_object()) continue;
                FPluginModuleDescriptor Mod;
                Mod.Name               = GetString(M, "Name");
                Mod.Type               = ParsePluginModuleType(GetString(M, "Type", "Runtime"));
                Mod.LoadingPhase       = ParsePluginLoadingPhase(GetString(M, "LoadingPhase", "PreEngineInit"));
                Mod.SupportedPlatforms = GetStringArray(M, "SupportedPlatforms");
                if (Mod.Name.empty())
                {
                    OutError = "Plugin '";
                    OutError += OutDescriptor.Name;
                    OutError += "' has a module with no Name.";
                    return false;
                }
                OutDescriptor.Modules.emplace_back(Move(Mod));
            }
        }

        return true;
    }
}
