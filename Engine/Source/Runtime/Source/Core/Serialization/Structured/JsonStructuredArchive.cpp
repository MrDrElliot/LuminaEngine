#include "RuntimePCH.h"
#include "JsonStructuredArchive.h"

#include "Core/Object/Class.h"
#include "Core/Object/Object.h"

namespace Lumina
{
    FArchive& FJsonLeafArchive::operator<<(CObject*& Value)
    {
        if (PendingNode == nullptr) { return *this; }
        if (bLoading)
        {
            // Path-based resolve is not wired for settings; leave unset.
            Value = nullptr;
        }
        else
        {
            *PendingNode = Value ? FJsonValue(Value->GetName().ToString()) : FJsonValue("");
        }
        return *this;
    }

    FArchive& FJsonLeafArchive::operator<<(FObjectHandle& Value)
    {
        // Settings don't persist live object handles; no-op.
        return *this;
    }


    FJsonStructuredArchive::FJsonStructuredArchive(FJsonValue& InRoot, bool bLoading)
        : Private::FJsonLeafHolder()
        , IStructuredArchive(Leaf)
    {
        RootNode = &InRoot;
        Leaf.PendingNode = RootNode;
        Leaf.bLoading = bLoading;
        Leaf.SetFlag(bLoading ? EArchiverFlags::Reading : EArchiverFlags::Writing);
    }

    void FJsonStructuredArchive::SaveStruct(FJsonValue& OutNode, const CStruct* Type, void* Data)
    {
        FJsonStructuredArchive Ar(OutNode, false);
        FArchiveSlot Root = Ar.Open();
        FArchiveRecord Record = Root.EnterRecord();
        Type->SerializeProperties(Record, Data);
    }

    void FJsonStructuredArchive::LoadStruct(FJsonValue& InNode, const CStruct* Type, void* Data)
    {
        FJsonStructuredArchive Ar(InNode, true);
        FArchiveSlot Root = Ar.Open();
        FArchiveRecord Record = Root.EnterRecord();
        Type->SerializeProperties(Record, Data);
    }

    void FJsonStructuredArchive::EnterRecord()
    {
        FJsonValue* Node = Leaf.PendingNode;
        if (IsSaving() && Node)
        {
            *Node = FJsonValue::MakeObject();
        }
        Containers.Emplace(Node, false, 0);
    }

    void FJsonStructuredArchive::LeaveRecord()
    {
        if (!Containers.IsEmpty()) { Containers.Pop(); }
        if (!CurrentScope.IsEmpty() && CurrentScope.Last().Type == StructuredArchive::EElementType::Record)
        {
            CurrentScope.Pop();
        }
    }

    FArchiveSlot FJsonStructuredArchive::EnterField(FName FieldName)
    {
        FJsonValue* Container = CurrentContainer();
        const FString Key = FieldName.ToString();

        if (IsSaving())
        {
            // CurrentContainer returns null for a null root, and this dereferenced it unconditionally.
            Leaf.PendingNode = Container && (Container->IsObject() || Container->IsNull()) ? &Container->FindOrAdd(Key.View()) : &MissingNode;
        }
        else
        {
            FJsonValue* Found = Container ? Container->Find(Key.View()) : nullptr;
            Leaf.PendingNode = Found != nullptr ? Found : &MissingNode;
        }

        return FArchiveSlot(*this, (uint32)CurrentScope.Num(), IDGenerator.Generate());
    }

    void FJsonStructuredArchive::EnterArray(int32& NumElements)
    {
        FJsonValue* Node = Leaf.PendingNode;
        if (IsSaving())
        {
            if (Node) { *Node = FJsonValue::MakeArray(); }
        }
        else
        {
            NumElements = (Node && Node->IsArray()) ? Node->Num() : 0;
        }
        Containers.Emplace(Node, true, 0);
    }

    void FJsonStructuredArchive::LeaveArray()
    {
        if (!Containers.IsEmpty()) { Containers.Pop(); }
        if (!CurrentScope.IsEmpty() && CurrentScope.Last().Type == StructuredArchive::EElementType::Array)
        {
            CurrentScope.Pop();
        }
    }

    FArchiveSlot FJsonStructuredArchive::EnterArrayElement()
    {
        // Routes to the sentinel so an unbalanced Enter and Leave writes somewhere harmless.
        if (Containers.IsEmpty())
        {
            Leaf.PendingNode = &MissingNode;
            return FArchiveSlot(*this, (uint32)CurrentScope.Num(), IDGenerator.Generate());
        }

        FContainer& C = Containers.Last();
        FJsonValue* Node = C.Node;

        if (IsSaving())
        {
            if (Node)
            {
                Leaf.PendingNode = &Node->Add(FJsonValue());
            }
            else
            {
                // A prior push_back reallocates, so leaving PendingNode alone writes through a dangle.
                Leaf.PendingNode = &MissingNode;
            }
        }
        else
        {
            const int32 Index = C.ReadIndex++;
            Leaf.PendingNode = (Node && Node->IsArray() && Index < Node->Num()) ? &Node->AsArray()[Index] : &MissingNode;
        }

        return FArchiveSlot(*this, (uint32)CurrentScope.Num(), IDGenerator.Generate());
    }

    void FJsonStructuredArchive::EnterStream()
    {
        int32 Unused = 0;
        EnterArray(Unused);
    }

    void FJsonStructuredArchive::LeaveStream()
    {
        if (!Containers.IsEmpty()) { Containers.Pop(); }
        if (!CurrentScope.IsEmpty() && CurrentScope.Last().Type == StructuredArchive::EElementType::Stream)
        {
            CurrentScope.Pop();
        }
    }

    FArchiveSlot FJsonStructuredArchive::EnterStreamElement()
    {
        return EnterArrayElement();
    }

    // No reflected property type drives this path, and it exists for interface completeness.
    void FJsonStructuredArchive::EnterMap(int32& NumElements)
    {
        FJsonValue* Node = Leaf.PendingNode;
        if (IsSaving())
        {
            if (Node) { *Node = FJsonValue::MakeArray(); }
        }
        else
        {
            NumElements = (Node && Node->IsArray()) ? Node->Num() : 0;
        }
        Containers.Emplace(Node, true, 0);
    }

    void FJsonStructuredArchive::LeaveMap()
    {
        if (!Containers.IsEmpty()) { Containers.Pop(); }
        if (!CurrentScope.IsEmpty() && CurrentScope.Last().Type == StructuredArchive::EElementType::Map)
        {
            CurrentScope.Pop();
        }
    }

    FArchiveSlot FJsonStructuredArchive::EnterMapKey()
    {
        FContainer& C = Containers.Last();
        FJsonValue* Node = C.Node;
        if (IsSaving())
        {
            if (Node)
            {
                FJsonValue& Pair = Node->Add(FJsonValue::MakeArray({ FJsonValue(), FJsonValue() }));
                Leaf.PendingNode = &Pair.AsArray()[0];
            }
        }
        else
        {
            const int32 Index = C.ReadIndex;
            FJsonValue* Pair = (Node && Node->IsArray() && Index < Node->Num()) ? &Node->AsArray()[Index] : nullptr;
            Leaf.PendingNode = Pair != nullptr && Pair->Num() > 0 && Pair->IsArray() ? &Pair->AsArray()[0] : &MissingNode;
        }
        return FArchiveSlot(*this, (uint32)CurrentScope.Num(), IDGenerator.Generate());
    }

    FArchiveSlot FJsonStructuredArchive::EnterMapValue()
    {
        FContainer& C = Containers.Last();
        FJsonValue* Node = C.Node;
        if (IsSaving())
        {
            if (Node && Node->Num() > 0)
            {
                FJsonValue& Pair = Node->AsArray()[Node->Num() - 1];
                Leaf.PendingNode = &Pair.AsArray()[1];
            }
        }
        else
        {
            const int32 Index = C.ReadIndex++;
            FJsonValue* Pair = (Node && Node->IsArray() && Index < Node->Num()) ? &Node->AsArray()[Index] : nullptr;
            Leaf.PendingNode = Pair != nullptr && Pair->IsArray() && Pair->Num() > 1 ? &Pair->AsArray()[1] : &MissingNode;
        }
        return FArchiveSlot(*this, (uint32)CurrentScope.Num(), IDGenerator.Generate());
    }
}
