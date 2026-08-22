#include "RuntimePCH.h"
#include "StructuredArchive.h"

namespace Lumina
{
    FArchiveRecord FArchiveSlot::EnterRecord()
    {
        int32 ElementIdx = StructuredArchive.EnterSlotAsType(*this, StructuredArchive::EElementType::Record);
        StructuredArchive.EnterRecord();
        return FArchiveRecord(StructuredArchive, Depth + 1, StructuredArchive.CurrentScope[ElementIdx].ID);
    }

    FArchiveArray FArchiveSlot::EnterArray(int32& NumElements)
    {
        int32 ElementIdx = StructuredArchive.EnterSlotAsType(*this, StructuredArchive::EElementType::Array);
        StructuredArchive.EnterArray(NumElements);
        return FArchiveArray(StructuredArchive, Depth + 1, StructuredArchive.CurrentScope[ElementIdx].ID);
    }

    FArchiveStream FArchiveSlot::EnterStream()
    {
        int32 ElementIdx = StructuredArchive.EnterSlotAsType(*this, StructuredArchive::EElementType::Stream);
        StructuredArchive.EnterStream();
        return FArchiveStream(StructuredArchive, Depth + 1, StructuredArchive.CurrentScope[ElementIdx].ID);
    }

    FArchiveMap FArchiveSlot::EnterMap(int32& NumElements)
    {
        int32 ElementIdx = StructuredArchive.EnterSlotAsType(*this, StructuredArchive::EElementType::Map);
        StructuredArchive.EnterMap(NumElements);
        return FArchiveMap(StructuredArchive, Depth + 1, StructuredArchive.CurrentScope[ElementIdx].ID);
    }

    void FArchiveSlot::Serialize(uint8& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(uint16& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(uint32& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(uint64& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(int8& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(int16& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(int32& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(int64& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(float& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(double& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(bool& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(FString& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(FName& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(CObject*& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(FObjectHandle& Value)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr << Value;
        StructuredArchive.LeaveSlot();
    }

    void FArchiveSlot::Serialize(void* Data, uint64 DataSize)
    {
        StructuredArchive.EnterSlot(*this);
        StructuredArchive.InnerAr.Serialize(Data, (int64)DataSize);
        StructuredArchive.LeaveSlot();
    }

    FArchive& FArchiveSlot::GetArchiver() const
    {
        return StructuredArchive.GetInnerAr();
    }

    FArchiveRecord::~FArchiveRecord()
    {
        StructuredArchive.LeaveRecord();
    }

    FArchiveSlot FArchiveRecord::EnterField(FName FieldName)
    {
        return StructuredArchive.EnterField(FieldName);
    }

    FArchiveArray::~FArchiveArray()
    {
        StructuredArchive.LeaveArray();
    }

    FArchiveSlot FArchiveArray::EnterElement()
    {
        return StructuredArchive.EnterArrayElement();
    }

    FArchiveStream::~FArchiveStream()
    {
        StructuredArchive.LeaveStream();
    }

    FArchiveSlot FArchiveStream::EnterElement()
    {
        return StructuredArchive.EnterStreamElement();
    }

    FArchiveMap::~FArchiveMap()
    {
        StructuredArchive.LeaveMap();
    }

    FArchiveSlot FArchiveMap::EnterKey()
    {
        return StructuredArchive.EnterMapKey();
    }

    FArchiveSlot FArchiveMap::EnterValue()
    {
        return StructuredArchive.EnterMapValue();
    }

    FArchiveSlot IStructuredArchive::Open()
    {
        ASSERT(RootElementID == 0);

        RootElementID = IDGenerator.Generate();
        CurrentScope.emplace_back(RootElementID, StructuredArchive::EElementType::Root);

        CurrentSlotID = IDGenerator.Generate();
        
        return FArchiveSlot(*this, 0, CurrentSlotID);
    }

    void IStructuredArchive::Close()
    {
        while (!CurrentScope.empty())
        {
            CurrentScope.pop_back();
        }
        
        RootElementID = 0;
        CurrentSlotID = 0;
    }

    void IStructuredArchive::SetScope(FSlotPosition Slot)
    {
        ASSERT(Slot.Depth < CurrentScope.size() && CurrentScope[Slot.Depth].ID == Slot.ID);

        CurrentScope.erase(CurrentScope.begin() + Slot.Depth + 1, CurrentScope.end());
        CurrentSlotID = Slot.ID;
    }

    int32 IStructuredArchive::EnterSlotAsType(FSlotPosition Slot, StructuredArchive::EElementType Type)
    {
        EnterSlot(Slot, Type == StructuredArchive::EElementType::AttributedValue);

        int32 NewSlotDepth = Slot.Depth;

        if (NewSlotDepth < (int32)CurrentScope.size() &&
            CurrentScope[NewSlotDepth].Type == StructuredArchive::EElementType::AttributedValue)
        {
            ++NewSlotDepth;
        }

        if (NewSlotDepth >= (int32)CurrentScope.size())
        {
            StructuredArchive::FSlotID NewID = IDGenerator.Generate();
            CurrentScope.emplace_back(NewID, Type);
            CurrentSlotID = NewID;
        }
        else
        {
            CurrentScope[NewSlotDepth].Type = Type;
            CurrentSlotID = CurrentScope[NewSlotDepth].ID;
        }

        return NewSlotDepth;
    }

    void IStructuredArchive::EnterSlot(FSlotPosition Slot, bool bEnteringAttributedValue)
    {
        SetScope(Slot);
    }

    FBinaryStructuredArchive::FBinaryStructuredArchive(FArchive& InAr)
        : IStructuredArchive(InAr)
    {
    }

    void FBinaryStructuredArchive::EnterSlot(FSlotPosition Slot, bool bEnteringAttributedValue)
    {
        IStructuredArchive::EnterSlot(Slot, bEnteringAttributedValue);
        // Binary format encodes structure implicitly via read/write order.
    }

    void FBinaryStructuredArchive::LeaveSlot()
    {
    }

    void FBinaryStructuredArchive::EnterRecord()
    {
        if (IsSaving())
        {

        }
    }

    void FBinaryStructuredArchive::LeaveRecord()
    {
        if (!CurrentScope.empty() && CurrentScope.back().Type == StructuredArchive::EElementType::Record)
        {
            CurrentScope.pop_back();
        }
    }

    FArchiveSlot FBinaryStructuredArchive::EnterField(FName FieldName)
    {
        if (IsSaving())
        {
            InnerAr << FieldName;
        }
        else if (IsLoading())
        {
            FName ReadFieldName;
            InnerAr << ReadFieldName;
            ASSERT(ReadFieldName == FieldName);
        }

        StructuredArchive::FSlotID NewSlotID = IDGenerator.Generate();
        uint32 NewDepth = (uint32)CurrentScope.size();
        
        return FArchiveSlot(*this, NewDepth, NewSlotID);
    }

    void FBinaryStructuredArchive::LeaveField()
    {
    }

    void FBinaryStructuredArchive::EnterArray(int32& NumElements)
    {
        InnerAr << NumElements;
        
        if (IsLoading())
        {
            ASSERT(NumElements >= 0);
        }
    }

    void FBinaryStructuredArchive::LeaveArray()
    {
        if (!CurrentScope.empty() && CurrentScope.back().Type == StructuredArchive::EElementType::Array)
        {
            CurrentScope.pop_back();
        }
    }

    FArchiveSlot FBinaryStructuredArchive::EnterArrayElement()
    {
        StructuredArchive::FSlotID NewSlotID = IDGenerator.Generate();
        uint32 NewDepth = (uint32)CurrentScope.size();
        
        return FArchiveSlot(*this, NewDepth, NewSlotID);
    }

    void FBinaryStructuredArchive::EnterStream()
    {
    }

    void FBinaryStructuredArchive::LeaveStream()
    {
        if (!CurrentScope.empty() && CurrentScope.back().Type == StructuredArchive::EElementType::Stream)
        {
            CurrentScope.pop_back();
        }
    }

    FArchiveSlot FBinaryStructuredArchive::EnterStreamElement()
    {
        StructuredArchive::FSlotID NewSlotID = IDGenerator.Generate();
        uint32 NewDepth = (uint32)CurrentScope.size();
        
        return FArchiveSlot(*this, NewDepth, NewSlotID);
    }

    void FBinaryStructuredArchive::EnterMap(int32& NumElements)
    {
        InnerAr << NumElements;
        
        if (IsLoading())
        {
            ASSERT(NumElements >= 0);
        }
    }

    void FBinaryStructuredArchive::LeaveMap()
    {
        if (!CurrentScope.empty() && CurrentScope.back().Type == StructuredArchive::EElementType::Map)
        {
            CurrentScope.pop_back();
        }
    }

    FArchiveSlot FBinaryStructuredArchive::EnterMapKey()
    {
        StructuredArchive::FSlotID NewSlotID = IDGenerator.Generate();
        uint32 NewDepth = (uint32)CurrentScope.size();
        
        return FArchiveSlot(*this, NewDepth, NewSlotID);
    }

    FArchiveSlot FBinaryStructuredArchive::EnterMapValue()
    {
        StructuredArchive::FSlotID NewSlotID = IDGenerator.Generate();
        uint32 NewDepth = (uint32)CurrentScope.size();
        
        return FArchiveSlot(*this, NewDepth, NewSlotID);
    }
}
