#include "ReflectedType.h"

#include <algorithm>
#include <array>
#include <ranges>

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"
#include "Reflector/Types/Properties/ReflectedProperty.h"

namespace Lumina::Reflection
{
    namespace
    {
        // Entries every build keeps, then the editor's, which a build without the editor counts off the end.
        std::vector<const FMetadataPair*> StoredMetadata(const std::vector<FMetadataPair>& Metadata, ESpecifierTarget Target, size_t& OutNumRuntime)
        {
            std::vector<const FMetadataPair*> Stored;
            for (const ESpecifierStorage Wanted : { ESpecifierStorage::Metadata, ESpecifierStorage::EditorMetadata })
            {
                for (const FMetadataPair& Pair : Metadata)
                {
                    if (!Pair.Key.empty() && GetSpecifierStorage(Target, Pair.Key) == Wanted)
                    {
                        Stored.push_back(&Pair);
                    }
                }
                if (Wanted == ESpecifierStorage::Metadata)
                {
                    OutNumRuntime = Stored.size();
                }
            }
            return Stored;
        }
    }

    void EmitMetadataArray(FCodeWriter& Writer, std::string_view SymbolBase, const std::vector<FMetadataPair>& Metadata, ESpecifierTarget Target)
    {
        size_t NumRuntime = 0;
        const std::vector<const FMetadataPair*> Stored = StoredMetadata(Metadata, Target, NumRuntime);
        if (Stored.empty())
        {
            return;
        }

        Writer.Linef("static constexpr Lumina::FMetadataEntry %.*s_Metadata[] = {",
            static_cast<int>(SymbolBase.size()), SymbolBase.data());

        for (const FMetadataPair* Pair : Stored)
        {
            Writer.Linef("\t{ \"%s\", \"%s\" },", Pair->Key.c_str(), Pair->Value.c_str());
        }

        Writer.Line("};");
    }

    std::string MetadataField(std::string_view SymbolBase, const std::vector<FMetadataPair>& Metadata, ESpecifierTarget Target)
    {
        size_t NumRuntime = 0;
        if (StoredMetadata(Metadata, Target, NumRuntime).empty())
        {
            return {};
        }
        return " LUMINA_METADATA(" + std::string(SymbolBase) + "_Metadata, " + std::to_string(NumRuntime) + ")";
    }

    std::string FReflectedType::GetTypeFunctionName() const
    {
        return "GetType_" + Names::FriendlyFromParts(Namespace, DisplayName);
    }

    const char* FReflectedType::AccessorName() const
    {
        switch (Type)
        {
        case EType::Class: return "StaticClass";
        case EType::Enum:  return "StaticEnum";
        default:           return "StaticStruct";
        }
    }

    void FReflectedType::OpenNamespace(FCodeWriter& Writer) const
    {
        if (!Namespace.empty())
        {
            Writer.Linef("namespace %s", Namespace.c_str());
            Writer.Line("{");
        }
    }

    void FReflectedType::CloseNamespace(FCodeWriter& Writer) const
    {
        if (!Namespace.empty())
        {
            Writer.Line("}");
        }
        Writer.Line();
    }

    void FReflectedType::EmitTypeFunctions(FCodeWriter& Writer, const std::string& ExtraConstruction) const
    {
        const std::string Kind = GetTypeName();
        const std::string Api = Names::ProjectApiMacro(Header->Project->Name);
        const std::string Function = GetTypeFunctionName();

        Writer.Line("namespace Lumina::Generated");
        Writer.Line("{");
        Writer.Linef("	Lumina::%s* %s()", Kind.c_str(), Function.c_str());
        Writer.Line("	{");
        Writer.Linef("		static Lumina::%s* Type = nullptr;", Kind.c_str());
        Writer.Line("		if (Type == nullptr)");
        Writer.Line("		{");
        Writer.Linef("			Lumina::Construct%s(&Type, %s::Desc);", Kind.c_str(), TypeInfoQualifiedName().c_str());
        if (!ExtraConstruction.empty())
        {
            Writer.Linef("			%s", ExtraConstruction.c_str());
        }
        Writer.Line("		}");
        Writer.Line("		return Type;");
        Writer.Line("	}");
        Writer.Line("}");
        Writer.Line();

        Writer.Linef("template<> %s Lumina::%s* Lumina::%s<%s>()", Api.c_str(), Kind.c_str(), AccessorName(), EmittedCppQualifiedName().c_str());
        Writer.Line("{");
        Writer.Linef("	return Lumina::Generated::%s();", Function.c_str());
        Writer.Line("}");
        Writer.Line();
    }

    namespace
    {
        struct FCoreTypeEntry
        {
            std::string_view   Name;
            EPropertyTypeFlags Flags;
        };

        // Sorted during constant evaluation, so the lookup below is a binary search over a read-only table.
        constexpr auto GCoreTypes = []
        {
            std::array Entries = std::to_array<FCoreTypeEntry>(
            {
                { "bool",                     EPropertyTypeFlags::Bool            },
                { "uint8",                    EPropertyTypeFlags::UInt8           },
                { "uint16",                   EPropertyTypeFlags::UInt16          },
                { "uint32",                   EPropertyTypeFlags::UInt32          },
                { "uint64",                   EPropertyTypeFlags::UInt64          },
                { "int8",                     EPropertyTypeFlags::Int8            },
                { "int16",                    EPropertyTypeFlags::Int16           },
                { "int32",                    EPropertyTypeFlags::Int32           },
                { "int64",                    EPropertyTypeFlags::Int64           },
                { "float",                    EPropertyTypeFlags::Float           },
                { "double",                   EPropertyTypeFlags::Double          },
                { "FEntity",                  EPropertyTypeFlags::Entity          },
                { "ECS::FEntity",             EPropertyTypeFlags::Entity          },
                { "Lumina::FEntity",          EPropertyTypeFlags::Entity          },
                { "Lumina::ECS::FEntity",     EPropertyTypeFlags::Entity          },
                { "Lumina::CClass",           EPropertyTypeFlags::Class           },
                { "Lumina::FName",            EPropertyTypeFlags::Name            },
                { "Lumina::FString",          EPropertyTypeFlags::String          },
                { "Lumina::FFixedString",     EPropertyTypeFlags::String          },
                { "Lumina::TVector",          EPropertyTypeFlags::Vector          },
                { "Lumina::TInlineVector",    EPropertyTypeFlags::Vector          },
                { "Lumina::THashMap",         EPropertyTypeFlags::Map             },
                { "Lumina::TOptional",        EPropertyTypeFlags::Optional        },
                { "Lumina::TStrongObjectPtr", EPropertyTypeFlags::Object          },
                { "Lumina::TWeakObjectPtr",   EPropertyTypeFlags::Object          },
                { "Lumina::CObject",          EPropertyTypeFlags::Object          },
                { "Lumina::TSubclassOf",      EPropertyTypeFlags::Class           },
                { "Lumina::TSubStructOf",     EPropertyTypeFlags::SubStruct       },
                { "Lumina::TComponentRef",    EPropertyTypeFlags::ComponentRef    },
                { "Lumina::TInstancedStruct", EPropertyTypeFlags::InstancedStruct },
                { "Lumina::FInstancedStruct", EPropertyTypeFlags::InstancedStruct },
                { "Lumina::TSoftObjectPtr",   EPropertyTypeFlags::SoftObject      },
                { "Lumina::FSoftObjectPath",  EPropertyTypeFlags::SoftObject      },
                { "Lumina::TScriptDelegate",  EPropertyTypeFlags::Delegate        },
                { "Lumina::FScriptDelegate",  EPropertyTypeFlags::Delegate        },
            });

            std::ranges::sort(Entries, {}, &FCoreTypeEntry::Name);
            return Entries;
        }();

        // A duplicate row would make one of the two spellings unreachable through the binary search.
        static_assert(std::ranges::adjacent_find(GCoreTypes, {}, &FCoreTypeEntry::Name) == GCoreTypes.end(),
            "Core type names must be unique");
    }

    EPropertyTypeFlags GetCoreTypeFromName(std::string_view Name)
    {
        const auto Found = std::ranges::lower_bound(GCoreTypes, Name, {}, &FCoreTypeEntry::Name);
        return Found != GCoreTypes.end() && Found->Name == Name ? Found->Flags : EPropertyTypeFlags::None;
    }

    const std::string* FReflectedType::TryGetMetadata(std::string_view Key) const
    {
        return FindMetadata(Metadata, Key);
    }

    bool FReflectedType::HasMetadata(std::string_view Meta) const
    {
        return FindMetadata(Metadata, Meta) != nullptr;
    }

    void FReflectedType::GenerateMetadata(const std::string& InMetadata)
    {
        FMetadataParser Parser(InMetadata);
        Metadata = std::move(Parser.Metadata);
    }
}
