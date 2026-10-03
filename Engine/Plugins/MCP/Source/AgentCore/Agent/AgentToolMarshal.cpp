#include "AgentCorePCH.h"
#include "Agent/AgentToolMarshal.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentReflectionUtils.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/StringFormat.h"
#include "Containers/Vector.h"
#include "Core/Templates/NumericLimits.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"
#include "Memory/Memory.h"

namespace Lumina::Agent
{
    namespace
    {
        FString JoinPath(FStringView Path, FStringView Leaf)
        {
            return Path.empty() ? FString(Leaf.data(), Leaf.size()) : Lumina::Format("{}.{}", Path, Leaf);
        }

        bool CanonicalizeProperty(FProperty* Property, nlohmann::json& Value, FStringView Path, FString& OutError);

        // Rewrites the shorthand forms into the archive's own shape, so validation and apply see one format.
        bool CanonicalizeStruct(CStruct* Struct, nlohmann::json& Value, FStringView Path, FString& OutError)
        {
            if (Struct == nullptr || !Value.is_object())
            {
                return true;
            }

            for (CStruct* Current : Detail::CollectStructChain(Struct))
            {
                bool bOk = true;

                Current->ForEachProperty<FProperty>([&](FProperty* Property)
                {
                    if (!bOk || Property == nullptr)
                    {
                        return;
                    }

                    const FString Name(Property->GetPropertyName().ToString().c_str());

                    const auto Found = Value.find(Detail::ToStandard(FStringView(Name)));
                    if (Found == Value.end())
                    {
                        return;
                    }

                    bOk = CanonicalizeProperty(Property, *Found, FStringView(JoinPath(Path, FStringView(Name))), OutError);
                });

                if (!bOk)
                {
                    return false;
                }
            }

            return true;
        }

        // A map given as an object needs each key turned back into the key property's JSON form.
        bool CanonicalizeMapKey(FProperty* KeyProperty, const std::string& Key, nlohmann::json& OutKey,
            FStringView Path, FString& OutError)
        {
            switch (KeyProperty->GetType())
            {
            case EPropertyTypeFlags::String:
            case EPropertyTypeFlags::Name:
            case EPropertyTypeFlags::Enum:
                OutKey = Key;
                return true;

            case EPropertyTypeFlags::Int8:
            case EPropertyTypeFlags::Int16:
            case EPropertyTypeFlags::Int32:
            case EPropertyTypeFlags::Int64:
            case EPropertyTypeFlags::UInt8:
            case EPropertyTypeFlags::UInt16:
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::UInt64:
            case EPropertyTypeFlags::Entity:
                {
                    char* End = nullptr;
                    const long long Parsed = std::strtoll(Key.c_str(), &End, 10);
                    if (End == Key.c_str() || *End != '\0')
                    {
                        OutError = Lumina::Format("'{}' has key '{}', which is not a whole number.", Path, Key);
                        return false;
                    }

                    OutKey = Parsed;
                    return true;
                }

            default:
                OutError = Lumina::Format("'{}' as an object needs string or integer keys; use the [key, value, ...] form.", Path);
                return false;
            }
        }

        bool CanonicalizeProperty(FProperty* Property, nlohmann::json& Value, FStringView Path, FString& OutError)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::SoftObject:
                {
                    // A GUID is what every other tool hands back, so accept it and store the path it names.
                    if (!Value.is_string())
                    {
                        return true;
                    }

                    const std::string Given = Value.get<std::string>();
                    const TOptional<FGuid> Guid = FGuid::TryParse(FStringView(Given.c_str()));
                    if (!Guid.IsSet())
                    {
                        return true;
                    }

                    const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(*Guid);
                    if (Data == nullptr)
                    {
                        OutError = Lumina::Format("'{}' names no asset with GUID {}.", Path, Given);
                        return false;
                    }

                    Value = Detail::ToStandard(FStringView(Data->Path.c_str()));
                    return true;
                }

            case EPropertyTypeFlags::Optional:
                {
                    FProperty* Inner = static_cast<FOptionalProperty*>(Property)->GetInternalProperty();

                    if (Value.is_null())
                    {
                        Value = nlohmann::json::object({ { "Engaged", false } });
                        return true;
                    }

                    const bool bCanonical = Value.is_object() && Value.contains("Engaged") && Value["Engaged"].is_boolean()
                        && Value.size() == (Value.contains("Value") ? 2u : 1u);

                    if (!bCanonical)
                    {
                        Value = nlohmann::json::object({ { "Engaged", true }, { "Value", Value } });
                    }

                    if (Inner != nullptr && Value.contains("Value"))
                    {
                        return CanonicalizeProperty(Inner, Value["Value"], FStringView(JoinPath(Path, "Value")), OutError);
                    }

                    return true;
                }

            case EPropertyTypeFlags::Map:
                {
                    FMapProperty* Map = static_cast<FMapProperty*>(Property);
                    FProperty* KeyProperty = Map->GetKeyProperty();
                    FProperty* ValueProperty = Map->GetValueProperty();

                    if (KeyProperty == nullptr || ValueProperty == nullptr)
                    {
                        return true;
                    }

                    if (Value.is_object())
                    {
                        nlohmann::json Flat = nlohmann::json::array();
                        for (auto& Entry : Value.items())
                        {
                            nlohmann::json Key;
                            if (!CanonicalizeMapKey(KeyProperty, Entry.key(), Key, Path, OutError))
                            {
                                return false;
                            }

                            Flat.push_back(Move(Key));
                            Flat.push_back(Entry.value());
                        }

                        Value = Move(Flat);
                    }

                    if (!Value.is_array())
                    {
                        return true;
                    }

                    for (size_t Index = 0; Index < Value.size(); ++Index)
                    {
                        FProperty* Element = Index % 2 == 0 ? KeyProperty : ValueProperty;
                        const FString ElementPath = Lumina::Format("{}[{}]", Path, Index);
                        if (!CanonicalizeProperty(Element, Value[Index], FStringView(ElementPath), OutError))
                        {
                            return false;
                        }
                    }

                    return true;
                }

            case EPropertyTypeFlags::InstancedStruct:
                {
                    if (!Value.is_object() || !Value.contains("StructType") || !Value["StructType"].is_string()
                        || !Value.contains("Data"))
                    {
                        return true;
                    }

                    const std::string Key = Value["StructType"].get<std::string>();
                    CStruct* Type = ResolveInstancedStructType(
                        static_cast<FInstancedStructProperty*>(Property)->GetMetaStruct(), FName(Key.c_str()));

                    return Type == nullptr
                        || CanonicalizeStruct(Type, Value["Data"], FStringView(JoinPath(Path, "Data")), OutError);
                }

            case EPropertyTypeFlags::Struct:
                return CanonicalizeStruct(static_cast<FStructProperty*>(Property)->GetStruct(), Value, Path, OutError);

            case EPropertyTypeFlags::Vector:
                {
                    FProperty* Inner = static_cast<FArrayProperty*>(Property)->GetInternalProperty();
                    if (Inner == nullptr || !Value.is_array())
                    {
                        return true;
                    }

                    for (size_t Index = 0; Index < Value.size(); ++Index)
                    {
                        const FString ElementPath = Lumina::Format("{}[{}]", Path, Index);
                        if (!CanonicalizeProperty(Inner, Value[Index], FStringView(ElementPath), OutError))
                        {
                            return false;
                        }
                    }

                    return true;
                }

            default:
                return true;
            }
        }

        bool ValidateProperty(FProperty* Property, const nlohmann::json& Value, FStringView Path, FString& OutError);

        bool ValidateStruct(CStruct* Struct, const nlohmann::json& Value, FStringView Path, FString& OutError)
        {
            if (Struct == nullptr)
            {
                OutError = Lumina::Format("'{}' has no reflected type.", Path);
                return false;
            }

            if (!Value.is_object())
            {
                OutError = Path.empty()
                    ? FString("The arguments have to be a JSON object.")
                    : Lumina::Format("'{}' has to be an object.", Path);
                return false;
            }

            TVector<FProperty*> Properties;
            for (CStruct* Current : Detail::CollectStructChain(Struct))
            {
                Current->ForEachProperty<FProperty>([&Properties](FProperty* Property)
                {
                    if (Property != nullptr)
                    {
                        Properties.push_back(Property);
                    }
                });
            }

            // A misspelled name would otherwise be dropped, leaving the caller sure it had been applied.
            for (const auto& Entry : Value.items())
            {
                bool bKnown = false;
                for (FProperty* Property : Properties)
                {
                    bKnown = bKnown || Property->GetPropertyName().ToString().c_str() == Entry.key();
                }

                if (!bKnown)
                {
                    OutError = Lumina::Format("'{}' is not a field of {}.",
                        JoinPath(Path, FStringView(Entry.key().c_str())), Struct->GetName());
                    return false;
                }
            }

            for (FProperty* Property : Properties)
            {
                const FString Name(Property->GetPropertyName().ToString().c_str());

                const auto Found = Value.find(Detail::ToStandard(FStringView(Name)));
                if (Found == Value.end())
                {
                    // An absent field keeps whatever the freshly constructed struct already holds.
                    continue;
                }

                if (!ValidateProperty(Property, *Found, FStringView(JoinPath(Path, FStringView(Name))), OutError))
                {
                    return false;
                }
            }

            return true;
        }

        // A value past the field's width would otherwise be stored truncated, so 300 in an int8 lands as 44.
        template<typename T>
        bool ValidateInteger(const nlohmann::json& Value, FStringView Path, FString& OutError)
        {
            const bool bFits = Value.is_number_unsigned()
                ? Value.get<uint64>() <= static_cast<uint64>(TNumericLimits<T>::Max())
                : Value.is_number_integer()
                    && Value.get<int64>() >= static_cast<int64>(TNumericLimits<T>::Min())
                    && (std::is_signed_v<T> || Value.get<int64>() >= 0)
                    && (sizeof(T) == sizeof(int64) || Value.get<int64>() <= static_cast<int64>(TNumericLimits<T>::Max()));

            if (!bFits)
            {
                OutError = Lumina::Format("'{}' has to be a whole number from {} to {}.", Path,
                    static_cast<int64>(TNumericLimits<T>::Min()), static_cast<uint64>(TNumericLimits<T>::Max()));
            }
            return bFits;
        }

        bool ValidateEnum(FEnumProperty* Property, const nlohmann::json& Value, FStringView Path, FString& OutError)
        {
            CEnum* Enum = Property->GetEnum();
            if (Enum == nullptr || Enum->IsBitmaskEnum())
            {
                OutError = Lumina::Format("'{}' has an enum shape that is not supported.", Path);
                return false;
            }

            if (!Value.is_string())
            {
                OutError = Lumina::Format("'{}' has to be one of the enum names, as a string.", Path);
                return false;
            }

            const std::string Given = Value.get<std::string>();

            FString Allowed;
            for (const TPair<FName, uint64>& Entry : Enum->Names)
            {
                const FString EntryName(Entry.first.ToString().c_str());
                if (Detail::ToStandard(FStringView(EntryName)) == Given)
                {
                    return true;
                }

                Allowed.append(Allowed.empty() ? "" : ", ");
                Allowed.append(EntryName);
            }

            OutError = Lumina::Format("'{}' is not one of {}.", Path, Allowed);
            return false;
        }

        bool ValidateProperty(FProperty* Property, const nlohmann::json& Value, FStringView Path, FString& OutError)
        {
            const auto Expect = [&](bool bMatches, FStringView Wanted)
            {
                if (!bMatches)
                {
                    OutError = Lumina::Format("'{}' has to be {}.", Path, Wanted);
                }
                return bMatches;
            };

            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Int8:   return ValidateInteger<int8>(Value, Path, OutError);
            case EPropertyTypeFlags::Int16:  return ValidateInteger<int16>(Value, Path, OutError);
            case EPropertyTypeFlags::Int32:  return ValidateInteger<int32>(Value, Path, OutError);
            case EPropertyTypeFlags::Int64:  return ValidateInteger<int64>(Value, Path, OutError);
            case EPropertyTypeFlags::UInt8:  return ValidateInteger<uint8>(Value, Path, OutError);
            case EPropertyTypeFlags::UInt16: return ValidateInteger<uint16>(Value, Path, OutError);
            case EPropertyTypeFlags::UInt32: return ValidateInteger<uint32>(Value, Path, OutError);
            case EPropertyTypeFlags::UInt64: return ValidateInteger<uint64>(Value, Path, OutError);
            case EPropertyTypeFlags::Entity: return ValidateInteger<uint32>(Value, Path, OutError);

            case EPropertyTypeFlags::Float:
            case EPropertyTypeFlags::Double:
                return Expect(Value.is_number(), "a number");

            case EPropertyTypeFlags::Bool:
                return Expect(Value.is_boolean(), "true or false");

            case EPropertyTypeFlags::String:
            case EPropertyTypeFlags::Name:
                return Expect(Value.is_string(), "a string");

            case EPropertyTypeFlags::Object:
                {
                    if (!Expect(Value.is_string(), "an asset GUID or content path, as a string"))
                    {
                        return false;
                    }

                    const std::string Given = Value.get<std::string>();
                    if (Given.empty())
                    {
                        return true;
                    }

                    if (!ParseAssetGuid(FStringView(Given.c_str())).IsSet())
                    {
                        OutError = Lumina::Format("'{}' is neither a GUID nor an asset path.", Path);
                        return false;
                    }

                    return true;
                }

            case EPropertyTypeFlags::Enum:
                return ValidateEnum(static_cast<FEnumProperty*>(Property), Value, Path, OutError);

            case EPropertyTypeFlags::Struct:
                return ValidateStruct(static_cast<FStructProperty*>(Property)->GetStruct(), Value, Path, OutError);

            case EPropertyTypeFlags::Vector:
                {
                    if (!Expect(Value.is_array(), "an array"))
                    {
                        return false;
                    }

                    FProperty* Inner = static_cast<FArrayProperty*>(Property)->GetInternalProperty();
                    if (Inner == nullptr)
                    {
                        OutError = Lumina::Format("'{}' has no element type.", Path);
                        return false;
                    }

                    for (size_t Index = 0; Index < Value.size(); ++Index)
                    {
                        const FString ElementPath = Lumina::Format("{}[{}]", Path, Index);
                        if (!ValidateProperty(Inner, Value[Index], FStringView(ElementPath), OutError))
                        {
                            return false;
                        }
                    }

                    return true;
                }

            case EPropertyTypeFlags::SoftObject:
                return Expect(Value.is_string(), "an asset path or GUID, as a string");

            case EPropertyTypeFlags::Class:
                {
                    if (!Expect(Value.is_string(), "a class name, as a string"))
                    {
                        return false;
                    }

                    const std::string Given = Value.get<std::string>();
                    if (Given.empty() || Given == "NAME_None")
                    {
                        return true;
                    }

                    CClass* Class = FindObject<CClass>(FName(Given.c_str()));
                    if (Class == nullptr)
                    {
                        OutError = Lumina::Format("'{}' names no class called {}.", Path, Given);
                        return false;
                    }

                    CClass* Base = static_cast<FClassProperty*>(Property)->GetMetaClass();
                    if (Base != nullptr && !Class->IsChildOf(Base))
                    {
                        OutError = Lumina::Format("'{}' is not a subclass of {}.", Given, Base->GetName());
                        return false;
                    }

                    return true;
                }

            case EPropertyTypeFlags::SubStruct:
                {
                    if (!Expect(Value.is_string(), "a struct name, as a string"))
                    {
                        return false;
                    }

                    const std::string Given = Value.get<std::string>();
                    if (Given.empty() || Given == "NAME_None")
                    {
                        return true;
                    }

                    CStruct* Struct = FindObject<CStruct>(FName(Given.c_str()));
                    if (Struct == nullptr)
                    {
                        OutError = Lumina::Format("'{}' names no struct called {}.", Path, Given);
                        return false;
                    }

                    CStruct* Base = static_cast<FSubStructProperty*>(Property)->GetMetaStruct();
                    if (Base != nullptr && !Struct->IsChildOf(Base))
                    {
                        OutError = Lumina::Format("'{}' does not derive from {}.", Given, Base->GetName());
                        return false;
                    }

                    return true;
                }

            case EPropertyTypeFlags::Optional:
                {
                    if (!Expect(Value.is_object() && Value.contains("Engaged") && Value["Engaged"].is_boolean(),
                            "null, a value, or {\"Engaged\": bool, \"Value\": ...}"))
                    {
                        return false;
                    }

                    if (!Value["Engaged"].get<bool>())
                    {
                        return true;
                    }

                    FProperty* Inner = static_cast<FOptionalProperty*>(Property)->GetInternalProperty();
                    if (Inner == nullptr || !Value.contains("Value"))
                    {
                        OutError = Lumina::Format("'{}' is engaged but carries no Value.", Path);
                        return false;
                    }

                    return ValidateProperty(Inner, Value["Value"], FStringView(JoinPath(Path, "Value")), OutError);
                }

            case EPropertyTypeFlags::InstancedStruct:
                {
                    if (!Expect(Value.is_object() && Value.contains("StructType") && Value["StructType"].is_string(),
                            "{\"StructType\": name, \"Data\": {...}}"))
                    {
                        return false;
                    }

                    const std::string Key = Value["StructType"].get<std::string>();
                    if (Key.empty() || Key == "NAME_None")
                    {
                        return true;
                    }

                    CStruct* Type = ResolveInstancedStructType(
                        static_cast<FInstancedStructProperty*>(Property)->GetMetaStruct(), FName(Key.c_str()));
                    if (Type == nullptr)
                    {
                        OutError = Lumina::Format("'{}' names no struct called {} that fits here.", Path, Key);
                        return false;
                    }

                    if (!Value.contains("Data"))
                    {
                        return true;
                    }

                    return ValidateStruct(Type, Value["Data"], FStringView(JoinPath(Path, "Data")), OutError);
                }

            case EPropertyTypeFlags::Map:
                {
                    if (!Expect(Value.is_array() && Value.size() % 2 == 0,
                            "an object, or a [key, value, key, value, ...] array"))
                    {
                        return false;
                    }

                    FMapProperty* Map = static_cast<FMapProperty*>(Property);
                    FProperty* KeyProperty = Map->GetKeyProperty();
                    FProperty* ValueProperty = Map->GetValueProperty();
                    if (KeyProperty == nullptr || ValueProperty == nullptr)
                    {
                        OutError = Lumina::Format("'{}' has no key or value type.", Path);
                        return false;
                    }

                    for (size_t Index = 0; Index < Value.size(); ++Index)
                    {
                        FProperty* Element = Index % 2 == 0 ? KeyProperty : ValueProperty;
                        const FString ElementPath = Lumina::Format("{}[{}]", Path, Index);
                        if (!ValidateProperty(Element, Value[Index], FStringView(ElementPath), OutError))
                        {
                            return false;
                        }
                    }

                    return true;
                }

            case EPropertyTypeFlags::Delegate:
                OutError = Lumina::Format("'{}' is a delegate and cannot be set.", Path);
                return false;

            default:
                // Refused for the same reason the schema refuses it, so the two halves cannot drift.
                OutError = Lumina::Format("'{}' is a {}, which is not supported yet.",
                    Path, Property->GetTypeName().ToString());
                return false;
            }
        }
        // Walking a struct that references nothing would cost a full property sweep for no reason.
        bool HoldsObjects(FProperty* Property, int32 Depth = 0);

        bool StructHoldsObjects(CStruct* Struct, int32 Depth)
        {
            if (Struct == nullptr || Depth > 8)
            {
                return false;
            }

            bool bAny = false;
            for (CStruct* Current : Detail::CollectStructChain(Struct))
            {
                Current->ForEachProperty<FProperty>([&](FProperty* Property)
                {
                    bAny = bAny || (Property != nullptr && HoldsObjects(Property, Depth + 1));
                });
            }

            return bAny;
        }

        bool HoldsObjects(FProperty* Property, int32 Depth)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Object:
                return true;

            case EPropertyTypeFlags::Struct:
                return StructHoldsObjects(static_cast<FStructProperty*>(Property)->GetStruct(), Depth);

            case EPropertyTypeFlags::Vector:
                {
                    FProperty* Inner = static_cast<FArrayProperty*>(Property)->GetInternalProperty();
                    return Inner != nullptr && HoldsObjects(Inner, Depth + 1);
                }

            case EPropertyTypeFlags::Optional:
                {
                    FProperty* Inner = static_cast<FOptionalProperty*>(Property)->GetInternalProperty();
                    return Inner != nullptr && HoldsObjects(Inner, Depth + 1);
                }

            case EPropertyTypeFlags::Map:
                {
                    FMapProperty* Map = static_cast<FMapProperty*>(Property);
                    return (Map->GetKeyProperty() != nullptr && HoldsObjects(Map->GetKeyProperty(), Depth + 1))
                        || (Map->GetValueProperty() != nullptr && HoldsObjects(Map->GetValueProperty(), Depth + 1));
                }

            case EPropertyTypeFlags::InstancedStruct:
                // The concrete type is only known per instance, so assume the worst and check on apply.
                return true;

            default:
                return false;
            }
        }

        bool ApplyStructObjects(CStruct* Struct, void* Data, const nlohmann::json& Value, FStringView Path, FString& OutError);

        // Builds the key the archive just inserted, so the matching value slot can be found by lookup.
        void* FindMapValueForKey(FMapProperty* Map, void* Container, const nlohmann::json& Key)
        {
            void* Scratch = Memory::Malloc(Map->GetKeySize(), 16);
            Map->ConstructKey(Container, Scratch);

            void* Found = nullptr;
            try
            {
                nlohmann::json Mutable = Key;
                FJsonStructuredArchive Archive(Mutable, true);
                Map->GetKeyProperty()->SerializeItem(Archive.Open(), Scratch, nullptr);
                Found = Map->Find(Container, Scratch);
            }
            catch (const std::exception&)
            {
                Found = nullptr;
            }

            Map->DestructKey(Container, Scratch);
            Memory::Free(Scratch);
            return Found;
        }

        bool ApplyPropertyObjects(FProperty* Property, void* ValuePtr, const nlohmann::json& Value,
            FStringView Path, FString& OutError)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Object:
                {
                    TObjectPtr<CObject>* Slot = static_cast<TObjectPtr<CObject>*>(ValuePtr);

                    const std::string Given = Value.get<std::string>();
                    if (Given.empty())
                    {
                        *Slot = nullptr;
                        return true;
                    }

                    CObject* Resolved = nullptr;
                    FString ResolveError;
                    if (!ResolveAssetObject(FStringView(Given.c_str()), Resolved, ResolveError))
                    {
                        OutError = Lumina::Format("'{}' names no asset that could be loaded. {}", Path, ResolveError);
                        return false;
                    }

                    CClass* Expected = static_cast<FObjectProperty*>(Property)->GetPropertyClass();
                    if (Expected != nullptr && !Resolved->GetClass()->IsChildOf(Expected))
                    {
                        OutError = Lumina::Format("'{}' names a {}, but a {} is wanted.",
                            Path, Resolved->GetClass()->GetName(), Expected->GetName());
                        return false;
                    }

                    *Slot = Resolved;
                    return true;
                }

            case EPropertyTypeFlags::Struct:
                return ApplyStructObjects(static_cast<FStructProperty*>(Property)->GetStruct(),
                    ValuePtr, Value, Path, OutError);

            case EPropertyTypeFlags::Vector:
                {
                    FArrayProperty* Array = static_cast<FArrayProperty*>(Property);
                    FProperty* Inner = Array->GetInternalProperty();

                    if (Inner == nullptr || !HoldsObjects(Inner))
                    {
                        return true;
                    }

                    // The archive already sized this, but it filled every object element with null.
                    Array->Resize(ValuePtr, Value.size());

                    for (size_t Index = 0; Index < Value.size(); ++Index)
                    {
                        const FString ElementPath = Lumina::Format("{}[{}]", Path, Index);
                        if (!ApplyPropertyObjects(Inner, Array->GetAt(ValuePtr, Index), Value[Index],
                                FStringView(ElementPath), OutError))
                        {
                            return false;
                        }
                    }

                    return true;
                }

            case EPropertyTypeFlags::Optional:
                {
                    FOptionalProperty* Optional = static_cast<FOptionalProperty*>(Property);
                    FProperty* Inner = Optional->GetInternalProperty();

                    if (Inner == nullptr || !Optional->HasValue(ValuePtr) || !Value.is_object() || !Value.contains("Value"))
                    {
                        return true;
                    }

                    return ApplyPropertyObjects(Inner, Optional->GetValue(ValuePtr), Value["Value"],
                        FStringView(JoinPath(Path, "Value")), OutError);
                }

            case EPropertyTypeFlags::InstancedStruct:
                {
                    FInstancedStruct* Instance = static_cast<FInstancedStruct*>(ValuePtr);
                    CStruct* Type = Instance->GetScriptStruct();

                    if (Type == nullptr || !Value.is_object() || !Value.contains("Data"))
                    {
                        return true;
                    }

                    return ApplyStructObjects(Type, Instance->GetMutableMemory(), Value["Data"],
                        FStringView(JoinPath(Path, "Data")), OutError);
                }

            case EPropertyTypeFlags::Map:
                {
                    FMapProperty* Map = static_cast<FMapProperty*>(Property);
                    FProperty* ValueProperty = Map->GetValueProperty();

                    if (ValueProperty == nullptr || !HoldsObjects(ValueProperty) || !Value.is_array())
                    {
                        return true;
                    }

                    for (size_t Index = 0; Index + 1 < Value.size(); Index += 2)
                    {
                        void* Slot = FindMapValueForKey(Map, ValuePtr, Value[Index]);
                        if (Slot == nullptr)
                        {
                            continue;
                        }

                        const FString ElementPath = Lumina::Format("{}[{}]", Path, Index + 1);
                        if (!ApplyPropertyObjects(ValueProperty, Slot, Value[Index + 1], FStringView(ElementPath), OutError))
                        {
                            return false;
                        }
                    }

                    return true;
                }

            default:
                return true;
            }
        }

        bool ApplyStructObjects(CStruct* Struct, void* Data, const nlohmann::json& Value, FStringView Path, FString& OutError)
        {
            for (CStruct* Current : Detail::CollectStructChain(Struct))
            {
                bool bOk = true;

                Current->ForEachProperty<FProperty>([&](FProperty* Property)
                {
                    if (!bOk || Property == nullptr || !HoldsObjects(Property))
                    {
                        return;
                    }

                    const FString Name(Property->GetPropertyName().ToString().c_str());

                    const auto Found = Value.find(Detail::ToStandard(FStringView(Name)));
                    if (Found == Value.end())
                    {
                        return;
                    }

                    bOk = ApplyPropertyObjects(Property, Property->GetValuePtr<uint8>(Data),
                        *Found, FStringView(JoinPath(Path, FStringView(Name))), OutError);
                });

                if (!bOk)
                {
                    return false;
                }
            }

            return true;
        }
        void CaptureStructObjects(CStruct* Struct, void* Data, nlohmann::json& Out);

        void CapturePropertyObjects(FProperty* Property, void* ValuePtr, nlohmann::json& Out)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Object:
                {
                    // The archive wrote the object's name, which cannot be looked back up.
                    const TObjectPtr<CObject>* Slot = static_cast<const TObjectPtr<CObject>*>(ValuePtr);
                    CObject* Referenced = Slot->Get();

                    Out = Referenced != nullptr
                        ? Detail::ToStandard(FStringView(Referenced->GetGUID().ToString()))
                        : std::string();
                    return;
                }

            case EPropertyTypeFlags::Struct:
                CaptureStructObjects(static_cast<FStructProperty*>(Property)->GetStruct(), ValuePtr, Out);
                return;

            case EPropertyTypeFlags::Vector:
                {
                    FArrayProperty* Array = static_cast<FArrayProperty*>(Property);
                    FProperty* Inner = Array->GetInternalProperty();

                    if (Inner == nullptr || !HoldsObjects(Inner) || !Out.is_array())
                    {
                        return;
                    }

                    const size_t Count = Array->GetNum(ValuePtr);
                    for (size_t Index = 0; Index < Count && Index < Out.size(); ++Index)
                    {
                        CapturePropertyObjects(Inner, Array->GetAt(ValuePtr, Index), Out[Index]);
                    }

                    return;
                }

            case EPropertyTypeFlags::Optional:
                {
                    FOptionalProperty* Optional = static_cast<FOptionalProperty*>(Property);
                    FProperty* Inner = Optional->GetInternalProperty();

                    if (Inner != nullptr && Optional->HasValue(ValuePtr) && Out.is_object() && Out.contains("Value"))
                    {
                        CapturePropertyObjects(Inner, Optional->GetValue(ValuePtr), Out["Value"]);
                    }

                    return;
                }

            case EPropertyTypeFlags::InstancedStruct:
                {
                    FInstancedStruct* Instance = static_cast<FInstancedStruct*>(ValuePtr);
                    CStruct* Type = Instance->GetScriptStruct();

                    if (Type != nullptr && Out.is_object() && Out.contains("Data"))
                    {
                        CaptureStructObjects(Type, Instance->GetMutableMemory(), Out["Data"]);
                    }

                    return;
                }

            case EPropertyTypeFlags::Map:
                {
                    FMapProperty* Map = static_cast<FMapProperty*>(Property);
                    FProperty* ValueProperty = Map->GetValueProperty();

                    if (ValueProperty == nullptr || !HoldsObjects(ValueProperty) || !Out.is_array())
                    {
                        return;
                    }

                    // The archive wrote pairs in ForEach order, so walking it again lines up with the output.
                    size_t Index = 1;
                    Map->ForEach(ValuePtr, [&](const void*, void* Slot)
                    {
                        if (Index < Out.size())
                        {
                            CapturePropertyObjects(ValueProperty, Slot, Out[Index]);
                        }
                        Index += 2;
                    });

                    return;
                }

            default:
                return;
            }
        }

        void CaptureStructObjects(CStruct* Struct, void* Data, nlohmann::json& Out)
        {
            if (Struct == nullptr || !Out.is_object())
            {
                return;
            }

            for (CStruct* Current : Detail::CollectStructChain(Struct))
            {
                Current->ForEachProperty<FProperty>([&](FProperty* Property)
                {
                    if (Property == nullptr || !HoldsObjects(Property))
                    {
                        return;
                    }

                    const FString Name(Property->GetPropertyName().ToString().c_str());
                    const std::string Key = Detail::ToStandard(FStringView(Name));

                    if (Out.contains(Key))
                    {
                        CapturePropertyObjects(Property, Property->GetValuePtr<uint8>(Data), Out[Key]);
                    }
                });
            }
        }
    }

    FMarshalResult ValidatePropertyValue(const nlohmann::json& In, FProperty* Property, FStringView Path)
    {
        FMarshalResult Result;

        if (Property == nullptr)
        {
            Result.Error = "No property was given.";
            return Result;
        }

        nlohmann::json Canonical = In;
        if (CanonicalizeProperty(Property, Canonical, Path, Result.Error))
        {
            ValidateProperty(Property, Canonical, Path, Result.Error);
        }

        return Result;
    }

    FMarshalResult ReadProperty(const nlohmann::json& In, FProperty* Property, void* ValuePtr, FStringView Path)
    {
        FMarshalResult Result;

        if (Property == nullptr || ValuePtr == nullptr)
        {
            Result.Error = "No property was given.";
            return Result;
        }

        nlohmann::json Canonical = In;
        if (!CanonicalizeProperty(Property, Canonical, Path, Result.Error)
            || !ValidateProperty(Property, Canonical, Path, Result.Error))
        {
            return Result;
        }

        // The archive walks a mutable tree, and the canonical copy is still needed intact for the fixup below.
        nlohmann::json Mutable = Canonical;

        try
        {
            FJsonStructuredArchive Archive(Mutable, true);
            IStructuredArchive::FSlot Slot = Archive.Open();
            Property->SerializeItem(Slot, ValuePtr, nullptr);
        }
        catch (const std::exception& Exception)
        {
            Result.Error = Lumina::Format("'{}' could not be applied. {}", Path, Exception.what());
            return Result;
        }

        // The archive deliberately leaves object references null, so they are resolved here instead.
        if (HoldsObjects(Property))
        {
            ApplyPropertyObjects(Property, ValuePtr, Canonical, Path, Result.Error);
        }

        return Result;
    }

    FMarshalResult WriteProperty(FProperty* Property, void* ValuePtr, nlohmann::json& Out)
    {
        FMarshalResult Result;

        if (Property == nullptr || ValuePtr == nullptr)
        {
            Result.Error = "No property was given.";
            return Result;
        }

        Out = nlohmann::json();

        try
        {
            FJsonStructuredArchive Archive(Out, false);
            IStructuredArchive::FSlot Slot = Archive.Open();
            Property->SerializeItem(Slot, ValuePtr, nullptr);

            if (HoldsObjects(Property))
            {
                CapturePropertyObjects(Property, ValuePtr, Out);
            }
        }
        catch (const std::exception& Exception)
        {
            Out = nlohmann::json();
            Result.Error = Lumina::Format("The value could not be written. {}", Exception.what());
        }

        return Result;
    }

    FMarshalResult ReadStruct(const nlohmann::json& In, CStruct* Type, void* Data)
    {
        FMarshalResult Result;

        if (Type == nullptr || Data == nullptr)
        {
            Result.Error = "No struct instance was given.";
            return Result;
        }

        nlohmann::json Canonical = In;
        if (!CanonicalizeStruct(Type, Canonical, FStringView(), Result.Error)
            || !ValidateStruct(Type, Canonical, FStringView(), Result.Error))
        {
            return Result;
        }

        // LoadStruct walks a mutable tree, and the canonical copy is still needed intact for the fixup below.
        nlohmann::json Mutable = Canonical;

        try
        {
            FJsonStructuredArchive::LoadStruct(Mutable, Type, Data);
        }
        catch (const std::exception& Exception)
        {
            Result.Error = Lumina::Format("The arguments could not be applied. {}", Exception.what());
            return Result;
        }

        // The archive deliberately leaves object references null, so they are resolved here instead.
        ApplyStructObjects(Type, Data, Canonical, FStringView(), Result.Error);

        return Result;
    }

    FMarshalResult WriteStruct(CStruct* Type, void* Data, nlohmann::json& Out)
    {
        FMarshalResult Result;

        if (Type == nullptr || Data == nullptr)
        {
            Result.Error = "No struct instance was given.";
            return Result;
        }

        Out = nlohmann::json::object();

        try
        {
            FJsonStructuredArchive::SaveStruct(Out, Type, Data);
            CaptureStructObjects(Type, Data, Out);
        }
        catch (const std::exception& Exception)
        {
            Out = nlohmann::json();
            Result.Error = Lumina::Format("The result could not be written. {}", Exception.what());
        }

        return Result;
    }
}
