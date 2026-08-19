#include "RuntimePCH.h"
#include "ScriptValueBridge.h"

#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Memory/Memory.h"

namespace Lumina::Scripting
{
    namespace
    {
        FScriptPropertyValue ReadValue(FProperty* Property, const void* ValuePtr);
        void WriteValue(FProperty* Property, void* ValuePtr, const FScriptPropertyValue& Value);

        bool NameEqualsIgnoreCase(const FName& A, const FName& B)
        {
            if (A == B)
            {
                return true;
            }
            const char* X = A.c_str();
            const char* Y = B.c_str();
            for (; *X != '\0' && *Y != '\0'; ++X, ++Y)
            {
                char CX = (*X >= 'A' && *X <= 'Z') ? (char)(*X + 32) : *X;
                char CY = (*Y >= 'A' && *Y <= 'Z') ? (char)(*Y + 32) : *Y;
                if (CX != CY)
                {
                    return false;
                }
            }
            return *X == *Y;
        }

        const FScriptPropertyEntry* FindEntry(const TVector<FScriptPropertyEntry>& Values, const FName& Name)
        {
            for (const FScriptPropertyEntry& Entry : Values)
            {
                if (NameEqualsIgnoreCase(Entry.Name, Name))
                {
                    return &Entry;
                }
            }
            return nullptr;
        }

        // The candidate struct (deriving from Base) carrying the matching ScriptTypeName, or null.
        CStruct* FindInstanceCandidate(CStruct* Base, const FString& TypeName)
        {
            if (Base == nullptr || TypeName.empty())
            {
                return nullptr;
            }
            for (TObjectIterator<CStruct> It; It; ++It)
            {
                CStruct* Candidate = *It;
                if (Candidate == Base || !Candidate->IsChildOf(Base))
                {
                    continue;
                }
                if (const FString* Name = Candidate->Metadata.TryGetMetadata("ScriptTypeName"); Name && *Name == TypeName)
                {
                    return Candidate;
                }
            }
            return nullptr;
        }

        void ReadStruct(const CStruct* Struct, const void* Buffer, TVector<FScriptPropertyEntry>& Out)
        {
            for (FProperty* Property = Struct->LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
            {
                FScriptPropertyEntry Entry;
                Entry.Name = Property->GetPropertyName();
                Entry.Value = ReadValue(Property, static_cast<const uint8*>(Buffer) + Property->Offset);
                Out.push_back(std::move(Entry));
            }
        }

        void WriteStruct(const CStruct* Struct, void* Buffer, const TVector<FScriptPropertyEntry>& Values)
        {
            for (FProperty* Property = Struct->LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
            {
                if (const FScriptPropertyEntry* Entry = FindEntry(Values, Property->GetPropertyName()))
                {
                    WriteValue(Property, static_cast<uint8*>(Buffer) + Property->Offset, Entry->Value);
                }
            }
        }

        FScriptPropertyValue ReadValue(FProperty* Property, const void* ValuePtr)
        {
            FScriptPropertyValue Value;
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Bool:
                Value.Kind = EScriptValueKind::Bool;
                Value.AsBool = static_cast<FNumericProperty*>(Property)->GetSignedIntPropertyValue(ValuePtr) != 0;
                break;
            case EPropertyTypeFlags::Int8:
            case EPropertyTypeFlags::Int16:
            case EPropertyTypeFlags::Int32:
            case EPropertyTypeFlags::Int64:
                Value.Kind = EScriptValueKind::Int;
                Value.AsInt = static_cast<FNumericProperty*>(Property)->GetSignedIntPropertyValue(ValuePtr);
                break;
            case EPropertyTypeFlags::UInt8:
            case EPropertyTypeFlags::UInt16:
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::UInt64:
                Value.Kind = EScriptValueKind::Int;
                Value.AsInt = (int64)static_cast<FNumericProperty*>(Property)->GetUnsignedIntPropertyValue(ValuePtr);
                break;
            case EPropertyTypeFlags::Float:
                Value.Kind = EScriptValueKind::Double;
                Value.AsDouble = *static_cast<const float*>(ValuePtr);
                break;
            case EPropertyTypeFlags::Double:
                Value.Kind = EScriptValueKind::Double;
                Value.AsDouble = *static_cast<const double*>(ValuePtr);
                break;
            case EPropertyTypeFlags::Enum:
                Value.Kind = EScriptValueKind::Int;
                Value.AsInt = *static_cast<const int64*>(ValuePtr);
                break;
            case EPropertyTypeFlags::String:
                Value.Kind = EScriptValueKind::String;
                Value.AsString = *static_cast<const FString*>(ValuePtr);
                break;
            case EPropertyTypeFlags::SoftObject:
            {
                Value.Kind = EScriptValueKind::String;
                const FStringView Path = static_cast<const FSoftObjectPath*>(ValuePtr)->GetPath();
                Value.AsString.assign(Path.data(), Path.size());
                break;
            }
            case EPropertyTypeFlags::Struct:
                Value.Kind = EScriptValueKind::Nested;
                ReadStruct(static_cast<FStructProperty*>(Property)->GetStruct(), ValuePtr, Value.StructFields);
                break;
            case EPropertyTypeFlags::InstancedStruct:
            {
                Value.Kind = EScriptValueKind::Instance;
                const FInstancedStruct* Instance = static_cast<const FInstancedStruct*>(ValuePtr);
                if (CStruct* Chosen = Instance->GetScriptStruct())
                {
                    if (const FString* TypeName = Chosen->Metadata.TryGetMetadata("ScriptTypeName"))
                    {
                        Value.AsString = *TypeName;
                    }
                    else
                    {
                        Value.AsString.assign(Chosen->GetName().c_str());
                    }
                    ReadStruct(Chosen, Instance->GetMemory(), Value.StructFields);
                }
                break;
            }
            case EPropertyTypeFlags::Vector:
            {
                Value.Kind = EScriptValueKind::Array;
                FArrayProperty* Array = static_cast<FArrayProperty*>(Property);
                FProperty* Inner = Array->GetInternalProperty();
                const SIZE_T Count = Array->GetNum(ValuePtr);
                Value.Items.reserve(Count);
                for (SIZE_T Index = 0; Index < Count; ++Index)
                {
                    Value.Items.push_back(ReadValue(Inner, Array->GetAt(const_cast<void*>(ValuePtr), Index)));
                }
                break;
            }
            case EPropertyTypeFlags::Map:
            {
                Value.Kind = EScriptValueKind::Map;
                FMapProperty* Map = static_cast<FMapProperty*>(Property);
                FProperty* KeyProp = Map->GetKeyProperty();
                FProperty* ValueProp = Map->GetValueProperty();
                if (KeyProp != nullptr && ValueProp != nullptr)
                {
                    Value.Items.reserve(Map->GetNum(ValuePtr) * 2);
                    Map->ForEach(ValuePtr, [&](const void* KeyPtr, void* PairValuePtr)
                    {
                        Value.Items.push_back(ReadValue(KeyProp, KeyPtr));
                        Value.Items.push_back(ReadValue(ValueProp, PairValuePtr));
                    });
                }
                break;
            }
            default:
                break;
            }
            return Value;
        }

        void WriteValue(FProperty* Property, void* ValuePtr, const FScriptPropertyValue& Value)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Bool:
                static_cast<FNumericProperty*>(Property)->SetIntPropertyValue(ValuePtr, (int64)(Value.AsBool ? 1 : 0));
                break;
            case EPropertyTypeFlags::Int8:
            case EPropertyTypeFlags::Int16:
            case EPropertyTypeFlags::Int32:
            case EPropertyTypeFlags::Int64:
                static_cast<FNumericProperty*>(Property)->SetIntPropertyValue(ValuePtr, Value.AsInt);
                break;
            case EPropertyTypeFlags::UInt8:
            case EPropertyTypeFlags::UInt16:
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::UInt64:
                static_cast<FNumericProperty*>(Property)->SetIntPropertyValue(ValuePtr, (uint64)Value.AsInt);
                break;
            case EPropertyTypeFlags::Float:
                *static_cast<float*>(ValuePtr) = (float)Value.AsDouble;
                break;
            case EPropertyTypeFlags::Double:
                *static_cast<double*>(ValuePtr) = Value.AsDouble;
                break;
            case EPropertyTypeFlags::Enum:
                *static_cast<int64*>(ValuePtr) = Value.AsInt;
                break;
            case EPropertyTypeFlags::String:
                *static_cast<FString*>(ValuePtr) = Value.AsString;
                break;
            case EPropertyTypeFlags::SoftObject:
                static_cast<FSoftObjectPath*>(ValuePtr)->SetPath(FStringView(Value.AsString.c_str(), Value.AsString.size()));
                break;
            case EPropertyTypeFlags::Struct:
                WriteStruct(static_cast<FStructProperty*>(Property)->GetStruct(), ValuePtr, Value.StructFields);
                break;
            case EPropertyTypeFlags::InstancedStruct:
            {
                FInstancedStruct* Instance = static_cast<FInstancedStruct*>(ValuePtr);
                if (Value.AsString.empty())
                {
                    Instance->Reset();
                    break;
                }
                CStruct* Base = static_cast<FInstancedStructProperty*>(Property)->GetMetaStruct();
                CStruct* Chosen = FindInstanceCandidate(Base, Value.AsString);
                Instance->InitializeAs(Chosen);
                if (Chosen != nullptr)
                {
                    WriteStruct(Chosen, Instance->GetMutableMemory(), Value.StructFields);
                }
                break;
            }
            case EPropertyTypeFlags::Vector:
            {
                FArrayProperty* Array = static_cast<FArrayProperty*>(Property);
                FProperty* Inner = Array->GetInternalProperty();
                Array->Clear(ValuePtr);
                for (SIZE_T Index = 0; Index < Value.Items.size(); ++Index)
                {
                    Array->PushBack(ValuePtr, nullptr);
                    WriteValue(Inner, Array->GetAt(ValuePtr, Index), Value.Items[Index]);
                }
                break;
            }
            case EPropertyTypeFlags::Map:
            {
                FMapProperty* Map = static_cast<FMapProperty*>(Property);
                FProperty* KeyProp = Map->GetKeyProperty();
                FProperty* ValueProp = Map->GetValueProperty();
                Map->Clear(ValuePtr);
                if (KeyProp == nullptr || ValueProp == nullptr) { break; }

                // Items are the interleaved [key, value] pairs. Build a scratch key, write the key into it, insert
                // to get the value slot, then write the value in place.
                const uint32 KeySize = Map->GetKeySize();
                void* KeyScratch = Memory::Malloc(KeySize > 0 ? KeySize : 1, 16);
                for (SIZE_T Index = 0; Index + 1 < Value.Items.size(); Index += 2)
                {
                    Map->ConstructKey(ValuePtr, KeyScratch);
                    WriteValue(KeyProp, KeyScratch, Value.Items[Index]);
                    void* Slot = Map->Insert(ValuePtr, KeyScratch, nullptr);
                    if (Slot != nullptr)
                    {
                        WriteValue(ValueProp, Slot, Value.Items[Index + 1]);
                    }
                    Map->DestructKey(ValuePtr, KeyScratch);
                }
                Memory::Free(KeyScratch);
                break;
            }
            default:
                break;
            }
        }
    }

    void ReadStructToValues(const CStruct* Layout, const void* Buffer, TVector<FScriptPropertyEntry>& OutValues)
    {
        OutValues.clear();
        if (Layout != nullptr && Buffer != nullptr)
        {
            ReadStruct(Layout, Buffer, OutValues);
        }
    }

    void WriteValuesToStruct(const CStruct* Layout, void* Buffer, const TVector<FScriptPropertyEntry>& Values)
    {
        if (Layout != nullptr && Buffer != nullptr)
        {
            WriteStruct(Layout, Buffer, Values);
        }
    }
}
