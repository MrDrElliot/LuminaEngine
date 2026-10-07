#include "ObjectSnapshotCommand.h"

#include "Core/Object/Object.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"

namespace Lumina
{
    FObjectSnapshotCommand::FObjectSnapshotCommand(CObject* InObject, FName InName)
        : Object(InObject)
        , Name(InName)
    {
        Capture(InObject, Before);
        CollectReferenced(InObject, Referenced);
    }

    FObjectSnapshotCommand::FObjectSnapshotCommand(CObject* InObject, FName InName, TVector<uint8> InBefore, TVector<uint8> InAfter,
        FSerializer InSerializer, TVector<TStrongObjectPtr<CObject>> InReferenced)
        : Object(InObject)
        , Name(InName)
        , Before(Move(InBefore))
        , After(Move(InAfter))
        , bAfterCaptured(true)
        , Serializer(Move(InSerializer))
        , Referenced(Move(InReferenced))
    {
        CollectReferenced(InObject, Referenced);
    }

    void FObjectSnapshotCommand::CollectReferenced(CObject* Object, TVector<TStrongObjectPtr<CObject>>& Out)
    {
        if (Object == nullptr)
        {
            return;
        }

        FObjectReferenceVisitor::VisitStruct(Object->GetClass(), Object, [&Out](CObject* Referenced) -> CObject*
        {
            if (Referenced != nullptr)
            {
                Out.push_back(Referenced);
            }
            return Referenced;
        });
    }

    void FObjectSnapshotCommand::Finalize()
    {
        if (!bAfterCaptured)
        {
            Capture(Object.Get(), After, Serializer);
            CollectReferenced(Object.Get(), Referenced);
            bAfterCaptured = true;
        }
    }

    void FObjectSnapshotCommand::Capture(CObject* Obj, TVector<uint8>& Out, const FSerializer& Serializer)
    {
        LUMINA_PROFILE_SCOPE();

        Out.clear();
        if (Obj == nullptr)
        {
            return;
        }

        FMemoryWriter Writer(Out);
        FObjectProxyArchiver Ar(Writer, false);
        if (Serializer)
        {
            Serializer(Ar, Obj);
        }
        else
        {
            Obj->GetClass()->SerializeTaggedProperties(Ar, Obj);
        }
    }

    void FObjectSnapshotCommand::Restore(const TVector<uint8>& In) const
    {
        CObject* Obj = Object.Get();
        if (Obj == nullptr || In.empty())
        {
            return;
        }

        FMemoryReader Reader(In);
        FObjectProxyArchiver Ar(Reader, true);
        if (Serializer)
        {
            Serializer(Ar, Obj);
        }
        else
        {
            Obj->GetClass()->SerializeTaggedProperties(Ar, Obj);
        }

        Obj->PostPropertyChange(nullptr);
        if (CPackage* Package = Obj->GetPackage())
        {
            Package->MarkDirty();
        }
    }

    void FObjectSnapshotCommand::Undo() { Restore(Before); }
    void FObjectSnapshotCommand::Redo() { Restore(After); }
}
