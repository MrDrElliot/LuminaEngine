#include "RuntimePCH.h"
#include "Blackboard.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectUtils.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"

namespace Lumina
{
    namespace
    {
        /** Key type a reflected property maps onto, or false when it has no blackboard equivalent. */
        bool MapPropertyToKeyType(EPropertyTypeFlags TypeFlags, EBlackboardKeyType& OutType)
        {
            switch (TypeFlags)
            {
            case EPropertyTypeFlags::Float:
            case EPropertyTypeFlags::Double:
                OutType = EBlackboardKeyType::Float;
                return true;

            // Every width collapses onto the one Int key type; the blackboard stores an int32, so a
            // 64-bit field is representable but truncating, and unsigned loses its top bit.
            case EPropertyTypeFlags::Int8:
            case EPropertyTypeFlags::Int16:
            case EPropertyTypeFlags::Int32:
            case EPropertyTypeFlags::Int64:
            case EPropertyTypeFlags::UInt8:
            case EPropertyTypeFlags::UInt16:
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::UInt64:
                OutType = EBlackboardKeyType::Int;
                return true;

            case EPropertyTypeFlags::Bool:
                OutType = EBlackboardKeyType::Bool;
                return true;

            case EPropertyTypeFlags::Enum:
                OutType = EBlackboardKeyType::Enum;
                return true;

            case EPropertyTypeFlags::Vector:
                OutType = EBlackboardKeyType::Vector;
                return true;

            case EPropertyTypeFlags::Object:
                OutType = EBlackboardKeyType::Object;
                return true;

            // Strings, containers, nested structs and delegates have no key type, and Entity is not a
            // reflected property type at all, so those keys stay hand-authored.
            default:
                return false;
            }
        }

        /** Reads the property's value out of a default-constructed instance into the key's default. */
        void ReadDefaultFromProperty(FBlackboardKey& Key, FProperty* Property, const void* Defaults)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Float:
                Key.DefaultFloat = *Property->GetValuePtr<float>(Defaults);
                break;

            case EPropertyTypeFlags::Double:
                Key.DefaultFloat = (float)*Property->GetValuePtr<double>(Defaults);
                break;

            case EPropertyTypeFlags::Int8:   Key.DefaultInt = (int32)*Property->GetValuePtr<int8>(Defaults);   break;
            case EPropertyTypeFlags::Int16:  Key.DefaultInt = (int32)*Property->GetValuePtr<int16>(Defaults);  break;
            case EPropertyTypeFlags::Int32:  Key.DefaultInt =        *Property->GetValuePtr<int32>(Defaults);  break;
            case EPropertyTypeFlags::Int64:  Key.DefaultInt = (int32)*Property->GetValuePtr<int64>(Defaults);  break;
            case EPropertyTypeFlags::UInt8:  Key.DefaultInt = (int32)*Property->GetValuePtr<uint8>(Defaults);  break;
            case EPropertyTypeFlags::UInt16: Key.DefaultInt = (int32)*Property->GetValuePtr<uint16>(Defaults); break;
            case EPropertyTypeFlags::UInt32: Key.DefaultInt = (int32)*Property->GetValuePtr<uint32>(Defaults); break;
            case EPropertyTypeFlags::UInt64: Key.DefaultInt = (int32)*Property->GetValuePtr<uint64>(Defaults); break;

            case EPropertyTypeFlags::Bool:
                Key.DefaultBool = *Property->GetValuePtr<bool>(Defaults);
                break;

            case EPropertyTypeFlags::Vector:
                Key.DefaultVector = *Property->GetValuePtr<FVector3>(Defaults);
                break;

            case EPropertyTypeFlags::Enum:
                {
                    // The value lives in the inner numeric property, whose width is the enum's
                    // underlying type; reading the enum property directly would assume a size.
                    FEnumProperty* EnumProperty = static_cast<FEnumProperty*>(Property);

                    if (CEnum* Enum = EnumProperty->GetEnum())
                    {
                        Key.EnumType = Enum->GetName();
                    }

                    if (FNumericProperty* Inner = EnumProperty->GetInnerProperty())
                    {
                        Key.DefaultInt = (int32)Inner->GetSignedIntPropertyValue_InContainer(Defaults);
                    }
                }
                break;

            // Object defaults stay null: a struct's default-constructed object field points at nothing
            // worth capturing, and anything it did point at would be a reference the asset has to own.
            default:
                break;
            }
        }

        bool KeysMatch(const FBlackboardKey& A, const FBlackboardKey& B)
        {
            return A.Name          == B.Name
                && A.Type          == B.Type
                && A.Flags         == B.Flags
                && A.DefaultFloat  == B.DefaultFloat
                && A.DefaultInt    == B.DefaultInt
                && A.DefaultBool   == B.DefaultBool
                && A.EnumType      == B.EnumType
                && A.DefaultVector == B.DefaultVector
                && A.DefaultObject == B.DefaultObject
                && A.bDerived      == B.bDerived;
        }
    }

    int32 CBlackboard::FindKeyIndex(const FName& Name) const
    {
        for (int32 i = 0; i < (int32)Keys.size(); ++i)
        {
            if (Keys[i].Name == Name)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    const FBlackboardKey* CBlackboard::FindKey(const FName& Name) const
    {
        const int32 Index = FindKeyIndex(Name);
        return Index == INDEX_NONE ? nullptr : &Keys[Index];
    }

    EBlackboardKeyType CBlackboard::GetKeyType(const FName& Name, EBlackboardKeyType Fallback) const
    {
        const FBlackboardKey* Key = FindKey(Name);
        return Key == nullptr ? Fallback : Key->Type;
    }

    FName CBlackboard::GetKeyName(int32 Index) const
    {
        return (Index >= 0 && Index < (int32)Keys.size()) ? Keys[Index].Name : FName();
    }

    CStruct* CBlackboard::GetBackingStruct() const
    {
        return BackingStructCache.Resolve(BackingStructName);
    }

    void CBlackboard::SetBackingStruct(CStruct* InStruct)
    {
        BackingStructName = InStruct != nullptr ? DataStructIdentity(InStruct) : FName();
        BackingStructCache.Set(InStruct, BackingStructName);

        SyncKeysFromBackingStruct();
    }

    bool CBlackboard::SyncKeysFromBackingStruct()
    {
        CStruct* Struct = GetBackingStruct();

        TVector<FBlackboardKey> Rebuilt;
        Rebuilt.reserve(Keys.size());

        if (Struct != nullptr && Struct->GetSize() > 0)
        {
            // Defaults come from a real instance rather than from the property's declared zero: a
            // struct's member initializers are what the author sees in the type, and they only exist
            // once it has been constructed.
            TVector<uint8> Instance;
            Instance.resize(Struct->GetSize());
            Struct->InitializeStruct(Instance.data());

            Struct->ForEachProperty<FProperty>([&](FProperty* Property)
            {
                EBlackboardKeyType KeyType;
                if (!MapPropertyToKeyType(Property->GetType(), KeyType))
                {
                    return;
                }

                FBlackboardKey Key;
                Key.Name     = Property->GetPropertyName();
                Key.Type     = KeyType;
                Key.bDerived = true;

                ReadDefaultFromProperty(Key, Property, Instance.data());

                Rebuilt.push_back(Move(Key));
            });

            Struct->DestroyStruct(Instance.data());
        }

        // Hand-authored keys survive a backing type change, and win a name collision: the derived one
        // can always be recovered by removing the manual key, whereas dropping the manual key loses
        // whatever was typed into it.
        for (const FBlackboardKey& Key : Keys)
        {
            if (Key.bDerived)
            {
                continue;
            }

            for (int32 i = 0; i < (int32)Rebuilt.size(); ++i)
            {
                if (Rebuilt[i].Name == Key.Name)
                {
                    Rebuilt.erase(Rebuilt.begin() + i);
                    break;
                }
            }

            Rebuilt.push_back(Key);
        }

        bool bChanged = Rebuilt.size() != Keys.size();
        for (int32 i = 0; !bChanged && i < (int32)Rebuilt.size(); ++i)
        {
            bChanged = !KeysMatch(Rebuilt[i], Keys[i]);
        }

        if (bChanged)
        {
            Keys = Move(Rebuilt);
        }

        return bChanged;
    }
}
