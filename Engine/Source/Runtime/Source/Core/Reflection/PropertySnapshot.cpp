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
        for (size_t Index = 0; Index < Fields.size(); ++Index)
        {
            if (IsTracked(Index))
            {
                Fields[Index]->DestructValue(Slot(Index));
            }
        }
        Fields.clear();
        Offsets.clear();
        Storage.clear();
        bCaptured = false;
    }

    void FPropertySnapshot::Capture(const TVector<FProperty*>& InFields, const void* Container)
    {
        Reset();
        Fields = InFields;

        size_t Size = 0;
        for (const FProperty* Field : Fields)
        {
            Offsets.push_back(Size);
            Size += (Field->GetElementSize() + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) * sizeof(std::max_align_t);
            if (Field->GetElementSize() == 0)
            {
                LOG_WARN("FPropertySnapshot: '{}' does not report its size, so changes to it are not detected.", Field->GetPropertyName());
            }
        }
        Storage.resize(Size / sizeof(std::max_align_t) + 1);

        for (size_t Index = 0; Index < Fields.size(); ++Index)
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

    void* FPropertySnapshot::Slot(size_t Index)
    {
        return reinterpret_cast<uint8*>(Storage.data()) + Offsets[Index];
    }

    bool FPropertySnapshot::IsTracked(size_t Index) const
    {
        return Fields[Index]->GetElementSize() != 0;
    }

    bool FPropertySnapshot::Matches(size_t Index, const void* Container)
    {
        return !IsTracked(Index) || Fields[Index]->Identical(Slot(Index), Fields[Index]->GetValuePtr<void>(Container));
    }

    void FPropertySnapshot::Store(size_t Index, const void* Container)
    {
        if (IsTracked(Index))
        {
            Fields[Index]->CopyCompleteValue(Slot(Index), Fields[Index]->GetValuePtr<void>(Container));
        }
    }

    int32 FPropertySnapshot::IndexOf(const FProperty* InField) const
    {
        for (size_t Index = 0; Index < Fields.size(); ++Index)
        {
            if (Fields[Index] == InField)
            {
                return (int32)Index;
            }
        }
        return -1;
    }
}
