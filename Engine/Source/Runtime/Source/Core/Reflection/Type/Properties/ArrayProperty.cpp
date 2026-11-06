#include "Core/Templates/NumericLimits.h"
#include "RuntimePCH.h"
#include <limits>
#include "ArrayProperty.h"
#include "StructProperty.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/PropertyFlags.h"
#include "Core/Versioning/CoreVersion.h"
#include "Scripting/ScriptStruct.h"

namespace Lumina::ArrayPropertyInternal
{
    // An older file stored only a size, so a changed numeric width is read as the same family at the old width.
    EPropertyTypeFlags GuessNumericKind(EPropertyTypeFlags Current, SIZE_T SerializedSize)
    {
        switch (Current)
        {
        case EPropertyTypeFlags::Float:
        case EPropertyTypeFlags::Double:
            return SerializedSize == 4 ? EPropertyTypeFlags::Float : SerializedSize == 8 ? EPropertyTypeFlags::Double : EPropertyTypeFlags::None;
        case EPropertyTypeFlags::Int8:
        case EPropertyTypeFlags::Int16:
        case EPropertyTypeFlags::Int32:
        case EPropertyTypeFlags::Int64:
            switch (SerializedSize)
            {
            case 1:  return EPropertyTypeFlags::Int8;
            case 2:  return EPropertyTypeFlags::Int16;
            case 4:  return EPropertyTypeFlags::Int32;
            case 8:  return EPropertyTypeFlags::Int64;
            default: return EPropertyTypeFlags::None;
            }
        case EPropertyTypeFlags::UInt8:
        case EPropertyTypeFlags::UInt16:
        case EPropertyTypeFlags::UInt32:
        case EPropertyTypeFlags::UInt64:
            switch (SerializedSize)
            {
            case 1:  return EPropertyTypeFlags::UInt8;
            case 2:  return EPropertyTypeFlags::UInt16;
            case 4:  return EPropertyTypeFlags::UInt32;
            case 8:  return EPropertyTypeFlags::UInt64;
            default: return EPropertyTypeFlags::None;
            }
        default:
            return EPropertyTypeFlags::None;
        }
    }

    void DrainBytes(FArchive& Ar, SIZE_T Remaining)
    {
        uint8 Scratch[1024];
        while (Remaining > 0 && !Ar.HasError())
        {
            const SIZE_T Chunk = Remaining < sizeof(Scratch) ? Remaining : sizeof(Scratch);
            Ar.Serialize(Scratch, static_cast<int64>(Chunk));
            Remaining -= Chunk;
        }
    }

    // True when the elements were saved as another kind and have been converted or dropped, rather than read as this one.
    bool ReadRetypedElements(FArchive& Ar, const FArrayProperty& Array, void* Value, int32 Count, SIZE_T SerializedSize, bool bKindRecorded, EPropertyTypeFlags SerializedKind)
    {
        const FProperty* Inner = Array.GetInternalProperty();
        const EPropertyTypeFlags CurrentKind = Inner->GetType();

        EPropertyTypeFlags OldKind = SerializedKind;
        if (!bKindRecorded)
        {
            if (SerializedSize == Inner->GetElementSize() || !IsPropertyNumeric(CurrentKind))
            {
                return false;
            }
            OldKind = GuessNumericKind(CurrentKind, SerializedSize);
        }
        else if (OldKind == CurrentKind)
        {
            return false;
        }

        Array.SetNum(Value, 0);

        if (IsPropertyNumeric(OldKind) && IsPropertyNumeric(CurrentKind) && GetNumericSize(OldKind) == SerializedSize)
        {
            Array.SetNum(Value, Count);
            uint8 Scratch[8] = {};
            int32 Lost = 0;
            for (int32 Index = 0; Index < Count && !Ar.HasError(); ++Index)
            {
                Ar.Serialize(Scratch, static_cast<int64>(SerializedSize));
                FNumericValue Number;
                if (!ReadNumber(OldKind, Scratch, Number) || !WriteNumber(CurrentKind, Array.GetAt(Value, Index), Number))
                {
                    ++Lost;
                }
            }

            LOG_WARN("Array '{}' was saved holding {} and now holds {}; converted its elements ({} did not fit and were left at zero).",
                Array.Name.ToString(), PropertyTypeToString(OldKind), PropertyTypeToString(CurrentKind), Lost);
            return true;
        }

        LOG_WARN("Array '{}' was saved holding {} and now holds {}, which do not convert; its elements were dropped.",
            Array.Name.ToString(), PropertyTypeToString(OldKind), PropertyTypeToString(CurrentKind));

        // A fixed-width element is drained here; anything else is skipped by the tagged reader seeking past the property.
        if (const uint32 FixedSize = OldKind == EPropertyTypeFlags::Bool ? 1u : GetNumericSize(OldKind); FixedSize != 0 && FixedSize == SerializedSize)
        {
            DrainBytes(Ar, Count * SerializedSize);
        }
        return true;
    }
}

namespace Lumina
{
    namespace
    {
        // Set in the serialized element size, which a real size never reaches, to mark the schema-plus-raw format.
        constexpr SIZE_T PlainStructArrayFlag = SIZE_T(1) << 63;

        constexpr uint32 MaxPlainFields     = 1024;
        constexpr uint8  MaxPlainFieldDepth = 16;
        constexpr SIZE_T PlainChunkBytes    = 64 * 1024;
        constexpr SIZE_T MaxPlainStride     = 1 << 20;

        struct FPlainField
        {
            TInlineVector<FName, 4> Path;
            EPropertyTypeFlags     Type = EPropertyTypeFlags::None;
            uint32                 Offset = 0;
            uint32                 Size = 0;
        };

        struct FByteRange
        {
            uint32 Start = 0;
            uint32 Length = 0;
        };

        struct FPlainFieldMap
        {
            uint32             From = 0;
            uint32             To = 0;
            EPropertyTypeFlags FromType = EPropertyTypeFlags::None;
            EPropertyTypeFlags ToType = EPropertyTypeFlags::None;
            uint32             Size = 0;
        };

        bool IsPlainNumeric(EPropertyTypeFlags Type)
        {
            return (Type >= EPropertyTypeFlags::Int8 && Type <= EPropertyTypeFlags::Bool) || Type == EPropertyTypeFlags::Entity;
        }

        // Every leaf of a struct made only of numbers, or false when any field needs its own serializer.
        bool CollectPlainFields(const CStruct* Struct, uint32 BaseOffset, TInlineVector<FName, 4>& Path, TVector<FPlainField>& Out)
        {
            if (Struct == nullptr || (Struct->GetStructOps() != nullptr && Struct->GetStructOps()->HasSerializer()))
            {
                return false;
            }

            for (FProperty* Property : Struct->GetProperties())
            {
                if (!Property->ShouldSerialize() || Property->IsEditorOnly() || Path.Num() >= MaxPlainFieldDepth)
                {
                    return false;
                }

                Path.Add(Property->GetPropertyName());
                const uint32 Offset = BaseOffset + Property->Offset;
                bool bPlain = true;

                if (IsPlainNumeric(Property->GetType()))
                {
                    Out.Emplace(Path, Property->GetType(), Offset, (uint32)Property->GetElementSize());
                }
                else if (Property->GetType() == EPropertyTypeFlags::Struct)
                {
                    bPlain = CollectPlainFields(static_cast<FStructProperty*>(Property)->GetStruct(), Offset, Path, Out);
                }
                else
                {
                    bPlain = false;
                }

                Path.Pop();
                if (!bPlain || static_cast<uint32>(Out.Num()) > MaxPlainFields)
                {
                    return false;
                }
            }
            return true;
        }

        const CStruct* GetPlainElementStruct(const FProperty* Inner)
        {
            return (Inner != nullptr && Inner->GetType() == EPropertyTypeFlags::Struct) ? static_cast<const FStructProperty*>(Inner)->GetStruct() : nullptr;
        }

        // A raw read covers every byte of an element, so it needs a type with nothing unreflected that could own memory.
        bool IsOverwritableStruct(const CStruct* Struct)
        {
            const CScriptStruct* Script = Struct != nullptr ? Cast<CScriptStruct>(Struct) : nullptr;
            if (Script == nullptr || Script->GetNativeBase() != nullptr)
            {
                return false;
            }
            for (FProperty* Property : Script->GetProperties())
            {
                if (Property->GetType() == EPropertyTypeFlags::Struct && !Property->IsTrivial()
                    && !IsOverwritableStruct(static_cast<const FStructProperty*>(Property)->GetStruct()))
                {
                    return false;
                }
            }
            return true;
        }

        bool BuildPlainLayout(const FProperty* Inner, SIZE_T Stride, TVector<FPlainField>& Out)
        {
            TInlineVector<FName, 4> Path;
            if (!CollectPlainFields(GetPlainElementStruct(Inner), 0, Path, Out) || Out.IsEmpty())
            {
                return false;
            }
            for (const FPlainField& Field : Out)
            {
                if ((SIZE_T)Field.Offset + Field.Size > Stride)
                {
                    return false;
                }
            }
            return true;
        }

        // Finds the field an older layout named, through the current struct, so a reshuffled struct still loads.
        bool ResolvePlainField(const CStruct* Struct, const FPlainField& Saved, uint32& OutOffset, EPropertyTypeFlags& OutType, uint32& OutSize)
        {
            uint32 Offset = 0;
            for (int32 Depth = 0; Depth < Saved.Path.Num(); ++Depth)
            {
                FProperty* Property = Struct != nullptr ? Struct->GetProperty(Saved.Path[Depth]) : nullptr;
                if (Property == nullptr)
                {
                    return false;
                }
                Offset += Property->Offset;

                if (Depth + 1 == Saved.Path.Num())
                {
                    if (!IsPlainNumeric(Property->GetType()))
                    {
                        return false;
                    }
                    OutOffset = Offset;
                    OutType = Property->GetType();
                    OutSize = (uint32)Property->GetElementSize();
                    return true;
                }
                if (Property->GetType() != EPropertyTypeFlags::Struct)
                {
                    return false;
                }
                Struct = static_cast<FStructProperty*>(Property)->GetStruct();
            }
            return false;
        }

        double ReadPlainNumber(const uint8* Source, EPropertyTypeFlags Type)
        {
            switch (Type)
            {
            case EPropertyTypeFlags::Int8:   { int8   V; memcpy(&V, Source, sizeof(V)); return V; }
            case EPropertyTypeFlags::Int16:  { int16  V; memcpy(&V, Source, sizeof(V)); return V; }
            case EPropertyTypeFlags::Int32:  { int32  V; memcpy(&V, Source, sizeof(V)); return V; }
            case EPropertyTypeFlags::Int64:  { int64  V; memcpy(&V, Source, sizeof(V)); return (double)V; }
            case EPropertyTypeFlags::UInt8:  { uint8  V; memcpy(&V, Source, sizeof(V)); return V; }
            case EPropertyTypeFlags::UInt16: { uint16 V; memcpy(&V, Source, sizeof(V)); return V; }
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::Entity: { uint32 V; memcpy(&V, Source, sizeof(V)); return V; }
            case EPropertyTypeFlags::UInt64: { uint64 V; memcpy(&V, Source, sizeof(V)); return (double)V; }
            case EPropertyTypeFlags::Float:  { float  V; memcpy(&V, Source, sizeof(V)); return V; }
            case EPropertyTypeFlags::Double: { double V; memcpy(&V, Source, sizeof(V)); return V; }
            case EPropertyTypeFlags::Bool:   { bool   V; memcpy(&V, Source, sizeof(V)); return V ? 1.0 : 0.0; }
            default:                         return 0.0;
            }
        }

        template<typename T>
        void StorePlainNumber(uint8* Dest, double Value)
        {
            const T Converted = static_cast<T>(Value);
            memcpy(Dest, &Converted, sizeof(T));
        }

        void WritePlainNumber(uint8* Dest, EPropertyTypeFlags Type, double Value)
        {
            switch (Type)
            {
            case EPropertyTypeFlags::Int8:   StorePlainNumber<int8>(Dest, Value);   break;
            case EPropertyTypeFlags::Int16:  StorePlainNumber<int16>(Dest, Value);  break;
            case EPropertyTypeFlags::Int32:  StorePlainNumber<int32>(Dest, Value);  break;
            case EPropertyTypeFlags::Int64:  StorePlainNumber<int64>(Dest, Value);  break;
            case EPropertyTypeFlags::UInt8:  StorePlainNumber<uint8>(Dest, Value);  break;
            case EPropertyTypeFlags::UInt16: StorePlainNumber<uint16>(Dest, Value); break;
            case EPropertyTypeFlags::UInt32: StorePlainNumber<uint32>(Dest, Value); break;
            case EPropertyTypeFlags::Entity: StorePlainNumber<uint32>(Dest, static_cast<double>(static_cast<uint32>(static_cast<int64>(Value)))); break;
            case EPropertyTypeFlags::UInt64: StorePlainNumber<uint64>(Dest, Value); break;
            case EPropertyTypeFlags::Float:  StorePlainNumber<float>(Dest, Value);  break;
            case EPropertyTypeFlags::Double: StorePlainNumber<double>(Dest, Value); break;
            case EPropertyTypeFlags::Bool:   StorePlainNumber<bool>(Dest, Value != 0.0); break;
            default:                         break;
            }
        }

        bool SameLayout(const TVector<FPlainField>& A, const TVector<FPlainField>& B)
        {
            if (A.Num() != B.Num())
            {
                return false;
            }
            for (int32 i = 0; i < A.Num(); ++i)
            {
                if (A[i].Type != B[i].Type || A[i].Offset != B[i].Offset || A[i].Size != B[i].Size || A[i].Path.Num() != B[i].Path.Num())
                {
                    return false;
                }
                for (int32 Depth = 0; Depth < A[i].Path.Num(); ++Depth)
                {
                    if (A[i].Path[Depth] != B[i].Path[Depth])
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        void SkipBytes(FArchive& Ar, SIZE_T Remaining)
        {
            // Chunked through Serialize rather than Seek, since Seek is a no-op on the base archive.
            uint8 Scratch[1024];
            while (Remaining > 0 && !Ar.HasError())
            {
                const SIZE_T Chunk = Remaining < sizeof(Scratch) ? Remaining : sizeof(Scratch);
                Ar.Serialize(Scratch, static_cast<int64>(Chunk));
                Remaining -= Chunk;
            }
        }

        // Padding bytes are zeroed on the way out, so saving the same data twice writes the same file.
        void WritePlainStructs(FArchive& Ar, const FArrayProperty& Array, void* Value, TVector<FPlainField>& Layout)
        {
            const int32 Count = Array.Num(Value);
            const SIZE_T Stride = Array.GetOps()->ElementSize;
            uint64 SerializedCount = Count;
            SIZE_T FlaggedStride = Stride | PlainStructArrayFlag;
            Ar << SerializedCount;
            Ar << FlaggedStride;

            uint32 NumFields = (uint32)Layout.Num();
            Ar << NumFields;
            for (FPlainField& Field : Layout)
            {
                uint8 Depth = (uint8)Field.Path.Num();
                Ar << Depth;
                for (FName& Segment : Field.Path)
                {
                    Ar << Segment;
                }
                uint8 Type = (uint8)Field.Type;
                Ar << Type;
                Ar << Field.Offset;
                Ar << Field.Size;
            }

            TVector<uint8> Covered(IntCastChecked<int32>(Stride), 0);
            for (const FPlainField& Field : Layout)
            {
                memset(Covered.GetData() + Field.Offset, 1, Field.Size);
            }
            TInlineVector<FByteRange, 8> Gaps;
            for (uint32 Byte = 0; Byte < (uint32)Stride; )
            {
                if (Covered[Byte])
                {
                    ++Byte;
                    continue;
                }
                const uint32 Start = Byte;
                while (Byte < (uint32)Stride && !Covered[Byte])
                {
                    ++Byte;
                }
                Gaps.Emplace(Start, Byte - Start);
            }

            uint8* Data = static_cast<uint8*>(Array.GetAt(Value, 0));
            if (Gaps.IsEmpty())
            {
                Ar.Serialize(Data, static_cast<int64>(Count * Stride));
                return;
            }

            const int32 PerChunk = Stride < PlainChunkBytes ? static_cast<int32>(PlainChunkBytes / Stride) : 1;
            TVector<uint8> Chunk(IntCastChecked<int32>(PerChunk * Stride));
            for (int32 First = 0; First < Count; First += PerChunk)
            {
                const int32 Num = Math::Min(Count - First, PerChunk);
                memcpy(Chunk.GetData(), Data + First * Stride, Num * Stride);
                for (int32 Element = 0; Element < Num; ++Element)
                {
                    uint8* Bytes = Chunk.GetData() + Element * Stride;
                    for (const FByteRange& Gap : Gaps)
                    {
                        memset(Bytes + Gap.Start, 0, Gap.Length);
                    }
                }
                Ar.Serialize(Chunk.GetData(), static_cast<int64>(Num * Stride));
            }
        }

        void ReadPlainStructs(FArchive& Ar, const FArrayProperty& Array, void* Value, int32 Count, SIZE_T SavedStride)
        {
            uint32 NumFields = 0;
            Ar << NumFields;
            if (NumFields == 0 || NumFields > MaxPlainFields || SavedStride == 0 || SavedStride > MaxPlainStride)
            {
                LOG_ERROR("Array property '{}' has a corrupt field schema ({} fields, stride {}); failing the archive.", Array.Name, NumFields, SavedStride);
                Ar.SetHasError(true);
                return;
            }

            TVector<FPlainField> Saved(NumFields);
            for (FPlainField& Field : Saved)
            {
                uint8 Depth = 0;
                Ar << Depth;
                if (Depth == 0 || Depth > MaxPlainFieldDepth)
                {
                    Ar.SetHasError(true);
                    break;
                }
                Field.Path.SetNum(Depth);
                for (FName& Segment : Field.Path)
                {
                    Ar << Segment;
                }
                uint8 Type = 0;
                Ar << Type;
                Field.Type = (EPropertyTypeFlags)Type;
                Ar << Field.Offset;
                Ar << Field.Size;
                if (!IsPlainNumeric(Field.Type) || Field.Size > sizeof(uint64) || (SIZE_T)Field.Offset + Field.Size > SavedStride)
                {
                    Ar.SetHasError(true);
                    break;
                }
            }
            if (Ar.HasError())
            {
                LOG_ERROR("Array property '{}' has a corrupt field schema; failing the archive.", Array.Name);
                return;
            }

            if (!Ar.CanHoldCount(Count, SavedStride))
            {
                LOG_ERROR("Array property '{}' claims {} elements of {} bytes, more than the archive has left; failing the archive.", Array.Name, Count, SavedStride);
                Ar.SetHasError(true);
                return;
            }

            const FProperty* Inner = Array.GetInternalProperty();
            const CStruct* Struct = GetPlainElementStruct(Inner);
            if (Struct == nullptr)
            {
                LOG_WARN("Array property '{}' no longer holds structs, skipping its {} saved elements.", Array.Name, Count);
                Array.SetNum(Value, 0);
                SkipBytes(Ar, Count * SavedStride);
                return;
            }

            Array.SetNum(Value, Count);
            if (Count == 0)
            {
                return;
            }

            const SIZE_T Stride = Array.GetOps()->ElementSize;
            TVector<FPlainField> Current;
            if (SavedStride == Stride && BuildPlainLayout(Inner, Stride, Current) && SameLayout(Saved, Current)
                && (Inner->IsTrivial() || IsOverwritableStruct(GetPlainElementStruct(Inner))))
            {
                Ar.Serialize(Array.GetAt(Value, 0), static_cast<int64>(Count * Stride));
                return;
            }

            TVector<FPlainFieldMap> Maps;
            Maps.Reserve(Saved.Num());
            for (const FPlainField& Field : Saved)
            {
                FPlainFieldMap Map{ Field.Offset, 0, Field.Type, EPropertyTypeFlags::None, Field.Size };
                uint32 ToSize = 0;
                if (ResolvePlainField(Struct, Field, Map.To, Map.ToType, ToSize) && (SIZE_T)Map.To + ToSize <= Stride)
                {
                    Maps.Add(Map);
                }
                else
                {
                    LOG_WARN("Array property '{}' dropped saved field '{}', which the struct no longer has.", Array.Name, Field.Path.Last());
                }
            }

            uint8* Data = static_cast<uint8*>(Array.GetAt(Value, 0));
            const int32 PerChunk = SavedStride < PlainChunkBytes ? static_cast<int32>(PlainChunkBytes / SavedStride) : 1;
            TVector<uint8> Chunk(IntCastChecked<int32>(PerChunk * SavedStride));
            for (int32 First = 0; First < Count && !Ar.HasError(); First += PerChunk)
            {
                const int32 Num = Math::Min(Count - First, PerChunk);
                Ar.Serialize(Chunk.GetData(), static_cast<int64>(Num * SavedStride));
                for (int32 Element = 0; Element < Num; ++Element)
                {
                    const uint8* Source = Chunk.GetData() + Element * SavedStride;
                    uint8* Dest = Data + (First + Element) * Stride;
                    for (const FPlainFieldMap& Map : Maps)
                    {
                        if (Map.FromType == Map.ToType)
                        {
                            memcpy(Dest + Map.To, Source + Map.From, Map.Size);
                        }
                        else
                        {
                            WritePlainNumber(Dest + Map.To, Map.ToType, ReadPlainNumber(Source + Map.From, Map.FromType));
                        }
                    }
                }
            }
        }
    }

    void FArrayProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        if (Ar.IsWriting())
        {
            const int32 Count = Num(Value);
            uint32 WireCount = static_cast<uint32>(Count);
            Ar.SerializeBits(&WireCount, 32);
            for (int32 i = 0; i < Count; ++i)
            {
                Inner->NetSerialize(Ar, GetAt(Value, i));
            }
        }
        else
        {
            uint32 WireCount = 0;
            Ar.SerializeBits(&WireCount, 32);
            if (!Ar.CanHoldCount(WireCount))
            {
                LOG_ERROR("Array property '{}' was sent {} elements, more than the packet holds; failing the archive.", Name, WireCount);
                Ar.SetHasError(true);
                return;
            }
            const int32 Count = static_cast<int32>(WireCount);
            SetNum(Value, Count);
            for (int32 i = 0; i < Count && !Ar.HasError(); ++i)
            {
                Inner->NetSerialize(Ar, GetAt(Value, i));
            }
        }
    }

    void FArrayProperty::Serialize(FArchive& Ar, void* Value)
    {
        // A struct of plain numbers writes its schema once, which is what keeps a million-element array cheap.
        if (Ar.IsWriting() && Num(Value) != 0)
        {
            TVector<FPlainField> Layout;
            if (BuildPlainLayout(Inner, Ops->ElementSize, Layout))
            {
                WritePlainStructs(Ar, *this, Value, Layout);
                return;
            }
        }

        uint64 SerializedCount = Num(Value);
        Ar << SerializedCount;

        size_t SerializedInnerElementSize = Inner->GetElementSize();
        Ar << SerializedInnerElementSize;

        const size_t CurrentInnerElementSize = Inner->GetElementSize();

        // Checked before anything derived from the count is trusted, including the skip below.
        if (SerializedCount > static_cast<uint64>(TNumericLimits<int32>::Max()))
        {
            // Failing the archive stops the cascade at the first real symptom instead of misaligning the rest.
            LOG_ERROR("Array property '{}' tried to serialize {} elements; failing the archive.", Name, SerializedCount);
            Ar.SetHasError(true);
            return;
        }

        if (!Ar.CanHoldCount(SerializedCount))
        {
            LOG_ERROR("Array property '{}' claims {} elements, more than the archive has left; failing the archive.", Name, SerializedCount);
            Ar.SetHasError(true);
            return;
        }

        const int32 ElementCount = static_cast<int32>(SerializedCount);

        if (Ar.IsReading() && (SerializedInnerElementSize & PlainStructArrayFlag) != 0)
        {
            ReadPlainStructs(Ar, *this, Value, ElementCount, SerializedInnerElementSize & ~PlainStructArrayFlag);
            return;
        }

        uint8 SerializedInnerKind = static_cast<uint8>(Inner->GetType());
        const bool bKindRecorded = Ar.GetFileVersion() >= static_cast<int32>(ELuminaEngineVersion::ARRAY_ELEMENT_KIND);
        if (bKindRecorded)
        {
            Ar << SerializedInnerKind;
        }

        if (Ar.IsReading() && ArrayPropertyInternal::ReadRetypedElements(Ar, *this, Value, ElementCount, SerializedInnerElementSize, bKindRecorded, static_cast<EPropertyTypeFlags>(SerializedInnerKind)))
        {
            return;
        }

        // Trivial types memcpy in bulk so size must match; non-trivial types tolerate in-memory padding diffs.
        if (Ar.IsReading() && Inner->IsTrivial() && SerializedInnerElementSize != CurrentInnerElementSize)
        {
            LOG_ERROR("Inner element size changed for array '{}' (inner '{}'), skipping it: Current=({}) Serialized=({})", Name, Inner->Name, CurrentInnerElementSize, SerializedInnerElementSize);

            // Chunked through Serialize rather than Seek, since Seek is a no-op on the base archive.
            SetNum(Value, 0);

            SIZE_T Remaining = ElementCount * SerializedInnerElementSize;
            uint8 Scratch[1024];
            while (Remaining > 0 && !Ar.HasError())
            {
                const SIZE_T Chunk = Remaining < sizeof(Scratch) ? Remaining : sizeof(Scratch);
                Ar.Serialize(Scratch, static_cast<int64>(Chunk));
                Remaining -= Chunk;
            }

            return;
        }

        const size_t InnerElementSize = CurrentInnerElementSize;

        if (Ar.IsWriting())
        {
            if (Inner->IsTrivial() && ElementCount)
            {
                Ar.Serialize(GetAt(Value, 0), static_cast<int64>(ElementCount * InnerElementSize));
            }
            else
            {
                for (int32 i = 0; i < ElementCount; ++i)
                {
                    Inner->Serialize(Ar, GetAt(Value, i));
                }
            }
        }
        else
        {
            if (Inner->IsTrivial() && ElementCount)
            {
                SetNum(Value, ElementCount);
                Ar.Serialize(GetAt(Value, 0), static_cast<int64>(ElementCount * InnerElementSize));
            }
            else
            {
                SetNum(Value, ElementCount);
                for (int32 i = 0; i < ElementCount; ++i)
                {
                    Inner->Serialize(Ar, GetAt(Value, i));
                }
            }
        }
    }

    void FArrayProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        int32 NumElements = Num(Value);
        FArchiveArray Array = Slot.EnterArray(NumElements);

        if (Slot.GetArchiver().IsReading())
        {
            SetNum(Value, NumElements);
        }

        for (int32 i = 0; i < NumElements; ++i)
        {
            Inner->SerializeItem(Array.EnterElement(), GetAt(Value, i));
        }
    }

    bool FArrayProperty::HasPlainElements() const
    {
        // By kind rather than the Trivial flag, which a property minted for a C# script never carries.
        return Ops != nullptr && Inner != nullptr && (IsPlainNumeric(Inner->GetType()) || Inner->GetType() == EPropertyTypeFlags::Enum)
            && Inner->GetElementSize() == Ops->ElementSize;
    }

    bool FArrayProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        const int32 NumA = Num(ValueA);
        const int32 NumB = Num(ValueB);
        if (NumA != NumB)
        {
            return false;
        }
        if (NumA == 0)
        {
            return true;
        }

        // A plain element already compares byte-wise, so the whole buffer compares in one pass.
        if (HasPlainElements())
        {
            return Memory::Memcmp(Ops->GetData(const_cast<void*>(ValueA)), Ops->GetData(const_cast<void*>(ValueB)), static_cast<SIZE_T>(NumA) * Ops->ElementSize) == 0;
        }

        for (int32 i = 0; i < NumA; ++i)
        {
            const void* ElemA = GetAt(const_cast<void*>(ValueA), i);
            const void* ElemB = GetAt(const_cast<void*>(ValueB), i);
            if (!Inner->Identical(ElemA, ElemB))
            {
                return false;
            }
        }
        return true;
    }

    void FArrayProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        const int32 SrcCount = Num(Src);
        SetNum(Dst, SrcCount);
        if (SrcCount > 0 && HasPlainElements())
        {
            Memory::Memcpy(Ops->GetData(Dst), Ops->GetData(const_cast<void*>(Src)), static_cast<SIZE_T>(SrcCount) * Ops->ElementSize);
            return;
        }
        for (int32 i = 0; i < SrcCount; ++i)
        {
            void* DstElem = GetAt(Dst, i);
            const void* SrcElem = GetAt(const_cast<void*>(Src), i);
            Inner->CopyCompleteValue(DstElem, SrcElem);
        }
    }
}
