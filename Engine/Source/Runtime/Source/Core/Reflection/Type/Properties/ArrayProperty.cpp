#include "RuntimePCH.h"
#include "ArrayProperty.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"

namespace Lumina
{
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
        SIZE_T ElementCount = GetNum(Value);
        Ar << ElementCount;
        
        size_t SerializedInnerElementSize = Inner->GetElementSize();
        Ar << SerializedInnerElementSize;

        const size_t CurrentInnerElementSize = Inner->GetElementSize();

        // Checked before anything derived from the count is trusted, including the skip below.
        if (ElementCount > eastl::numeric_limits<uint32>::max())
        {
            // The count is the only record of how much payload follows, so a garbage one leaves no way to
            // step over it. Returning here used to leave the archive misaligned and every later property
            // read garbage, turning one bad array into a cascade of them; failing the archive stops that
            // at the first real symptom instead.
            LOG_ERROR("Array property '{}' tried to serialize {} elements; failing the archive.", Name, ElementCount);
            Ar.SetHasError(true);
            return;
        }

        // Trivial types memcpy in bulk so size must match; non-trivial types tolerate in-memory padding diffs.
        if (Ar.IsReading() && Inner->IsTrivial() && SerializedInnerElementSize != CurrentInnerElementSize)
        {
            LOG_ERROR("Inner element size changed for array '{}' (inner '{}'), skipping it: Current=({}) Serialized=({})", Name, Inner->Name, CurrentInnerElementSize, SerializedInnerElementSize);

            // The count is trustworthy here, so the payload can be stepped over exactly and the rest of the
            // object still loads. Chunked through Serialize rather than Seek because Seek is a no-op on the
            // base archive, and not every reader overrides it.
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
