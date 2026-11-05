#include "PropertySnapshot.h"

#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    FPropertySnapshot::~FPropertySnapshot()
    {
        Reset();
    }

    void FPropertySnapshot::Reset()
    {
        for (int32 Index = 0; Index < Fields.Num(); ++Index)
        {
            if (IsTracked(Index))
            {
                Fields[Index]->DestructValue(Slot(Index));
            }
        }
        Fields.Reset();
        Offsets.Reset();
        Storage.Reset();
        bCaptured = false;
    }

    void FPropertySnapshot::Capture(const TVector<FProperty*>& InFields, const void* Container)
    {
        Reset();
        Fields = InFields;

        size_t Size = 0;
        for (const FProperty* Field : Fields)
        {
            Offsets.Add(Size);
            Size += (Field->GetElementSize() + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) * sizeof(std::max_align_t);
            if (Field->GetElementSize() == 0)
            {
                LOG_WARN("FPropertySnapshot: '{}' does not report its size, so changes to it are not detected.", Field->GetPropertyName());
            }
        }
        Storage.SetNum(IntCastChecked<int32>(Size / sizeof(std::max_align_t) + 1));

        for (int32 Index = 0; Index < Fields.Num(); ++Index)
        {
            if (!IsTracked(Index))
            {
                continue;
            }
            Fields[Index]->ConstructValue(Slot(Index));
            Fields[Index]->CopyCompleteValue(Slot(Index), Fields[Index]->GetValuePtr<void>(Container));
        }
        bCaptured = true;
    }

    void* FPropertySnapshot::Slot(int32 Index)
    {
        return reinterpret_cast<uint8*>(Storage.GetData()) + Offsets[Index];
    }

    bool FPropertySnapshot::IsTracked(int32 Index) const
    {
        return Fields[Index]->GetElementSize() != 0;
    }

    bool FPropertySnapshot::Matches(int32 Index, const void* Container)
    {
        return !IsTracked(Index) || Fields[Index]->Identical(Slot(Index), Fields[Index]->GetValuePtr<void>(Container));
    }

    void FPropertySnapshot::Store(int32 Index, const void* Container)
    {
        if (IsTracked(Index))
        {
            Fields[Index]->CopyCompleteValue(Slot(Index), Fields[Index]->GetValuePtr<void>(Container));
        }
    }

    int32 FPropertySnapshot::IndexOf(const FProperty* InField) const
    {
        for (int32 Index = 0; Index < Fields.Num(); ++Index)
        {
            if (Fields[Index] == InField)
            {
                return (int32)Index;
            }
        }
        return -1;
    }
}
