#include "RuntimePCH.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "Core/Object/ManagedInstance.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectReferenceProvider.h"
#include "Core/Threading/Sync.h"
#include "Log/Log.h"
#include "Scripting/ScriptCallback.h"
#include "Scripting/ScriptableObject.h"

// A wrapper to a since-freed object throws on access rather than reading reclaimed memory.
LUMINA_DOTNET_EXPORT(int64, ObjectGetHandle)(void* Object)
{
    const Lumina::FObjectHandle Handle(static_cast<Lumina::CObjectBase*>(static_cast<Lumina::CObject*>(Object)));
    return (static_cast<int64>(static_cast<uint32>(Handle.Generation)) << 32)
         | static_cast<int64>(static_cast<uint32>(Handle.Index));
}

LUMINA_DOTNET_EXPORT(void*, ObjectResolve)(int32 Index, int32 Generation)
{
    return Lumina::FObjectHandle(Index, Generation).Resolve();
}

// The object's array entry, which outlives the object, so managed can revalidate without crossing back.
LUMINA_DOTNET_EXPORT(void*, ObjectGetEntry)(void* Object)
{
    return Lumina::Object::Internal::GetEntry(static_cast<Lumina::CObjectBase*>(static_cast<Lumina::CObject*>(Object)));
}

// Field offsets for that revalidation, exported rather than hard-coded so a layout change cannot drift.
LUMINA_DOTNET_EXPORT(int32, ObjectLayoutOffset)(int32 Which)
{
    switch (Which)
    {
        case 0:  return (int32)offsetof(Lumina::Object::Internal::FCObjectEntry, Generation);
        case 1:  return (int32)offsetof(Lumina::Object::Internal::FCObjectEntry, Object);
        case 2:  return (int32)Lumina::CObjectBase::GetFlagsOffsetForInterop();
        case 3:  return (int32)Lumina::OF_MarkedDestroy;
        default: return -1;
    }
}

// Every C# wrap asks here first, so a C# class's own instance is made on demand at this one point.
LUMINA_DOTNET_EXPORT(void*, ObjectGetManagedInstance)(void* Object)
{
    Lumina::CObject* Typed = static_cast<Lumina::CObject*>(Object);
    if (Typed != nullptr && Lumina::ToScriptClass(Typed->GetClass()) != nullptr && !Typed->HasAnyFlag(Lumina::OF_DefaultObject))
    {
        return Lumina::Scriptable::GetOrCreateInstance(Typed);
    }
    return Lumina::ManagedInstances::Find(Typed);
}

LUMINA_DOTNET_EXPORT(void, ObjectSetManagedInstance)(void* Object, void* Handle)
{
    Lumina::ManagedInstances::Set(static_cast<Lumina::CObjectBase*>(static_cast<Lumina::CObject*>(Object)), Handle);
}

// Nonzero when C# took ownership, after which the table holds WeakHandle and C# keeps the object alive.
LUMINA_DOTNET_EXPORT(int32, ObjectAdoptScriptTwin)(void* Object, void* WeakHandle)
{
    return Lumina::ManagedInstances::AdoptScriptTwin(static_cast<Lumina::CObjectBase*>(static_cast<Lumina::CObject*>(Object)), WeakHandle) ? 1 : 0;
}

// Called before the collectible load context unloads, since Scriptable instances are held strongly.
LUMINA_DOTNET_EXPORT(void, ReleaseAllManagedInstances)()
{
    Lumina::ManagedInstances::ReleaseAll();
}

namespace
{
    using FManagedPin = Lumina::TStrongObjectPtr<Lumina::CObject>;

    // Without this a reinstance leaves the replacement unowned while C# keeps the original alive.
    class FManagedPinProvider final : public Lumina::IObjectReferenceProvider
    {
    public:

        static FManagedPinProvider& Get()
        {
            static FManagedPinProvider Instance;
            return Instance;
        }

        const char* GetReferenceProviderName() const override { return "ManagedPins"; }

        // Visited unlocked, since only the game thread removes a pin and a reinstance runs there too.
        void VisitObjectReferences(Lumina::FObjectReferenceVisitor::FSlotFunc Func) override
        {
            Lumina::TVector<FManagedPin*> Snapshot;
            {
                Lumina::FScopeLock Lock(Mutex);
                Snapshot.Reset();
                Snapshot.Append(Pins.begin(), Pins.end());
            }
            for (FManagedPin* Pin : Snapshot)
            {
                Lumina::CObject* const Current = Pin->Get();
                if (Lumina::CObject* const Replacement = Func(Current); Replacement != Current)
                {
                    *Pin = Replacement;
                }
            }
        }

        FManagedPin* Pin(Lumina::CObject* Object)
        {
            FManagedPin* Pin = Lumina::Memory::New<FManagedPin>(Object);
            Lumina::FScopeLock Lock(Mutex);
            Pins.Add(Pin);
            return Pin;
        }

        void Unpin(FManagedPin* Pin)
        {
            {
                Lumina::FScopeLock Lock(Mutex);
                Pins.Remove(Pin);
            }
            Lumina::Memory::Delete(Pin);
        }

    private:

        FManagedPinProvider()
        {
            Lumina::FObjectReferenceProviders::Register(this);
        }

        Lumina::FMutex                Mutex;
        Lumina::THashSet<FManagedPin*> Pins;
    };
}

// The strong reference a canonical C# wrapper owns, released once the wrapper is collected.
LUMINA_DOTNET_EXPORT(void*, PinObject)(void* Object)
{
    if (Object == nullptr)
    {
        return nullptr;
    }
    return FManagedPinProvider::Get().Pin(static_cast<Lumina::CObject*>(Object));
}

LUMINA_DOTNET_EXPORT(void, UnpinObject)(void* Pin)
{
    if (Pin != nullptr)
    {
        FManagedPinProvider::Get().Unpin(static_cast<FManagedPin*>(Pin));
    }
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(ObjectGetHandle),
    LUMINA_DOTNET_SIG(ObjectResolve),
    LUMINA_DOTNET_SIG(ObjectGetEntry),
    LUMINA_DOTNET_SIG(ObjectLayoutOffset),
    LUMINA_DOTNET_SIG(ObjectGetManagedInstance),
    LUMINA_DOTNET_SIG(ObjectSetManagedInstance),
    LUMINA_DOTNET_SIG(ReleaseAllManagedInstances),
    LUMINA_DOTNET_SIG(PinObject),
    LUMINA_DOTNET_SIG(ObjectAdoptScriptTwin),
    LUMINA_DOTNET_SIG(UnpinObject)
);
