#include "RuntimePCH.h"
#include "SaveGameArchive.h"

#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/ScriptableObject.h"

namespace Lumina
{
    namespace
    {
        enum class EObjectRecord : uint8
        {
            Null,
            Asset,
            Inline,
            Repeat,
        };

        // Anything outside a real package was made at runtime, so a GUID would name nothing on the next run.
        bool IsWrittenInline(const CObject* Object)
        {
            const CPackage* Package = Object->GetPackage();
            return Package == nullptr || Package->IsTransientPackage();
        }

        bool Wants(const FProperty* Property, ESaveGameProperties Which)
        {
            if (!Property->IsSaveGame())
            {
                return false;
            }
            return Which == ESaveGameProperties::SaveGame || !Property->ShouldSerialize();
        }

        // Patched in after the payload, so the count and every length cost nothing to know up front.
        template<typename TBody>
        void WriteSized(FArchive& Ar, TBody&& Body)
        {
            const int64 SizePosition = Ar.Tell();
            int64 Size = 0;
            Ar << Size;
            const int64 Start = Ar.Tell();
            Body();
            const int64 End = Ar.Tell();
            Size = End - Start;
            Ar.Seek(SizePosition);
            Ar << Size;
            Ar.Seek(End);
        }
    }

    FSaveGameArchive::FSaveGameArchive(FArchive& InInnerAr)
        : FProxyArchive(InInnerAr)
    {
        SetFlag(EArchiverFlags::SaveGame);
    }

    FArchive& FSaveGameArchive::operator<<(CObject*& Value)
    {
        if (IsWriting())
        {
            EObjectRecord Kind = EObjectRecord::Null;
            if (Value == nullptr)
            {
                *this << Kind;
                return *this;
            }

            if (!IsWrittenInline(Value))
            {
                Kind = EObjectRecord::Asset;
                FGuid Guid = Value->GetGUID();
                *this << Kind << Guid;
                return *this;
            }

            if (const auto Found = WrittenObjects.find(Value); Found != WrittenObjects.end())
            {
                Kind = EObjectRecord::Repeat;
                uint32 Index = Found->second;
                *this << Kind << Index;
                return *this;
            }

            // Recorded before the payload, so an object that reaches itself writes a repeat instead of recursing.
            WrittenObjects.emplace(Value, (uint32)WrittenObjects.size());
            Kind = EObjectRecord::Inline;
            FName ClassName = Value->GetClass()->GetName();
            *this << Kind << ClassName;
            WriteSized(*this, [&] { SaveGame::SerializeObjectState(*this, Value); });
            return *this;
        }

        EObjectRecord Kind = EObjectRecord::Null;
        *this << Kind;
        switch (Kind)
        {
        case EObjectRecord::Null:
            {
                Value = nullptr;
                break;
            }
        case EObjectRecord::Asset:
            {
                FGuid Guid;
                *this << Guid;
                Value = Guid.IsValid() ? FindObject<CObject>(Guid) : nullptr;
                if (Value == nullptr && Guid.IsValid())
                {
                    Value = LoadObject<CObject>(Guid);
                }
                break;
            }
        case EObjectRecord::Repeat:
            {
                uint32 Index = 0;
                *this << Index;
                Value = Index < ReadObjects.size() ? ReadObjects[Index] : nullptr;
                break;
            }
        case EObjectRecord::Inline:
            {
                FName ClassName;
                int64 Size = 0;
                *this << ClassName << Size;
                const int64 Start = Tell();

                CClass* Class = FScriptableRegistry::ResolveClass(ClassName);
                CObject* Object = Class != nullptr ? NewObject(Class, nullptr, NAME_None, FGuid::New(), OF_Transient) : nullptr;
                ReadObjects.push_back(Object);
                if (Object != nullptr)
                {
                    SaveGame::SerializeObjectState(*this, Object);
                }
                else
                {
                    LOG_WARN("Save game: class '{}' no longer exists, so an object of it loads as null.", ClassName.c_str());
                }
                Seek(Start + Size);
                Value = Object;
                break;
            }
        default:
            {
                LOG_ERROR("Save game: unknown object record {}, the file is damaged.", (uint32)Kind);
                SetHasError(true);
                Value = nullptr;
                break;
            }
        }
        return *this;
    }

    FArchive& FSaveGameArchive::operator<<(FObjectHandle& Value)
    {
        CObject* Object = Value.IsValid() ? Value.Resolve() : nullptr;
        *this << Object;
        if (IsReading())
        {
            Value = FObjectHandle(Object);
        }
        return *this;
    }

    void FSaveGameArchive::SerializeEntityId(uint32& PackedEntity)
    {
        InnerArchive.SerializeEntityId(PackedEntity);
        if (IsReading() && TranslateEntity)
        {
            const ECS::FEntity Saved = ECS::FEntity::FromPacked(PackedEntity);
            if (!Saved.IsNull())
            {
                PackedEntity = TranslateEntity(Saved).Value;
            }
        }
    }

    namespace SaveGame
    {
        void SerializeProperties(FArchive& Ar, const CStruct* Struct, void* Data, ESaveGameProperties Which)
        {
            if (Ar.IsWriting())
            {
                const int64 CountPosition = Ar.Tell();
                uint32 Count = 0;
                Ar << Count;
                for (FProperty* Property : Struct->GetProperties())
                {
                    if (!Wants(Property, Which))
                    {
                        continue;
                    }
                    FName Name = Property->GetPropertyName();
                    uint8 Type = (uint8)Property->GetType();
                    Ar << Name << Type;
                    WriteSized(Ar, [&] { Property->Serialize(Ar, Property->GetValuePtr<void>(Data)); });
                    ++Count;
                }
                const int64 End = Ar.Tell();
                Ar.Seek(CountPosition);
                Ar << Count;
                Ar.Seek(End);
                return;
            }

            uint32 Count = 0;
            Ar << Count;
            if (!Ar.CanHoldCount(Count, sizeof(int64)))
            {
                LOG_ERROR("Save game: a property block claims {} entries, more than the file holds.", Count);
                Ar.SetHasError(true);
                return;
            }

            for (uint32 Index = 0; Index < Count && !Ar.HasError(); ++Index)
            {
                FName Name;
                uint8 Type = 0;
                int64 Size = 0;
                Ar << Name << Type << Size;
                const int64 Start = Ar.Tell();

                FProperty* Property = Struct->GetProperty(Name);
                if (Property != nullptr && (uint8)Property->GetType() == Type)
                {
                    Property->Serialize(Ar, Property->GetValuePtr<void>(Data));
                }
                else if (Property != nullptr)
                {
                    LOG_WARN("Save game: '{}.{}' changed type since it was saved, so it keeps its current value.",
                        Struct->GetName().c_str(), Name.c_str());
                }
                Ar.Seek(Start + Size);
            }
        }

        void SerializeObjectState(FArchive& Ar, CObject* Object)
        {
            Object->Serialize(Ar);
            SerializeProperties(Ar, Object->GetClass(), Object, ESaveGameProperties::SaveGameOnly);
        }

        bool HasSaveGameProperties(const CStruct* Struct)
        {
            if (Struct == nullptr)
            {
                return false;
            }
            for (const FProperty* Property : Struct->GetProperties())
            {
                if (Property->IsSaveGame())
                {
                    return true;
                }
            }
            return false;
        }
    }
}
