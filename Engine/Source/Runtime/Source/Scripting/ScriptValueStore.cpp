#include "RuntimePCH.h"
#include "ScriptValueStore.h"

#include "Core/Object/Class.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Memory/Memory.h"
#include "Scripting/ScriptStruct.h"

namespace Lumina
{
    SScriptValueStore::~SScriptValueStore()
    {
        Reset();
    }

    SScriptValueStore::SScriptValueStore(const SScriptValueStore& Other)
    {
        Layout = Other.Layout;
        Serialized = Other.Serialized;
        if (Other.Buffer != nullptr && Other.Layout.Get() != nullptr)
        {
            const uint32 Size = Other.Layout->GetAlignedSize();
            if (Size > 0)
            {
                Buffer = static_cast<uint8*>(Memory::Malloc(Size, Other.Layout->GetAlignment()));
                Other.Layout->ConstructInto(Buffer);
                Other.Layout->CopyInto(Buffer, Other.Buffer);
            }
        }
    }

    SScriptValueStore& SScriptValueStore::operator=(const SScriptValueStore& Other)
    {
        if (this != &Other)
        {
            Reset();
            Layout = Other.Layout;
            Serialized = Other.Serialized;
            if (Other.Buffer != nullptr && Other.Layout.Get() != nullptr)
            {
                const uint32 Size = Other.Layout->GetAlignedSize();
                if (Size > 0)
                {
                    Buffer = static_cast<uint8*>(Memory::Malloc(Size, Other.Layout->GetAlignment()));
                    Other.Layout->ConstructInto(Buffer);
                    Other.Layout->CopyInto(Buffer, Other.Buffer);
                }
            }
        }
        return *this;
    }

    SScriptValueStore::SScriptValueStore(SScriptValueStore&& Other) noexcept
        : Layout(std::move(Other.Layout))
        , Buffer(Other.Buffer)
        , Serialized(std::move(Other.Serialized))
    {
        Other.Buffer = nullptr;
    }

    SScriptValueStore& SScriptValueStore::operator=(SScriptValueStore&& Other) noexcept
    {
        if (this != &Other)
        {
            Reset();
            Layout = std::move(Other.Layout);
            Buffer = Other.Buffer;
            Serialized = std::move(Other.Serialized);
            Other.Buffer = nullptr;
        }
        return *this;
    }

    void SScriptValueStore::Reset()
    {
        if (Buffer != nullptr && Layout.Get() != nullptr)
        {
            Layout->DestructIn(Buffer);
        }
        if (Buffer != nullptr)
        {
            Memory::Free((void*&)Buffer);
            Buffer = nullptr;
        }
        Layout = nullptr;
        Serialized.clear();
    }

    void SScriptValueStore::EnsureLayout(const CScriptStruct* NewLayout)
    {
        CScriptStruct* New = const_cast<CScriptStruct*>(NewLayout);
        if (Layout.Get() == New && Buffer != nullptr)
        {
            return;
        }

        // Capture live values as tagged bytes so a layout change migrates them by name.
        if (Buffer != nullptr && Layout.Get() != nullptr)
        {
            Serialized.clear();
            FMemoryWriter Writer(Serialized);
            Layout->SerializeTaggedProperties(Writer, Buffer);
            Layout->DestructIn(Buffer);
            Memory::Free((void*&)Buffer);
            Buffer = nullptr;
        }

        Layout = New;

        if (New != nullptr)
        {
            const uint32 Size = New->GetAlignedSize();
            if (Size > 0)
            {
                Buffer = static_cast<uint8*>(Memory::Malloc(Size, New->GetAlignment()));
                New->ConstructInto(Buffer);
                if (const void* Def = New->GetDefaults())
                {
                    New->CopyInto(Buffer, Def);
                }
                if (!Serialized.empty())
                {
                    FMemoryReader Reader(Serialized);
                    New->SerializeTaggedProperties(Reader, Buffer);
                }
            }
            Serialized.clear();
        }
    }

    bool SScriptValueStore::Serialize(FArchive& Ar)
    {
        if (Ar.IsWriting())
        {
            TVector<uint8> Bytes;
            if (Buffer != nullptr && Layout.Get() != nullptr)
            {
                FMemoryWriter Writer(Bytes);
                Layout->SerializeTaggedProperties(Writer, Buffer);
            }
            else
            {
                Bytes = Serialized;
            }
            uint32 Count = (uint32)Bytes.size();
            Ar << Count;
            if (Count > 0)
            {
                Ar.Serialize(Bytes.data(), (int64)Count);
            }
        }
        else
        {
            uint32 Count = 0;
            Ar << Count;
            Serialized.clear();
            if (Count > 0 && Count <= Ar.GetMaxSerializeSize())
            {
                Serialized.resize(Count);
                Ar.Serialize(Serialized.data(), (int64)Count);
            }
        }
        return true;
    }
}
