#include "RuntimePCH.h"
#include "JsonStructuredArchive.h"

#include "Core/Object/Class.h"
#include "Core/Object/Object.h"

namespace Lumina
{
    using json = nlohmann::json;

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
            *PendingNode = Value ? std::string(Value->GetName().c_str()) : std::string();
        }
        return *this;
    }

    FArchive& FJsonLeafArchive::operator<<(FObjectHandle& Value)
    {
        // Settings don't persist live object handles; no-op.
        return *this;
    }


    FJsonStructuredArchive::FJsonStructuredArchive(json& InRoot, bool bLoading)
        : Private::FJsonLeafHolder()
        , IStructuredArchive(Leaf)
    {
        RootNode = &InRoot;
        Leaf.PendingNode = RootNode;
        Leaf.bLoading = bLoading;
        Leaf.SetFlag(bLoading ? EArchiverFlags::Reading : EArchiverFlags::Writing);
    }

    void FJsonStructuredArchive::SaveStruct(json& OutNode, const CStruct* Type, void* Data)
    {
        FJsonStructuredArchive Ar(OutNode, false);
        FArchiveSlot Root = Ar.Open();
        FArchiveRecord Record = Root.EnterRecord();
        Type->SerializeTaggedProperties(Record, Data);
    }

    void FJsonStructuredArchive::LoadStruct(json& InNode, const CStruct* Type, void* Data)
    {
        FJsonStructuredArchive Ar(InNode, true);
        FArchiveSlot Root = Ar.Open();
        FArchiveRecord Record = Root.EnterRecord();
        Type->SerializeTaggedProperties(Record, Data);
    }

    void FJsonStructuredArchive::EnterRecord()
    {
        json* Node = Leaf.PendingNode;
        if (IsSaving() && Node)
        {
            *Node = json::object();
        }
        Containers.push_back({ Node, false, 0 });
    }

    void FJsonStructuredArchive::LeaveRecord()
    {
        if (!Containers.empty()) { Containers.pop_back(); }
        if (!CurrentScope.empty() && CurrentScope.back().Type == StructuredArchive::EElementType::Record)
        {
            CurrentScope.pop_back();
        }
    }

    FArchiveSlot FJsonStructuredArchive::EnterField(FName FieldName)
    {
        json* Container = CurrentContainer();
        const std::string Key = FieldName.c_str();

        if (IsSaving())
        {
            // CurrentContainer() returns null for a null RootNode or a container entered on a null node,
            // and this dereferenced it unconditionally.
            Leaf.PendingNode = Container ? &(*Container)[Key] : &MissingNode;
        }
        else
        {
            if (Container && Container->is_object())
            {
                auto It = Container->find(Key);
                Leaf.PendingNode = (It != Container->end()) ? &(*It) : &MissingNode;
            }
            else
            {
                Leaf.PendingNode = &MissingNode;
            }
        }

        return FArchiveSlot(*this, CurrentScope.size(), IDGenerator.Generate());
    }

    void FJsonStructuredArchive::EnterArray(int32& NumElements)
    {
        json* Node = Leaf.PendingNode;
        if (IsSaving())
        {
            if (Node) { *Node = json::array(); }
        }
        else
        {
            NumElements = (Node && Node->is_array()) ? (int32)Node->size() : 0;
        }
        Containers.push_back({ Node, true, 0 });
    }

    void FJsonStructuredArchive::LeaveArray()
    {
        if (!Containers.empty()) { Containers.pop_back(); }
        if (!CurrentScope.empty() && CurrentScope.back().Type == StructuredArchive::EElementType::Array)
        {
            CurrentScope.pop_back();
        }
    }

    FArchiveSlot FJsonStructuredArchive::EnterArrayElement()
    {
        // Unbalanced Enter/Leave would make this back() UB, the way every other method here already
        // guards for. Route to the sentinel instead so the caller writes somewhere harmless.
        if (Containers.empty())
        {
            Leaf.PendingNode = &MissingNode;
            return FArchiveSlot(*this, CurrentScope.size(), IDGenerator.Generate());
        }

        FContainer& C = Containers.back();
        json* Node = C.Node;

        if (IsSaving())
        {
            if (Node)
            {
                Node->push_back(json());
                Leaf.PendingNode = &Node->back();
            }
            else
            {
                // MUST reassign. Leaving PendingNode alone left it pointing at the PREVIOUS element,
                // whose storage a prior push_back has already reallocated (json arrays are vectors), so
                // the caller's EnterRecord then wrote an object through a dangling pointer and the next
                // push_back crashed on the corrupted heap.
                Leaf.PendingNode = &MissingNode;
            }
        }
        else
        {
            const size_t Index = C.ReadIndex++;
            Leaf.PendingNode = (Node && Node->is_array() && Index < Node->size()) ? &(*Node)[Index] : &MissingNode;
        }

        return FArchiveSlot(*this, CurrentScope.size(), IDGenerator.Generate());
    }

    void FJsonStructuredArchive::EnterStream()
    {
        int32 Unused = 0;
        EnterArray(Unused);
    }

    void FJsonStructuredArchive::LeaveStream()
    {
        if (!Containers.empty()) { Containers.pop_back(); }
        if (!CurrentScope.empty() && CurrentScope.back().Type == StructuredArchive::EElementType::Stream)
        {
            CurrentScope.pop_back();
        }
    }

    FArchiveSlot FJsonStructuredArchive::EnterStreamElement()
    {
        return EnterArrayElement();
    }

    // Maps are represented as a JSON array of [key, value] pairs. No reflected property
    // type currently drives this path; implemented for interface completeness.
    void FJsonStructuredArchive::EnterMap(int32& NumElements)
    {
        json* Node = Leaf.PendingNode;
        if (IsSaving())
        {
            if (Node) { *Node = json::array(); }
        }
        else
        {
            NumElements = (Node && Node->is_array()) ? (int32)Node->size() : 0;
        }
        Containers.push_back({ Node, true, 0 });
    }

    void FJsonStructuredArchive::LeaveMap()
    {
        if (!Containers.empty()) { Containers.pop_back(); }
        if (!CurrentScope.empty() && CurrentScope.back().Type == StructuredArchive::EElementType::Map)
        {
            CurrentScope.pop_back();
        }
    }

    FArchiveSlot FJsonStructuredArchive::EnterMapKey()
    {
        FContainer& C = Containers.back();
        json* Node = C.Node;
        if (IsSaving())
        {
            if (Node)
            {
                Node->push_back(json::array({ json(), json() }));
                Leaf.PendingNode = &Node->back()[0];
            }
        }
        else
        {
            const size_t Index = C.ReadIndex;
            Leaf.PendingNode = (Node && Node->is_array() && Index < Node->size()) ? &(*Node)[Index][0] : &MissingNode;
        }
        return FArchiveSlot(*this, CurrentScope.size(), IDGenerator.Generate());
    }

    FArchiveSlot FJsonStructuredArchive::EnterMapValue()
    {
        FContainer& C = Containers.back();
        json* Node = C.Node;
        if (IsSaving())
        {
            if (Node) { Leaf.PendingNode = &Node->back()[1]; }
        }
        else
        {
            const size_t Index = C.ReadIndex++;
            Leaf.PendingNode = (Node && Node->is_array() && Index < Node->size()) ? &(*Node)[Index][1] : &MissingNode;
        }
        return FArchiveSlot(*this, CurrentScope.size(), IDGenerator.Generate());
    }
}
