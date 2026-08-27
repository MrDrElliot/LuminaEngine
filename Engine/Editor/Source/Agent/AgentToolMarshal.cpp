#include "EditorPCH.h"
#include "Agent/AgentToolMarshal.h"

#include "Agent/AgentReflectionUtils.h"
#include "Containers/StringFormat.h"
#include "Containers/Vector.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"

namespace Lumina::Agent
{
    namespace
    {
        FString JoinPath(FStringView Path, FStringView Leaf)
        {
            return Path.empty() ? FString(Leaf.data(), Leaf.size()) : Lumina::Format("{}.{}", Path, Leaf);
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
            case EPropertyTypeFlags::Int8:
            case EPropertyTypeFlags::Int16:
            case EPropertyTypeFlags::Int32:
            case EPropertyTypeFlags::Int64:
            case EPropertyTypeFlags::UInt8:
            case EPropertyTypeFlags::UInt16:
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::UInt64:
                return Expect(Value.is_number_integer(), "a whole number");

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
                    if (!Expect(Value.is_string(), "an asset GUID, as a string"))
                    {
                        return false;
                    }

                    const std::string Given = Value.get<std::string>();
                    if (Given.empty())
                    {
                        return true;
                    }

                    if (!FGuid::TryParse(FStringView(Given.c_str())).IsSet())
                    {
                        OutError = Lumina::Format("'{}' is not a GUID.", Path);
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

            default:
                // Refused for the same reason the schema refuses it, so the two halves cannot drift.
                OutError = Lumina::Format("'{}' is a {}, which is not supported yet.",
                    Path, Property->TypeName.ToString());
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

            default:
                return false;
            }
        }

        bool ApplyStructObjects(CStruct* Struct, void* Data, const nlohmann::json& Value, FStringView Path, FString& OutError);

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

                    const TOptional<FGuid> Guid = FGuid::TryParse(FStringView(Given.c_str()));
                    CObject* Resolved = Guid.IsSet() ? StaticLoadObject(*Guid) : nullptr;

                    if (Resolved == nullptr)
                    {
                        OutError = Lumina::Format("'{}' names no asset that could be loaded.", Path);
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

        ValidateProperty(Property, In, Path, Result.Error);
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

        if (!ValidateProperty(Property, In, Path, Result.Error))
        {
            return Result;
        }

        // Object references never travel through the archive, so they are applied on their own path.
        if (HoldsObjects(Property))
        {
            ApplyPropertyObjects(Property, ValuePtr, In, Path, Result.Error);
            return Result;
        }

        nlohmann::json Mutable = In;

        try
        {
            FJsonStructuredArchive Archive(Mutable, true);
            IStructuredArchive::FSlot Slot = Archive.Open();
            Property->SerializeItem(Slot, ValuePtr, nullptr);
        }
        catch (const std::exception& Exception)
        {
            Result.Error = Lumina::Format("'{}' could not be applied. {}", Path, Exception.what());
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

        if (!ValidateStruct(Type, In, FStringView(), Result.Error))
        {
            return Result;
        }

        // LoadStruct walks a mutable tree, and the caller's arguments are not ours to touch.
        nlohmann::json Mutable = In;

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
        ApplyStructObjects(Type, Data, In, FStringView(), Result.Error);

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
