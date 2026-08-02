#pragma once
#include "Containers/Array.h"
#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    class FName;
    class FArchiveRecord;
    class FArchiveArray;
    class FArchiveStream;
    class FArchiveMap;

    namespace StructuredArchive
    {
        using FSlotID = uint64;

        enum class EElementType : uint8
        {
            Root,
            Record,
            Array,
            Stream,
            Map,
            AttributedValue,
        };
        
        template<typename T>
        class TNamedValue
        {
        public:
            TNamedValue(const FName& InName, T& InValue)
                : Name(InName)
                , Value(InValue)
            {}
            
            FName Name;
            T& Value;
        };
    }
    
    class RUNTIME_API FSlotPosition
    {
    public:
        FSlotPosition() = default;

        FSlotPosition(uint32 InDepth, StructuredArchive::FSlotID InID = 0)
            : Depth(InDepth)
            , ID(InID)
        {}

        uint32 Depth = 0;
        StructuredArchive::FSlotID ID = 0;
    };

    class IStructuredArchive;
    
    class FArchiveSlot : protected FSlotPosition
    {
        friend class IStructuredArchive;
        friend class FArchiveRecord;
        friend class FArchiveArray;

    public:
        FArchiveSlot(IStructuredArchive& InAr, uint32 InDepth, StructuredArchive::FSlotID InID)
            : FSlotPosition(InDepth, InID)
            , StructuredArchive(InAr)
        {}

        RUNTIME_API FArchiveRecord EnterRecord();
        RUNTIME_API FArchiveArray EnterArray(int32& NumElements);
        RUNTIME_API FArchiveStream EnterStream();
        RUNTIME_API FArchiveMap EnterMap(int32& NumElements);

        void Serialize(uint8& Value);
        void Serialize(uint16& Value);
        void Serialize(uint32& Value);
        void Serialize(uint64& Value);
        void Serialize(int8& Value);
        void Serialize(int16& Value);
        void Serialize(int32& Value);
        void Serialize(int64& Value);
        void Serialize(float& Value);
        void Serialize(double& Value);
        void Serialize(bool& Value);
        void Serialize(FString& Value);
        void Serialize(FName& Value);
        void Serialize(CObject*& Value);
        void Serialize(FObjectHandle& Value);
        void Serialize(void* Data, uint64 DataSize);
        FArchive& GetArchiver() const;

        template<typename T>
        FORCEINLINE void operator<<(StructuredArchive::TNamedValue<T> Item);

        template<typename T>
        void Serialize(TVector<T>& Value);
        
    

    protected:
        
        IStructuredArchive& StructuredArchive;
    };

    class RUNTIME_API FArchiveRecord : protected FSlotPosition
    {
        friend class FArchiveSlot;
        friend class FArchiveArray;

    public:
        FArchiveRecord(IStructuredArchive& InAr, uint32 InDepth, StructuredArchive::FSlotID InID)
            : FSlotPosition(InDepth, InID)
            , StructuredArchive(InAr)
        {}

        ~FArchiveRecord();

        FArchiveSlot EnterField(FName FieldName);

        template<typename T>
        FORCEINLINE void operator<<(StructuredArchive::TNamedValue<T> Item)
        {
            FArchiveSlot Field = EnterField(Item.Name);
            Field.Serialize(Item.Value);
        }

    protected:
        IStructuredArchive& StructuredArchive;
    };

    class FArchiveArray : protected FSlotPosition
    {
        friend class FArchiveSlot;

    public:
        FArchiveArray(IStructuredArchive& InAr, uint32 InDepth, StructuredArchive::FSlotID InID)
            : FSlotPosition(InDepth, InID)
            , StructuredArchive(InAr)
        {}

        ~FArchiveArray();

        FArchiveSlot EnterElement();

    protected:
        
        IStructuredArchive& StructuredArchive;
    };

    class FArchiveStream : protected FSlotPosition
    {
        friend class FArchiveSlot;

    public:
        FArchiveStream(IStructuredArchive& InAr, uint32 InDepth, StructuredArchive::FSlotID InID)
            : FSlotPosition(InDepth, InID)
            , StructuredArchive(InAr)
        {}

        ~FArchiveStream();

        FArchiveSlot EnterElement();

    protected:
        IStructuredArchive& StructuredArchive;
    };

    class FArchiveMap : protected FSlotPosition
    {
        friend class FArchiveSlot;

    public:
        FArchiveMap(IStructuredArchive& InAr, uint32 InDepth, StructuredArchive::FSlotID InID)
            : FSlotPosition(InDepth, InID)
            , StructuredArchive(InAr)
        {}

        ~FArchiveMap();

        RUNTIME_API FArchiveSlot EnterKey();

        RUNTIME_API FArchiveSlot EnterValue();

    protected:
        IStructuredArchive& StructuredArchive;
    };

    class IStructuredArchive
    {
        friend class FArchiveSlot;
        friend class FArchiveRecord;
        friend class FArchiveArray;
        friend class FArchiveStream;
        friend class FArchiveMap;
    
    public:
        using FSlot     = FArchiveSlot;
        using FRecord   = FArchiveRecord;

        struct FIDGenerator
        {
            uint64 Generate()
            {
                return NextID++;
            }
            
            uint64 NextID = 1;
        };

        struct FElement
        {
            StructuredArchive::FSlotID ID;
            StructuredArchive::EElementType Type;

            FElement(StructuredArchive::FSlotID InID, StructuredArchive::EElementType InType)
                : ID(InID)
                , Type(InType)
            {}
        };

        virtual ~IStructuredArchive() = default;

        IStructuredArchive(FArchive& InInnerAr)
            : RootElementID(0)
            , CurrentSlotID(0)
            , InnerAr(InInnerAr)
        {}

        /** Begin writing at the root slot. */
        RUNTIME_API FArchiveSlot Open();

        RUNTIME_API void Close();

        RUNTIME_API void SetScope(FSlotPosition Slot);

        RUNTIME_API int32 EnterSlotAsType(FSlotPosition Slot, StructuredArchive::EElementType Type);

        virtual void EnterSlot(FSlotPosition Slot, bool bEnteringAttributedValue = false);
        virtual void LeaveSlot() = 0;

        virtual void EnterRecord() = 0;
        virtual void LeaveRecord() = 0;
        virtual FArchiveSlot EnterField(FName FieldName) = 0;
        virtual void LeaveField() = 0;

        virtual void EnterArray(int32& NumElements) = 0;
        virtual void LeaveArray() = 0;
        virtual FArchiveSlot EnterArrayElement() = 0;

        virtual void EnterStream() = 0;
        virtual void LeaveStream() = 0;
        virtual FArchiveSlot EnterStreamElement() = 0;

        virtual void EnterMap(int32& NumElements) = 0;
        virtual void LeaveMap() = 0;
        virtual FArchiveSlot EnterMapKey() = 0;
        virtual FArchiveSlot EnterMapValue() = 0;

        FArchive& GetInnerAr() const { return InnerAr; }
        NODISCARD bool IsLoading() const { return InnerAr.IsReading(); }
        NODISCARD bool IsSaving() const { return InnerAr.IsWriting(); }

    protected:
        TFixedVector<FElement, 32>      CurrentScope;
        FIDGenerator                    IDGenerator;
        StructuredArchive::FSlotID      RootElementID;
        StructuredArchive::FSlotID      CurrentSlotID;
        FArchive&                       InnerAr;
    };

    class FBinaryStructuredArchive final : public IStructuredArchive
    {
    public:
        RUNTIME_API FBinaryStructuredArchive(FArchive& InAr);

        virtual void EnterSlot(FSlotPosition Slot, bool bEnteringAttributedValue = false) override;
        virtual void LeaveSlot() override;

        virtual void EnterRecord() override;
        virtual void LeaveRecord() override;
        virtual FArchiveSlot EnterField(FName FieldName) override;
        virtual void LeaveField() override;

        virtual void EnterArray(int32& NumElements) override;
        virtual void LeaveArray() override;
        virtual FArchiveSlot EnterArrayElement() override;

        virtual void EnterStream() override;
        virtual void LeaveStream() override;
        virtual FArchiveSlot EnterStreamElement() override;

        virtual void EnterMap(int32& NumElements) override;
        virtual void LeaveMap() override;
        virtual FArchiveSlot EnterMapKey() override;
        virtual FArchiveSlot EnterMapValue() override;

    private:
        
        struct FFieldInfo
        {
            FName   FieldName;
            uint64  Offset;
        };

        THashMap<StructuredArchive::FSlotID, TVector<FFieldInfo>> RecordFields;
    };

    template <typename T>
    void FArchiveSlot::operator<<(StructuredArchive::TNamedValue<T> Item)
    {
        FArchiveRecord Record = EnterRecord();
        FArchiveSlot Field = Record.EnterField(Item.Name);
        Field.Serialize(Item.Value);
    }

    template<typename T>
    void FArchiveSlot::Serialize(TVector<T>& Value)
    {
        int32 NumElements = Value.Num();
        FArchiveArray Array = EnterArray(NumElements);
        
        if (StructuredArchive.IsLoading())
        {
            Value.SetNum(NumElements);
        }
        
        for (int32 i = 0; i < NumElements; ++i)
        {
            FArchiveSlot ElementSlot = Array.EnterElement();
            ElementSlot.Serialize(Value[i]);
        }
    }

    // Helper macro for easy field serialization
    #define SERIALIZE_NAMED_FIELD(Archive, Field) Archive << StructuredArchive::TNamedValue(#Field, Field)
}