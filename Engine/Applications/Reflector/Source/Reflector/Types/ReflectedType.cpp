#include "ReflectedType.h"

#include <algorithm>
#include <array>
#include <ranges>

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/Properties/ReflectedProperty.h"

namespace Lumina::Reflection
{
    void EmitMetadataArray(FCodeWriter& Writer, std::string_view SymbolBase, const std::vector<FMetadataPair>& Metadata)
    {
        if (Metadata.empty())
        {
            return;
        }

        Writer.Linef("static constexpr Lumina::FMetaDataPairParam %.*s_Metadata[] = {",
            static_cast<int>(SymbolBase.size()), SymbolBase.data());

        for (const FMetadataPair& Pair : Metadata)
        {
            Writer.Linef("\t{ \"%s\", \"%s\" },", Pair.Key.c_str(), Pair.Value.c_str());
        }

        Writer.Line("};");
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
                { "FEntity",                  EPropertyTypeFlags::Int32           },
                { "ECS::FEntity",             EPropertyTypeFlags::Int32           },
                { "Lumina::FEntity",          EPropertyTypeFlags::Int32           },
                { "Lumina::ECS::FEntity",     EPropertyTypeFlags::Int32           },
                { "Lumina::CClass",           EPropertyTypeFlags::Class           },
                { "Lumina::FName",            EPropertyTypeFlags::Name            },
                { "Lumina::FString",          EPropertyTypeFlags::String          },
                { "Lumina::FFixedString",     EPropertyTypeFlags::String          },
                { "Lumina::TVector",          EPropertyTypeFlags::Vector          },
                { "Lumina::TFixedVector",     EPropertyTypeFlags::Vector          },
                { "Lumina::THashMap",         EPropertyTypeFlags::Map             },
                { "Lumina::TOptional",        EPropertyTypeFlags::Optional        },
                { "Lumina::TObjectPtr",       EPropertyTypeFlags::Object          },
                { "Lumina::TWeakObjectPtr",   EPropertyTypeFlags::Object          },
                { "Lumina::CObject",          EPropertyTypeFlags::Object          },
                { "Lumina::TSubclassOf",      EPropertyTypeFlags::Class           },
                { "Lumina::TSubStructOf",     EPropertyTypeFlags::SubStruct       },
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
        for (const FMetadataPair& Pair : Metadata)
        {
            if (Pair.Key == Key)
            {
                return &Pair.Value;
            }
        }
        return nullptr;
    }

    bool FReflectedType::HasMetadata(std::string_view Meta) const
    {
        return std::any_of(Metadata.begin(), Metadata.end(),
            [&](const FMetadataPair& Pair) { return Pair.Key == Meta; });
    }

    void FReflectedType::GenerateMetadata(const std::string& InMetadata)
    {
        FMetadataParser Parser(InMetadata);
        Metadata = std::move(Parser.Metadata);
    }

    bool FReflectedType::DeclareAccessors(FCodeWriter& Writer, const std::string& FileID)
    {
        const bool bHasAnyAccessor = std::any_of(Props.begin(), Props.end(),
            [](const auto& Prop) { return Prop->HasAccessors(); });

        if (!bHasAnyAccessor)
        {
            return false;
        }

        Writer.Linef("#define %s_%u_ACCESSORS \\", FileID.c_str(), GeneratedBodyLineNumber);

        for (const auto& Prop : Props)
        {
            Prop->DeclareAccessors(Writer, FileID);
        }

        // The last Macro line still ends with a line continuation, so strip it to close the define cleanly.
        Writer.FinalizeMacro();
        Writer.Line();

        return true;
    }
}
