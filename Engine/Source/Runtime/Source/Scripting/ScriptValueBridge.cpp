#include "RuntimePCH.h"
#include "ScriptValueBridge.h"

#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Memory/Memory.h"

namespace Lumina::Scripting
{
    namespace
    {
        void ScriptValueBridgeWriteValue(FProperty* Property, void* ValuePtr, const FScriptPropertyValue& Value);

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

        void WriteStruct(const CStruct* Struct, void* Buffer, const TVector<FScriptPropertyEntry>& Values)
        {
            for (FProperty* Property : Struct->GetProperties())
            {
                if (const FScriptPropertyEntry* Entry = FindEntry(Values, Property->GetPropertyName()))
                {
                    ScriptValueBridgeWriteValue(Property, static_cast<uint8*>(Buffer) + Property->Offset, Entry->Value);
                }
            }
        }

        void ScriptValueBridgeWriteValue(FProperty* Property, void* ValuePtr, const FScriptPropertyValue& Value)
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
            case EPropertyTypeFlags::Entity:
                static_cast<FNumericProperty*>(Property)->SetIntPropertyValue(ValuePtr, (uint64)Value.AsInt);
                break;
            case EPropertyTypeFlags::Float:
                *static_cast<float*>(ValuePtr) = (float)Value.AsDouble;
                break;
            case EPropertyTypeFlags::Double:
                *static_cast<double*>(ValuePtr) = Value.AsDouble;
                break;
            case EPropertyTypeFlags::Enum:
            {
                if (FNumericProperty* Inner = static_cast<FEnumProperty*>(Property)->GetInnerProperty())
                {
                    Inner->SetIntPropertyValue(ValuePtr, Value.AsInt);
                }
                break;
            }
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
                    ScriptValueBridgeWriteValue(Inner, Array->GetAt(ValuePtr, Index), Value.Items[Index]);
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

                // Build a scratch key, insert to get the value slot, then write the value in place.
                const uint32 KeySize = Map->GetKeySize();
                void* KeyScratch = Memory::Malloc(KeySize > 0 ? KeySize : 1, 16);
                for (SIZE_T Index = 0; Index + 1 < Value.Items.size(); Index += 2)
                {
                    Map->ConstructKey(ValuePtr, KeyScratch);
                    ScriptValueBridgeWriteValue(KeyProp, KeyScratch, Value.Items[Index]);
                    void* Slot = Map->Insert(ValuePtr, KeyScratch, nullptr);
                    if (Slot != nullptr)
                    {
                        ScriptValueBridgeWriteValue(ValueProp, Slot, Value.Items[Index + 1]);
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

    void WriteValuesToStruct(const CStruct* Layout, void* Buffer, const TVector<FScriptPropertyEntry>& Values)
    {
        if (Layout != nullptr && Buffer != nullptr)
        {
            WriteStruct(Layout, Buffer, Values);
        }
    }
}
