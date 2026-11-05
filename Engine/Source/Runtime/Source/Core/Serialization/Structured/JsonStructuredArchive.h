#pragma once

#include <type_traits>

#include "Core/Json/Json.h"
#include "StructuredArchive.h"

namespace Lumina
{
    class CStruct;

    // Inner flat archive the structured layer writes leaf values through. Each leaf
    // Serialize() routes here via the (virtual) FArchive operators; we read/write the
    // node the structured layer parked in PendingNode.
    class RUNTIME_API FJsonLeafArchive : public FArchive
    {
    public:

        using FArchive::operator<<;

        FJsonValue* PendingNode = nullptr;
        bool            bLoading    = false;

        bool IsReading() const override { return bLoading; }
        bool IsWriting() const override { return !bLoading; }

        // Loading accepts a number or a bool for any arithmetic field and casts it to the field type.
        template<typename T>
        void ReadWrite(T& Value)
        {
            if (PendingNode == nullptr) { return; }
            if (!bLoading)
            {
                *PendingNode = FJsonValue(Value);
                return;
            }
            if (PendingNode->IsBool())
            {
                Value = static_cast<T>(PendingNode->AsBool());
            }
            else if (PendingNode->IsNumber())
            {
                if constexpr (std::is_same_v<T, bool>)
                {
                    Value = PendingNode->AsDouble() != 0.0;
                }
                else if constexpr (std::is_floating_point_v<T>)
                {
                    Value = static_cast<T>(PendingNode->AsDouble());
                }
                else if constexpr (std::is_signed_v<T>)
                {
                    Value = static_cast<T>(PendingNode->AsInt64());
                }
                else
                {
                    Value = static_cast<T>(PendingNode->IsUnsigned() ? PendingNode->AsUInt64() : static_cast<uint64>(PendingNode->AsInt64()));
                }
            }
        }

        FArchive& operator<<(uint8&  V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(int8&   V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(uint16& V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(int16&  V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(uint32& V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(int32&  V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(uint64& V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(int64&  V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(float&  V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(double& V) override { ReadWrite(V); return *this; }
        FArchive& operator<<(bool&   V) override { ReadWrite(V); return *this; }

        FArchive& operator<<(FString& Str) override
        {
            if (PendingNode == nullptr) { return *this; }
            if (bLoading)
            {
                if (PendingNode->IsString()) { Str = FString(PendingNode->AsString()); }
            }
            else
            {
                *PendingNode = FJsonValue(Str);
            }
            return *this;
        }

        FArchive& operator<<(FName& Name) override
        {
            if (PendingNode == nullptr) { return *this; }
            if (bLoading)
            {
                if (PendingNode->IsString())
                {
                    // JSON carries no file version; files written before None's wire form became
                    // the empty string stored its "NAME_None" display rendering.
                    const FStringView Text = PendingNode->AsString();
                    Name = Text == "NAME_None" ? FName() : FName(Text);
                }
            }
            else
            {
                *PendingNode = Name.IsNone() ? FJsonValue("") : FJsonValue(Name.ToString());
            }
            return *this;
        }

        // Object refs in settings are stored as their string path (null when unset).
        FArchive& operator<<(CObject*& Value) override;
        FArchive& operator<<(FObjectHandle& Value) override;
    };


    namespace Private
    {
        // base-from-member: constructs the leaf archive before the IStructuredArchive
        // base, which stores it by reference.
        struct FJsonLeafHolder { FJsonLeafArchive Leaf; };
    }

    // JSON backend for IStructuredArchive. Maintains a node stack mirroring the
    // record/array scope; reflected properties serialize through their existing
    // SerializeItem(FSlot) implementations. FName/enums are written as strings;
    // missing keys on load leave the C++ value at its default.
    class RUNTIME_API FJsonStructuredArchive : private Private::FJsonLeafHolder, public IStructuredArchive
    {
    public:

        FJsonStructuredArchive(FJsonValue& InRoot, bool bLoading);

        // Serialize a reflected struct/object's properties to/from the given JSON node.
        static void SaveStruct(FJsonValue& OutNode, const CStruct* Type, void* Data);
        static void LoadStruct(FJsonValue& InNode,  const CStruct* Type, void* Data);

        void EnterSlot(FSlotPosition Slot, bool bEnteringAttributedValue = false) override {}
        void LeaveSlot() override {}

        void EnterRecord() override;
        void LeaveRecord() override;
        FArchiveSlot EnterField(FName FieldName) override;
        void LeaveField() override {}

        void EnterArray(int32& NumElements) override;
        void LeaveArray() override;
        FArchiveSlot EnterArrayElement() override;

        void EnterStream() override;
        void LeaveStream() override;
        FArchiveSlot EnterStreamElement() override;

        void EnterMap(int32& NumElements) override;
        void LeaveMap() override;
        FArchiveSlot EnterMapKey() override;
        FArchiveSlot EnterMapValue() override;

    private:

        struct FContainer
        {
            FJsonValue* Node;
            bool        bArray;
            int32       ReadIndex;
        };

        FJsonValue* CurrentContainer() { return Containers.IsEmpty() ? RootNode : Containers.Last().Node; }

        FJsonValue*                 RootNode = nullptr;
        // Null sentinel for absent keys on load, and the discard sink on save: any node that cannot be
        // resolved routes writes here rather than leaving PendingNode pointing at stale storage.
        FJsonValue              MissingNode;
        TVector<FContainer>         Containers;
    };
}
