#pragma once
#include "StructuredArchive.h"
#include "nlohmann/json.hpp"

namespace Lumina
{
    class CStruct;

    // Inner flat archive the structured layer writes leaf values through. Each leaf
    // Serialize() routes here via the (virtual) FArchive operators; we read/write the
    // node the structured layer parked in PendingNode.
    class RUNTIME_API FJsonLeafArchive : public FArchive
    {
    public:

        nlohmann::json* PendingNode = nullptr;
        bool            bLoading    = false;

        bool IsReading() const override { return bLoading; }
        bool IsWriting() const override { return !bLoading; }

        template<typename T>
        void ReadWrite(T& Value)
        {
            if (PendingNode == nullptr) { return; }
            if (bLoading)
            {
                if (!PendingNode->is_null()) { Value = PendingNode->get<T>(); }
            }
            else
            {
                *PendingNode = Value;
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
                if (PendingNode->is_string()) { Str = PendingNode->get<std::string>().c_str(); }
            }
            else
            {
                *PendingNode = std::string(Str.c_str());
            }
            return *this;
        }

        FArchive& operator<<(FName& Name) override
        {
            if (PendingNode == nullptr) { return *this; }
            if (bLoading)
            {
                if (PendingNode->is_string())
                {
                    const std::string Str = PendingNode->get<std::string>();
                    // JSON carries no file version; files written before None's wire form became
                    // the empty string stored its "NAME_None" display rendering.
                    Name = Str == "NAME_None" ? FName() : FName(Str.c_str());
                }
            }
            else
            {
                *PendingNode = std::string(Name.IsNone() ? "" : Name.c_str());
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

        FJsonStructuredArchive(nlohmann::json& InRoot, bool bLoading);

        // Serialize a reflected struct/object's properties to/from the given JSON node.
        static void SaveStruct(nlohmann::json& OutNode, const CStruct* Type, void* Data);
        static void LoadStruct(nlohmann::json& InNode,  const CStruct* Type, void* Data);

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
            nlohmann::json* Node;
            bool            bArray;
            size_t          ReadIndex;
        };

        nlohmann::json* CurrentContainer() { return Containers.empty() ? RootNode : Containers.back().Node; }

        nlohmann::json*             RootNode = nullptr;
        nlohmann::json              MissingNode;            // null sentinel for absent keys on load
        TVector<FContainer>         Containers;
    };
}
