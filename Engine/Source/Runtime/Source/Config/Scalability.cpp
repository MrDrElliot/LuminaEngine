#include "RuntimePCH.h"
#include "Scalability.h"

#include "Config.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

using Json = nlohmann::json;

namespace Lumina::Scalability
{
    namespace
    {
        // The same shape a project's /Config/Scalability.json takes, which replaces any group it names.
        constexpr const char* EngineTable = R"json({
            "ViewDistance": {
                "Low":    { "RendererSettings.LODDistanceScale": 0.6 },
                "Medium": { "RendererSettings.LODDistanceScale": 0.8 },
                "High":   { "RendererSettings.LODDistanceScale": 1.0 },
                "Ultra":  { "RendererSettings.LODDistanceScale": 1.5 }
            },
            "Shadows": {
                "Low":    { "RendererSettings.ShadowQuality": "Low",    "RendererSettings.PointShadowResolution": 256,  "RendererSettings.bCloudShadows": false, "RendererSettings.CloudShadowSteps": 4 },
                "Medium": { "RendererSettings.ShadowQuality": "Medium", "RendererSettings.PointShadowResolution": 512,  "RendererSettings.bCloudShadows": true,  "RendererSettings.CloudShadowSteps": 6 },
                "High":   { "RendererSettings.ShadowQuality": "High",   "RendererSettings.PointShadowResolution": 512,  "RendererSettings.bCloudShadows": true,  "RendererSettings.CloudShadowSteps": 8 },
                "Ultra":  { "RendererSettings.ShadowQuality": "Ultra",  "RendererSettings.PointShadowResolution": 1024, "RendererSettings.bCloudShadows": true,  "RendererSettings.CloudShadowSteps": 12 }
            },
            "Effects": {
                "Low":    { "RendererSettings.bScreenSpaceReflections": false, "RendererSettings.SSRQuality": "Low",    "RendererSettings.bSupersampleVolumetricLights": false },
                "Medium": { "RendererSettings.bScreenSpaceReflections": false, "RendererSettings.SSRQuality": "Low",    "RendererSettings.bSupersampleVolumetricLights": false },
                "High":   { "RendererSettings.bScreenSpaceReflections": true,  "RendererSettings.SSRQuality": "Medium", "RendererSettings.bSupersampleVolumetricLights": true },
                "Ultra":  { "RendererSettings.bScreenSpaceReflections": true,  "RendererSettings.SSRQuality": "Ultra",  "RendererSettings.bSupersampleVolumetricLights": true }
            },
            "PostProcess": {
                "Low":    { "RendererSettings.bEnableGTAO": false, "RendererSettings.GTAOQualityLevel": 0 },
                "Medium": { "RendererSettings.bEnableGTAO": true,  "RendererSettings.GTAOQualityLevel": 1 },
                "High":   { "RendererSettings.bEnableGTAO": true,  "RendererSettings.GTAOQualityLevel": 2 },
                "Ultra":  { "RendererSettings.bEnableGTAO": true,  "RendererSettings.GTAOQualityLevel": 3 }
            },
            "Textures": {
                "Low":    { "TextureStreamingSettings.ResolutionBias": 0.5,  "TextureStreamingSettings.PoolSizeMB": 512,  "RendererSettings.MaxAnisotropy": 2 },
                "Medium": { "TextureStreamingSettings.ResolutionBias": 0.75, "TextureStreamingSettings.PoolSizeMB": 768,  "RendererSettings.MaxAnisotropy": 4 },
                "High":   { "TextureStreamingSettings.ResolutionBias": 1.0,  "TextureStreamingSettings.PoolSizeMB": 1024, "RendererSettings.MaxAnisotropy": 8 },
                "Ultra":  { "TextureStreamingSettings.ResolutionBias": 1.0,  "TextureStreamingSettings.PoolSizeMB": 2048, "RendererSettings.MaxAnisotropy": 16 }
            },
            "AntiAliasing": {
                "Low":    { "RendererSettings.SMAAMode": "Off",    "RendererSettings.SMAAQuality": "Low" },
                "Medium": { "RendererSettings.SMAAMode": "SMAA1x", "RendererSettings.SMAAQuality": "Medium" },
                "High":   { "RendererSettings.SMAAMode": "SMAA1x", "RendererSettings.SMAAQuality": "High" },
                "Ultra":  { "RendererSettings.SMAAMode": "SMAA1x", "RendererSettings.SMAAQuality": "Ultra" }
            }
        })json";

        // Each settings class as the project configured it, captured before the first group overrode any of it.
        THashMap<CClass*, Json> ProjectValues;

        Json BuildTable()
        {
            Json Table = Json::parse(EngineTable);

            FString Text;
            if (VFS::Exists("/Config/Scalability.json") && VFS::ReadFile(Text, "/Config/Scalability.json"))
            {
                try
                {
                    for (const auto& [Group, Levels] : Json::parse(Text.c_str()).items())
                    {
                        Table[Group] = Levels;
                    }
                }
                catch (const std::exception& Ex)
                {
                    LOG_WARN("Scalability: /Config/Scalability.json could not be read ({}); using the engine's table.", Ex.what());
                }
            }
            return Table;
        }

        const Json& GetTable()
        {
            static const Json Table = BuildTable();
            return Table;
        }

        const char* LevelName(EQualityLevel Level)
        {
            switch (Level)
            {
            case EQualityLevel::Low:    return "Low";
            case EQualityLevel::Medium: return "Medium";
            case EQualityLevel::High:   return "High";
            case EQualityLevel::Ultra:  return "Ultra";
            default:                    return nullptr;
            }
        }

        CClass* FindSettingsClass(FStringView Section)
        {
            CClass* Found = nullptr;
            if (GConfig != nullptr)
            {
                GConfig->ForEachSettingsClass([&](CClass* Class)
                {
                    if (Found == nullptr && FStringView(FConfig::GetSettingsSection(Class)) == Section)
                    {
                        Found = Class;
                    }
                });
            }
            return Found;
        }

        // Splits "Section.Property" keys into one JSON object per settings class, the shape LoadStruct reads.
        THashMap<FString, Json> BySection(const Json& Values)
        {
            THashMap<FString, Json> Sections;
            for (const auto& [Key, Value] : Values.items())
            {
                const size_t Dot = Key.find('.');
                if (Dot == std::string::npos)
                {
                    LOG_WARN("Scalability: '{}' does not name a settings section, as Section.Property.", Key.c_str());
                    continue;
                }
                Sections[FString(Key.substr(0, Dot).c_str())][Key.substr(Dot + 1)] = Value;
            }
            return Sections;
        }

        void Write(const FString& Section, Json& Values)
        {
            CClass* Class = FindSettingsClass(Section);
            if (Class == nullptr || Class->GetDefaultObject() == nullptr)
            {
                LOG_WARN("Scalability: no settings class has the section '{}'.", Section.c_str());
                return;
            }

            if (ProjectValues.find(Class) == ProjectValues.end())
            {
                FJsonStructuredArchive::SaveStruct(ProjectValues[Class], Class, Class->GetDefaultObject());
            }
            FJsonStructuredArchive::LoadStruct(Values, Class, Class->GetDefaultObject());
        }
    }

    FString GetGroupName(EScalabilityGroup Group)
    {
        switch (Group)
        {
        case EScalabilityGroup::ViewDistance: return "ViewDistance";
        case EScalabilityGroup::Shadows:      return "Shadows";
        case EScalabilityGroup::Effects:      return "Effects";
        case EScalabilityGroup::PostProcess:  return "PostProcess";
        case EScalabilityGroup::Textures:     return "Textures";
        case EScalabilityGroup::AntiAliasing: return "AntiAliasing";
        }
        return FString();
    }

    void ApplyGroup(EScalabilityGroup Group, EQualityLevel Level)
    {
        const Json& Table = GetTable();
        const auto GroupIt = Table.find(GetGroupName(Group).c_str());
        if (GroupIt == Table.end())
        {
            return;
        }

        // Default hands every property this group can touch back to the project's value.
        if (Level == EQualityLevel::Default || Level == EQualityLevel::Custom)
        {
            Json Restore = Json::object();
            for (const auto& [LevelKey, Values] : GroupIt->items())
            {
                for (const auto& [Key, Value] : BySection(Values))
                {
                    CClass* Class = FindSettingsClass(Key);
                    auto Saved = Class != nullptr ? ProjectValues.find(Class) : ProjectValues.end();
                    if (Saved == ProjectValues.end())
                    {
                        continue;
                    }
                    for (const auto& [Property, Ignored] : Value.items())
                    {
                        if (Saved->second.contains(Property))
                        {
                            Restore[Key.c_str()][Property] = Saved->second[Property];
                        }
                    }
                }
            }
            for (auto& [Section, Values] : Restore.items())
            {
                Write(FString(Section.c_str()), Values);
            }
            return;
        }

        const auto LevelIt = GroupIt->find(LevelName(Level));
        if (LevelIt == GroupIt->end())
        {
            return;
        }

        for (auto& [Section, Values] : BySection(*LevelIt))
        {
            Write(Section, Values);
        }
    }

    void OverrideSetting(FStringView Key, FStringView JsonValue)
    {
        Json Values = Json::object();
        try
        {
            Values[std::string(Key.data(), Key.size())] = Json::parse(std::string(JsonValue.data(), JsonValue.size()));
        }
        catch (const std::exception& Ex)
        {
            LOG_WARN("Scalability: '{}' is not a JSON value for {} ({}).", JsonValue, Key, Ex.what());
            return;
        }

        for (auto& [Section, SectionValues] : BySection(Values))
        {
            Write(Section, SectionValues);
        }
    }

    void RestoreSetting(FStringView Key)
    {
        const std::string Full(Key.data(), Key.size());
        const size_t Dot = Full.find('.');
        if (Dot == std::string::npos)
        {
            return;
        }

        CClass* Class = FindSettingsClass(FStringView(Full.data(), Dot));
        const auto Saved = Class != nullptr ? ProjectValues.find(Class) : ProjectValues.end();
        const std::string Property = Full.substr(Dot + 1);
        if (Saved == ProjectValues.end() || !Saved->second.contains(Property))
        {
            return;
        }

        Json Values = Json::object();
        Values[Property] = Saved->second[Property];
        Write(FString(Full.substr(0, Dot).c_str()), Values);
    }

    void RestoreProjectValues()
    {
        for (auto& [Class, Values] : ProjectValues)
        {
            if (Class->GetDefaultObject() != nullptr)
            {
                FJsonStructuredArchive::LoadStruct(Values, Class, Class->GetDefaultObject());
            }
        }
        ProjectValues.clear();
    }
}
