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
        Capture(Before);
    }

    void FObjectSnapshotCommand::Finalize()
    {
        Capture(After);
    }

    void FObjectSnapshotCommand::Capture(TVector<uint8>& Out) const
    {
        Out.clear();

        CObject* Obj = Object.Get();
        if (Obj == nullptr)
        {
            return;
        }

        FMemoryWriter Writer(Out);
        FObjectProxyArchiver Ar(Writer, false);
        Obj->GetClass()->SerializeTaggedProperties(Ar, Obj);
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
        Obj->GetClass()->SerializeTaggedProperties(Ar, Obj);

        Obj->PostPropertyChange(nullptr);
        if (CPackage* Package = Obj->GetPackage())
        {
            Package->MarkDirty();
        }
    }

    void FObjectSnapshotCommand::Undo() { Restore(Before); }
    void FObjectSnapshotCommand::Redo() { Restore(After); }
}
