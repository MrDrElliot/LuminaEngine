#include "Core/Templates/NumericLimits.h"
#include "RuntimePCH.h"
#include <limits>
#include "ArrayProperty.h"
#include "StructProperty.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"

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
            TFixedVector<FName, 4> Path;
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
            return Type >= EPropertyTypeFlags::Int8 && Type <= EPropertyTypeFlags::Bool;
        }

        // Every leaf of a struct made only of numbers, or false when any field needs its own serializer.
        bool CollectPlainFields(const CStruct* Struct, uint32 BaseOffset, TFixedVector<FName, 4>& Path, TVector<FPlainField>& Out)
        {
            if (Struct == nullptr || (Struct->GetStructOps() != nullptr && Struct->GetStructOps()->HasSerializer()))
            {
                return false;
            }

            for (FProperty* Property : Struct->GetProperties())
            {
                if (!Property->ShouldSerialize() || Property->IsEditorOnly() || Path.size() >= MaxPlainFieldDepth)
                {
                    return false;
                }

                Path.push_back(Property->GetPropertyName());
                const uint32 Offset = BaseOffset + Property->Offset;
                bool bPlain = true;

                if (IsPlainNumeric(Property->GetType()))
                {
                    Out.push_back(FPlainField{ Path, Property->GetType(), Offset, (uint32)Property->GetElementSize() });
                }
                else if (Property->GetType() == EPropertyTypeFlags::Struct)
                {
                    bPlain = CollectPlainFields(static_cast<FStructProperty*>(Property)->GetStruct(), Offset, Path, Out);
                }
                else
                {
                    bPlain = false;
                }

                Path.pop_back();
                if (!bPlain || Out.size() > MaxPlainFields)
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

        bool BuildPlainLayout(const FProperty* Inner, SIZE_T Stride, TVector<FPlainField>& Out)
        {
            TFixedVector<FName, 4> Path;
            if (!CollectPlainFields(GetPlainElementStruct(Inner), 0, Path, Out) || Out.empty())
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
            for (size_t Depth = 0; Depth < Saved.Path.size(); ++Depth)
            {
                FProperty* Property = Struct != nullptr ? Struct->GetProperty(Saved.Path[Depth]) : nullptr;
                if (Property == nullptr)
                {
                    return false;
                }
                Offset += Property->Offset;

                if (Depth + 1 == Saved.Path.size())
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
            case EPropertyTypeFlags::UInt32: { uint32 V; memcpy(&V, Source, sizeof(V)); return V; }
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
            case EPropertyTypeFlags::UInt64: StorePlainNumber<uint64>(Dest, Value); break;
            case EPropertyTypeFlags::Float:  StorePlainNumber<float>(Dest, Value);  break;
            case EPropertyTypeFlags::Double: StorePlainNumber<double>(Dest, Value); break;
            case EPropertyTypeFlags::Bool:   StorePlainNumber<bool>(Dest, Value != 0.0); break;
            default:                         break;
            }
        }

        bool SameLayout(const TVector<FPlainField>& A, const TVector<FPlainField>& B)
        {
            if (A.size() != B.size())
            {
                return false;
            }
            for (size_t i = 0; i < A.size(); ++i)
            {
                if (A[i].Type != B[i].Type || A[i].Offset != B[i].Offset || A[i].Size != B[i].Size || A[i].Path.size() != B[i].Path.size())
                {
                    return false;
                }
                for (size_t Depth = 0; Depth < A[i].Path.size(); ++Depth)
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
            SIZE_T Count = Array.GetNum(Value);
            const SIZE_T Stride = Array.GetOps()->ElementSize;
            SIZE_T FlaggedStride = Stride | PlainStructArrayFlag;
            Ar << Count;
            Ar << FlaggedStride;

            uint32 NumFields = (uint32)Layout.size();
            Ar << NumFields;
            for (FPlainField& Field : Layout)
            {
                uint8 Depth = (uint8)Field.Path.size();
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

            TVector<uint8> Covered(Stride, 0);
            for (const FPlainField& Field : Layout)
            {
                memset(Covered.data() + Field.Offset, 1, Field.Size);
            }
            TFixedVector<FByteRange, 8> Gaps;
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
                Gaps.push_back({ Start, Byte - Start });
            }

            uint8* Data = static_cast<uint8*>(Array.GetAt(Value, 0));
            if (Gaps.empty())
            {
                Ar.Serialize(Data, static_cast<int64>(Count * Stride));
                return;
            }

            const SIZE_T PerChunk = Stride < PlainChunkBytes ? PlainChunkBytes / Stride : 1;
            TVector<uint8> Chunk(PerChunk * Stride);
            for (SIZE_T First = 0; First < Count; First += PerChunk)
            {
                const SIZE_T Num = (Count - First) < PerChunk ? (Count - First) : PerChunk;
                memcpy(Chunk.data(), Data + First * Stride, Num * Stride);
                for (SIZE_T Element = 0; Element < Num; ++Element)
                {
                    uint8* Bytes = Chunk.data() + Element * Stride;
                    for (const FByteRange& Gap : Gaps)
                    {
                        memset(Bytes + Gap.Start, 0, Gap.Length);
                    }
                }
                Ar.Serialize(Chunk.data(), static_cast<int64>(Num * Stride));
            }
        }

        void ReadPlainStructs(FArchive& Ar, const FArrayProperty& Array, void* Value, SIZE_T Count, SIZE_T SavedStride)
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
                Field.Path.resize(Depth);
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

            const FProperty* Inner = Array.GetInternalProperty();
            const CStruct* Struct = GetPlainElementStruct(Inner);
            if (Struct == nullptr)
            {
                LOG_WARN("Array property '{}' no longer holds structs, skipping its {} saved elements.", Array.Name, Count);
                Array.Resize(Value, 0);
                SkipBytes(Ar, Count * SavedStride);
                return;
            }

            Array.Resize(Value, Count);
            if (Count == 0)
            {
                return;
            }

            const SIZE_T Stride = Array.GetOps()->ElementSize;
            TVector<FPlainField> Current;
            if (SavedStride == Stride && BuildPlainLayout(Inner, Stride, Current) && SameLayout(Saved, Current))
            {
                Ar.Serialize(Array.GetAt(Value, 0), static_cast<int64>(Count * Stride));
                return;
            }

            TVector<FPlainFieldMap> Maps;
            Maps.reserve(Saved.size());
            for (const FPlainField& Field : Saved)
            {
                FPlainFieldMap Map{ Field.Offset, 0, Field.Type, EPropertyTypeFlags::None, Field.Size };
                uint32 ToSize = 0;
                if (ResolvePlainField(Struct, Field, Map.To, Map.ToType, ToSize) && (SIZE_T)Map.To + ToSize <= Stride)
                {
                    Maps.push_back(Map);
                }
                else
                {
                    LOG_WARN("Array property '{}' dropped saved field '{}', which the struct no longer has.", Array.Name, Field.Path.back());
                }
            }

            uint8* Data = static_cast<uint8*>(Array.GetAt(Value, 0));
            const SIZE_T PerChunk = SavedStride < PlainChunkBytes ? PlainChunkBytes / SavedStride : 1;
            TVector<uint8> Chunk(PerChunk * SavedStride);
            for (SIZE_T First = 0; First < Count && !Ar.HasError(); First += PerChunk)
            {
                const SIZE_T Num = (Count - First) < PerChunk ? (Count - First) : PerChunk;
                Ar.Serialize(Chunk.data(), static_cast<int64>(Num * SavedStride));
                for (SIZE_T Element = 0; Element < Num; ++Element)
                {
                    const uint8* Source = Chunk.data() + Element * SavedStride;
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
            uint32 Num = static_cast<uint32>(GetNum(Value));
            Ar.SerializeBits(&Num, 32);
            for (uint32 i = 0; i < Num; ++i)
            {
                Inner->NetSerialize(Ar, GetAt(Value, i));
            }
        }
        else
        {
            uint32 Num = 0;
            Ar.SerializeBits(&Num, 32);
            Resize(Value, Num);
            for (uint32 i = 0; i < Num && !Ar.HasError(); ++i)
            {
                Inner->NetSerialize(Ar, GetAt(Value, i));
            }
        }
    }

    void FArrayProperty::Serialize(FArchive& Ar, void* Value)
    {
        // A struct of plain numbers writes its schema once, which is what keeps a million-element array cheap.
        if (Ar.IsWriting() && GetNum(Value) != 0)
        {
            TVector<FPlainField> Layout;
            if (BuildPlainLayout(Inner, Ops->ElementSize, Layout))
            {
                WritePlainStructs(Ar, *this, Value, Layout);
                return;
            }
        }

        SIZE_T ElementCount = GetNum(Value);
        Ar << ElementCount;
        
        size_t SerializedInnerElementSize = Inner->GetElementSize();
        Ar << SerializedInnerElementSize;

        const size_t CurrentInnerElementSize = Inner->GetElementSize();

        // Checked before anything derived from the count is trusted, including the skip below.
        if (ElementCount > TNumericLimits<uint32>::Max())
        {
            // Failing the archive stops the cascade at the first real symptom instead of misaligning the rest.
            LOG_ERROR("Array property '{}' tried to serialize {} elements; failing the archive.", Name, ElementCount);
            Ar.SetHasError(true);
            return;
        }

        if (Ar.IsReading() && (SerializedInnerElementSize & PlainStructArrayFlag) != 0)
        {
            ReadPlainStructs(Ar, *this, Value, ElementCount, SerializedInnerElementSize & ~PlainStructArrayFlag);
            return;
        }

        // Trivial types memcpy in bulk so size must match; non-trivial types tolerate in-memory padding diffs.
        if (Ar.IsReading() && Inner->IsTrivial() && SerializedInnerElementSize != CurrentInnerElementSize)
        {
            LOG_ERROR("Inner element size changed for array '{}' (inner '{}'), skipping it: Current=({}) Serialized=({})", Name, Inner->Name, CurrentInnerElementSize, SerializedInnerElementSize);

            // Chunked through Serialize rather than Seek, since Seek is a no-op on the base archive.
            Resize(Value, 0);

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
                for (SIZE_T i = 0; i < ElementCount; i++)
                {
                    Inner->Serialize(Ar, GetAt(Value, i));
                }   
            }
        }
        else
        {
            if (Inner->IsTrivial() && ElementCount)
            {
                Resize(Value, ElementCount);
                Ar.Serialize(GetAt(Value, 0), static_cast<int64>(ElementCount * InnerElementSize));
            }
            else
            {
                Resize(Value, ElementCount);
                for (SIZE_T i = 0; i < ElementCount; ++i)
                {
                    Inner->Serialize(Ar, GetAt(Value, i));
                }
            }
        }
    }

    void FArrayProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        int32 NumElements = (int32)GetNum(Value);
        FArchiveArray Array = Slot.EnterArray(NumElements);

        if (Slot.GetArchiver().IsReading())
        {
            Resize(Value, (size_t)NumElements);
        }

        for (int32 i = 0; i < NumElements; ++i)
        {
            Inner->SerializeItem(Array.EnterElement(), GetAt(Value, (size_t)i));
        }
    }

    bool FArrayProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        const SIZE_T NumA = GetNum(ValueA);
        const SIZE_T NumB = GetNum(ValueB);
        if (NumA != NumB)
        {
            return false;
        }

        for (SIZE_T i = 0; i < NumA; ++i)
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
        const SIZE_T SrcCount = GetNum(Src);
        Resize(Dst, SrcCount);
        for (SIZE_T i = 0; i < SrcCount; ++i)
        {
            void* DstElem = GetAt(Dst, i);
            const void* SrcElem = GetAt(const_cast<void*>(Src), i);
            Inner->CopyCompleteValue(DstElem, SrcElem);
        }
    }
}
