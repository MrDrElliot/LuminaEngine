#include "RuntimePCH.h"
#include "PluginDescriptor.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Platform/Filesystem/FileHelper.h"

#include "Core/Json/Json.h"

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

    bool TryParsePluginModuleType(FStringView Str, EPluginModuleType& OutType)
    {
        for (const EPluginModuleType Type : { EPluginModuleType::Runtime, EPluginModuleType::Editor, EPluginModuleType::Developer })
        {
            if (Str == LexToString(Type))
            {
                OutType = Type;
                return true;
            }
        }
        return false;
    }

    namespace PluginDescriptorInternal
    {
        // A missing field takes its default, but a present one of the wrong type fails, which is what the build tool does.
        class FFieldReader
        {
        public:

            explicit FFieldReader(FString& InError) : Error(InError) {}

            NODISCARD bool Failed() const { return bFailed; }

            FString String(const FJsonValue& Object, const char* Key, const char* Default = "")
            {
                const FJsonValue* Field = Find(Object, Key, &FJsonValue::IsString, "a string");
                if (Field == nullptr)
                {
                    return FString(Default);
                }
                return FString(Field->AsString());
            }

            int32 Int(const FJsonValue& Object, const char* Key, int32 Default)
            {
                const FJsonValue* Field = Find(Object, Key, &FJsonValue::IsInteger, "an integer");
                if (Field == nullptr)
                {
                    return Default;
                }
                const int64 Value = Field->AsInt64();
                if (Value < TNumericLimits<int32>::Min() || Value > TNumericLimits<int32>::Max())
                {
                    Fail(Key, "an integer that fits 32 bits");
                    return Default;
                }
                return static_cast<int32>(Value);
            }

            bool Bool(const FJsonValue& Object, const char* Key, bool Default)
            {
                const FJsonValue* Field = Find(Object, Key, &FJsonValue::IsBool, "a boolean");
                return Field != nullptr ? Field->AsBool() : Default;
            }

            TVector<FString> StringArray(const FJsonValue& Object, const char* Key)
            {
                TVector<FString> Out;
                const FJsonValue* Field = Find(Object, Key, &FJsonValue::IsArray, "an array");
                if (Field == nullptr)
                {
                    return Out;
                }
                Out.Reserve(Field->Num());
                for (const FJsonValue& Value : Field->AsArray())
                {
                    if (!Value.IsString())
                    {
                        Fail(Key, "an array of strings");
                        return Out;
                    }
                    Out.Emplace(Value.AsString());
                }
                return Out;
            }

            const FJsonValue* Array(const FJsonValue& Object, const char* Key)
            {
                return Find(Object, Key, &FJsonValue::IsArray, "an array");
            }

            void Fail(const char* Key, const char* Expected)
            {
                if (!bFailed)
                {
                    Error = Format("Plugin descriptor field '{}' must be {}.", Key, Expected);
                    bFailed = true;
                }
            }

            void FailWith(FString Message)
            {
                if (!bFailed)
                {
                    Error = Move(Message);
                    bFailed = true;
                }
            }

        private:

            const FJsonValue* Find(const FJsonValue& Object, const char* Key, bool (FJsonValue::*IsType)() const noexcept, const char* Expected)
            {
                const FJsonValue* Field = Object.Find(Key);
                if (Field == nullptr)
                {
                    return nullptr;
                }
                if (!(Field->*IsType)())
                {
                    Fail(Key, Expected);
                    return nullptr;
                }
                return Field;
            }

            FString& Error;
            bool     bFailed = false;
        };
    }

    bool FPluginDescriptor::LoadFromFile(FStringView FilePath, FPluginDescriptor& OutDescriptor, FString& OutError)
    {
        FString Json;
        if (!FileHelper::LoadFileIntoString(Json, FilePath))
        {
            OutError = "Failed to read plugin descriptor file: ";
            OutError.Append(FilePath.GetData(), FilePath.Len());
            return false;
        }
        return LoadFromString(Json, OutDescriptor, OutError);
    }

    // The schema, defaults and errors here match LuminaBuildTool's PluginDescriptor, so a descriptor builds only if it loads.
    bool FPluginDescriptor::LoadFromString(FStringView JsonText, FPluginDescriptor& OutDescriptor, FString& OutError)
    {
        using namespace PluginDescriptorInternal;

        FJsonValue J;
        FJsonParseError ParseError;
        if (!FJsonValue::Parse(JsonText, J, &ParseError))
        {
            OutError = "Malformed plugin descriptor JSON: " + ParseError.Message;
            return false;
        }

        if (!J.IsObject())
        {
            OutError = "Plugin descriptor root must be an object.";
            return false;
        }

        FFieldReader Read(OutError);

        OutDescriptor.FormatVersion      = Read.Int(J, "FormatVersion", 1);
        OutDescriptor.Name               = Read.String(J, "Name");
        OutDescriptor.Version            = Read.Int(J, "Version", 1);
        OutDescriptor.VersionName        = Read.String(J, "VersionName");
        OutDescriptor.Author             = Read.String(J, "Author");
        OutDescriptor.Description        = Read.String(J, "Description");
        OutDescriptor.Category           = Read.String(J, "Category");
        OutDescriptor.bEnabledByDefault  = Read.Bool(J, "EnabledByDefault", true);
        OutDescriptor.bEditorOnly        = Read.Bool(J, "EditorOnly", false);
        OutDescriptor.bContainsContent   = Read.Bool(J, "ContainsContent", true);
        OutDescriptor.SupportedPlatforms = Read.StringArray(J, "SupportedPlatforms");

        if (!Read.Failed() && OutDescriptor.Name.IsEmpty())
        {
            Read.FailWith("Plugin descriptor missing required field 'Name'.");
        }

        if (const FJsonValue* Dependencies = Read.Array(J, "Dependencies"))
        {
            OutDescriptor.Dependencies.Reserve(Dependencies->Num());
            for (const FJsonValue& Entry : Dependencies->AsArray())
            {
                FPluginDependency Dependency;
                if (Entry.IsString())
                {
                    Dependency.Name = FString(Entry.AsString());
                }
                else if (Entry.IsObject())
                {
                    Dependency.Name      = Read.String(Entry, "Name");
                    Dependency.Version   = Read.Int(Entry, "Version", 0);
                    Dependency.bOptional = Read.Bool(Entry, "Optional", false);
                }
                else
                {
                    Read.FailWith("A dependency must be a plugin name or an object with a Name.");
                    break;
                }

                if (Dependency.Name.IsEmpty())
                {
                    Read.FailWith(Format("Plugin '{}' has a dependency with no Name.", OutDescriptor.Name));
                    break;
                }
                OutDescriptor.Dependencies.Emplace(Move(Dependency));
            }
        }

        // Cook roots are read here and carried into the engine, and the cooker iterates them.
        if (const FJsonValue* Roots = Read.Array(J, "CookRoots"))
        {
            OutDescriptor.CookRoots.Reserve(Roots->Num());
            for (const FJsonValue& Entry : Roots->AsArray())
            {
                FCookRoot Root;
                if (Entry.IsString())
                {
                    // The shorthand form, where a bare string is the asset path.
                    Root.Asset = Entry.AsString();
                }
                else if (Entry.IsObject())
                {
                    Root.Asset = Read.String(Entry, "Asset");
                    if (Root.Asset.IsEmpty())
                    {
                        LOG_WARN("[PluginManager] Plugin '{}' CookRoots entry has no 'Asset' field; skipping",
                            OutDescriptor.Name);
                    }
                    Root.Chunk = FName(*Read.String(Entry, "Chunk"));
                }
                if (!Root.Asset.IsEmpty())
                {
                    OutDescriptor.CookRoots.Emplace(Move(Root));
                }
            }
        }

        if (const FJsonValue* Modules = Read.Array(J, "Modules"))
        {
            OutDescriptor.Modules.Reserve(Modules->Num());
            for (const FJsonValue& Entry : Modules->AsArray())
            {
                if (!Entry.IsObject())
                {
                    Read.FailWith(Format("Plugin '{}' has a module entry that is not an object.", OutDescriptor.Name));
                    break;
                }

                FPluginModuleDescriptor Module;
                Module.Name               = Read.String(Entry, "Name");
                Module.SupportedPlatforms = Read.StringArray(Entry, "SupportedPlatforms");
                const FString Type        = Read.String(Entry, "Type", "Runtime");
                const FString Phase       = Read.String(Entry, "LoadingPhase", "PreEngineInit");
                if (Read.Failed())
                {
                    break;
                }

                if (Module.Name.IsEmpty())
                {
                    Read.FailWith(Format("Plugin '{}' has a module with no Name.", OutDescriptor.Name));
                    break;
                }
                if (!TryParsePluginModuleType(Type, Module.Type))
                {
                    Read.FailWith(Format("Plugin '{}' module '{}' has unknown Type '{}'; expected Runtime, Editor or Developer.",
                        OutDescriptor.Name, Module.Name, Type));
                    break;
                }
                if (!TryParsePluginLoadingPhase(Phase, Module.LoadingPhase))
                {
                    Read.FailWith(Format("Plugin '{}' module '{}' has unknown LoadingPhase '{}'.", OutDescriptor.Name, Module.Name, Phase));
                    break;
                }
                OutDescriptor.Modules.Emplace(Move(Module));
            }
        }

        return !Read.Failed();
    }
}
