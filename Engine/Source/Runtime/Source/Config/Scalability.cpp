#include "RuntimePCH.h"
#include "Scalability.h"

#include "Config.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

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
        THashMap<CClass*, FJsonValue> ProjectValues;

        FJsonValue BuildTable()
        {
            FJsonValue Table;
            const bool bEngineTable = FJsonValue::Parse(EngineTable, Table);
            LUMINA_CONTAINER_CHECK(bEngineTable);

            FString Text;
            if (VFS::Exists("/Config/Scalability.json") && VFS::ReadFile(Text, "/Config/Scalability.json"))
            {
                FJsonValue Project;
                FJsonParseError Error;
                if (FJsonValue::Parse(Text.View(), Project, &Error))
                {
                    for (const auto& [Group, Levels] : Project.Members())
                    {
                        Table.Set(Group.View(), Levels);
                    }
                }
                else
                {
                    LOG_WARN("Scalability: /Config/Scalability.json could not be read ({}); using the engine's table.", Error.Message);
                }
            }
            return Table;
        }

        const FJsonValue& GetTable()
        {
            static const FJsonValue Table = BuildTable();
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
        THashMap<FString, FJsonValue> BySection(const FJsonValue& Values)
        {
            THashMap<FString, FJsonValue> Sections;
            for (const auto& [Key, Value] : Values.Members())
            {
                const FStringView Full = Key.View();
                const int32 Dot = Full.Find('.');
                if (Dot == Constants::kIndexNone)
                {
                    LOG_WARN("Scalability: '{}' does not name a settings section, as Section.Property.", Full);
                    continue;
                }
                Sections.FindOrAdd(FString(Full.Left(Dot))).Set(Full.RightChop(Dot + 1), Value);
            }
            return Sections;
        }

        void Write(const FString& Section, FJsonValue& Values)
        {
            CClass* Class = FindSettingsClass(Section);
            if (Class == nullptr || Class->GetDefaultObject() == nullptr)
            {
                LOG_WARN("Scalability: no settings class has the section '{}'.", *Section);
                return;
            }

            if (!ProjectValues.Contains(Class))
            {
                FJsonStructuredArchive::SaveStruct(ProjectValues.Add(Class), Class, Class->GetDefaultObject());
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
        const FJsonValue& Table = GetTable();
        const FJsonValue* GroupIt = Table.Find(GetGroupName(Group).View());
        if (GroupIt == nullptr)
        {
            return;
        }

        // Default hands every property this group can touch back to the project's value.
        if (Level == EQualityLevel::Default || Level == EQualityLevel::Custom)
        {
            FJsonValue Restore = FJsonValue::MakeObject();
            for (const auto& [LevelKey, Values] : GroupIt->Members())
            {
                for (const auto& [Key, Value] : BySection(Values))
                {
                    CClass* Class = FindSettingsClass(Key);
                    const FJsonValue* Saved = Class != nullptr ? ProjectValues.Find(Class) : nullptr;
                    if (Saved == nullptr)
                    {
                        continue;
                    }
                    for (const auto& [Property, Ignored] : Value.Members())
                    {
                        if (const FJsonValue* Original = Saved->Find(Property.View()))
                        {
                            Restore.FindOrAdd(Key.View()).Set(Property.View(), *Original);
                        }
                    }
                }
            }
            for (auto& [Section, Values] : Restore.Members())
            {
                Write(Section.ToString(), Values);
            }
            return;
        }

        const FJsonValue* LevelIt = GroupIt->Find(LevelName(Level));
        if (LevelIt == nullptr)
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
        FJsonValue Parsed;
        FJsonParseError Error;
        if (!FJsonValue::Parse(JsonValue, Parsed, &Error))
        {
            LOG_WARN("Scalability: '{}' is not a JSON value for {} ({}).", JsonValue, Key, Error.Message);
            return;
        }
        FJsonValue Values = FJsonValue::MakeObject();
        Values.Set(Key, Move(Parsed));

        for (auto& [Section, SectionValues] : BySection(Values))
        {
            Write(Section, SectionValues);
        }
    }

    void RestoreSetting(FStringView Key)
    {
        const int32 Dot = Key.Find('.');
        if (Dot == Constants::kIndexNone)
        {
            return;
        }

        CClass* Class = FindSettingsClass(Key.Left(Dot));
        const FJsonValue* const Saved = Class != nullptr ? ProjectValues.Find(Class) : nullptr;
        const FStringView Property = Key.RightChop(Dot + 1);
        const FJsonValue* const Original = Saved != nullptr ? Saved->Find(Property) : nullptr;
        if (Original == nullptr)
        {
            return;
        }

        FJsonValue Values = FJsonValue::MakeObject();
        Values.Set(Property, *Original);
        Write(FString(Key.Left(Dot)), Values);
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
        ProjectValues.Reset();
    }
}
