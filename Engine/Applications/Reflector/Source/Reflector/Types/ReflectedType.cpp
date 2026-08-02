#include "ReflectedType.h"

#include <EASTL/algorithm.h>

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/Properties/ReflectedProperty.h"

namespace Lumina::Reflection
{
    void EmitMetadataArray(FCodeWriter& Writer, eastl::string_view SymbolBase, const eastl::vector<FMetadataPair>& Metadata)
    {
        if (Metadata.empty())
        {
            return;
        }

        Writer.Linef("static constexpr Lumina::FMetaDataPairParam %s_Metadata[] = {",
            eastl::string(SymbolBase).c_str());

        for (const FMetadataPair& Pair : Metadata)
        {
            Writer.Linef("\t{ \"%s\", \"%s\" },", Pair.Key.c_str(), Pair.Value.c_str());
        }

        Writer.Line("};");
    }

    namespace
    {
        constexpr uint32_t Fnv1aLike(const char* Str)
        {
            uint32_t Hash = 5381;
            while (*Str)
            {
                Hash = ((Hash << 5) + Hash) + static_cast<unsigned char>(*Str++);
            }
            return Hash;
        }
    }

    EPropertyTypeFlags GetCoreTypeFromName(const char* Name)
    {
        if (Name == nullptr)
        {
            return EPropertyTypeFlags::None;
        }

        switch (Fnv1aLike(Name))
        {
            case Fnv1aLike("bool"):                     return EPropertyTypeFlags::Bool;
            case Fnv1aLike("uint8"):                    return EPropertyTypeFlags::UInt8;
            case Fnv1aLike("uint16"):                   return EPropertyTypeFlags::UInt16;
            case Fnv1aLike("uint32"):                   return EPropertyTypeFlags::UInt32;
            case Fnv1aLike("uint64"):                   return EPropertyTypeFlags::UInt64;
            case Fnv1aLike("int8"):                     return EPropertyTypeFlags::Int8;
            case Fnv1aLike("int16"):                    return EPropertyTypeFlags::Int16;
            case Fnv1aLike("int32"):                    return EPropertyTypeFlags::Int32;
            case Fnv1aLike("int64"):                    return EPropertyTypeFlags::Int64;
            case Fnv1aLike("float"):                    return EPropertyTypeFlags::Float;
            case Fnv1aLike("double"):                   return EPropertyTypeFlags::Double;
            case Fnv1aLike("entt::entity"):             return EPropertyTypeFlags::Int32;
            case Fnv1aLike("Lumina::FEntity"):          return EPropertyTypeFlags::Int32;
            case Fnv1aLike("FEntity"):                  return EPropertyTypeFlags::Int32;
            case Fnv1aLike("Lumina::CClass"):           return EPropertyTypeFlags::Class;
            case Fnv1aLike("Lumina::FName"):            return EPropertyTypeFlags::Name;
            case Fnv1aLike("Lumina::FString"):          return EPropertyTypeFlags::String;
            case Fnv1aLike("Lumina::FFixedString"):     return EPropertyTypeFlags::String;
            case Fnv1aLike("Lumina::TVector"):          return EPropertyTypeFlags::Vector;
            case Fnv1aLike("Lumina::TFixedVector"):     return EPropertyTypeFlags::Vector;
            case Fnv1aLike("Lumina::THashMap"):         return EPropertyTypeFlags::Map;
            case Fnv1aLike("Lumina::TOptional"):        return EPropertyTypeFlags::Optional;
            case Fnv1aLike("Lumina::TObjectPtr"):       return EPropertyTypeFlags::Object;
            case Fnv1aLike("Lumina::TWeakObjectPtr"):   return EPropertyTypeFlags::Object;
            case Fnv1aLike("Lumina::CObject"):          return EPropertyTypeFlags::Object;
            case Fnv1aLike("Lumina::TSubclassOf"):      return EPropertyTypeFlags::Class;
            case Fnv1aLike("Lumina::TSubStructOf"):     return EPropertyTypeFlags::SubStruct;
            case Fnv1aLike("Lumina::TInstancedStruct"): return EPropertyTypeFlags::InstancedStruct;
            case Fnv1aLike("Lumina::FInstancedStruct"): return EPropertyTypeFlags::InstancedStruct;
            case Fnv1aLike("Lumina::TSoftObjectPtr"):   return EPropertyTypeFlags::SoftObject;
            case Fnv1aLike("Lumina::FSoftObjectPath"):  return EPropertyTypeFlags::SoftObject;
            case Fnv1aLike("Lumina::TScriptDelegate"):  return EPropertyTypeFlags::Delegate;
            case Fnv1aLike("Lumina::FScriptDelegate"):  return EPropertyTypeFlags::Delegate;
            default:                                       return EPropertyTypeFlags::None;
        }
    }
    
    bool FReflectedType::HasMetadata(const eastl::string& Meta) const
    {
        return eastl::any_of(Metadata.begin(), Metadata.end(),
            [&](const FMetadataPair& Pair) { return Pair.Key == Meta; });
    }

    void FReflectedType::GenerateMetadata(const eastl::string& InMetadata)
    {
        FMetadataParser Parser(InMetadata);
        Metadata = eastl::move(Parser.Metadata);
    }

    bool FReflectedType::DeclareAccessors(FCodeWriter& Writer, const eastl::string& FileID)
    {
        const bool bHasAnyAccessor = eastl::any_of(Props.begin(), Props.end(),
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

        // Close the #define cleanly: the last Macro line still ends with " \\\n", strip it.
        Writer.FinalizeMacro();
        Writer.Line();

        return true;
    }
}
