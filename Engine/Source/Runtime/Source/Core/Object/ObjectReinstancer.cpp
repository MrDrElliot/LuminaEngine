#include "RuntimePCH.h"

#include "ObjectReinstancer.h"

#include "Class.h"
#include "Object.h"
#include "ObjectArray.h"
#include "ObjectCore.h"
#include "ObjectHandleTyped.h"
#include "ObjectReferenceProvider.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Core/Object/Package/Package.h"

namespace Lumina
{
    namespace
    {
        FName MakeRetiredName(const FName& Original)
        {
            static uint32 Counter = 0;
            const FString Base = FString("RETIRED_") + *Original;
            return FName(*Base, ++Counter);
        }
    }

    void FObjectReinstancer::MapClass(CClass* Old, CClass* New)
    {
        if (Old == nullptr || New == nullptr || Old == New)
        {
            return;
        }

        ClassMap.Add(Old, New);
    }

    CObject* FObjectReinstancer::BuildReplacement(CObject* Old, CClass* NewClass)
    {
        const FName        Name    = Old->GetName();
        CPackage* const    Package = Old->GetPackage();
        const FGuid        Guid    = Old->GetGUID();
        const EObjectFlags Flags   = Old->GetFlags();

        // The replacement takes the name, GUID and package, and the original steps aside out of the package so no save writes it.
        Old->Rename(MakeRetiredName(Name), CPackage::GetTransientPackage());
        Old->HandleGUIDChange(FGuid::New());

        CObject* New = NewObject(NewClass, Package, Name, Guid, Flags);
        if (New == nullptr)
        {
            Old->HandleGUIDChange(Guid);
            Old->Rename(Name, Package);
            return nullptr;
        }

        // OF_Rooted only mirrors membership of the root set, so the membership itself has to move across.
        if (Old->HasAnyFlag(OF_Rooted))
        {
            New->ClearFlags(OF_Rooted);
            New->AddToRoot();
            Old->RemoveFromRoot();
        }

        // Tagged, so a property the new shape added, dropped or moved is matched by name and the rest of the
        // values survive a layout change that a raw copy could not.
        TVector<uint8> Bytes;
        {
            FMemoryWriter Writer(Bytes);
            FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
            Old->GetClass()->SerializeProperties(Ar, Old);
        }
        {
            FMemoryReader Reader(Bytes);
            FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
            NewClass->SerializeProperties(Ar, New);
        }

        if (PostReplace)
        {
            PostReplace(Old, New);
        }

        return New;
    }

    FReinstanceResult FObjectReinstancer::Commit()
    {
        FReinstanceResult Result;
        if (ClassMap.IsEmpty())
        {
            return Result;
        }

        TVector<CObject*> Originals;
        ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base == nullptr || Base->HasAnyFlag(OF_MarkedDestroy) || Base->HasAnyFlag(OF_DefaultObject))
            {
                return;
            }
            if (ClassMap.Contains(Base->GetClass()))
            {
                Originals.Add(static_cast<CObject*>(Base));
            }
        });

        // Repointing an original's last holder drops it to zero, so each one is held until the walk is done with it.
        TVector<TStrongObjectPtr<CObject>> Pins(Originals.begin(), Originals.end());

        for (CObject* Old : Originals)
        {
            CClass* const NewClass = ClassMap.FindChecked(Old->GetClass());
            const FObjectHandle OldHandle = Object::Internal::GetHandleByObject(Old);
            if (CObject* New = BuildReplacement(Old, NewClass))
            {
                ObjectMap.Add(Old, New);

                // A weak reference is not a property, so no walk can reach it. Redirecting the handle is what
                // makes one follow the instance instead of reading as destroyed.
                Object::Internal::AddReinstanceRedirect(OldHandle, Object::Internal::GetHandleByObject(New));
                ++Result.InstancesReplaced;
            }
        }

        // The classes go in the same table, so a TSubclassOf or a TSubStructOf naming the retired class is
        // repointed by the very same walk that repoints the instances.
        for (const auto& [Old, New] : ClassMap)
        {
            ObjectMap.Add(static_cast<CObject*>(Old), static_cast<CObject*>(New));
        }

        if (ObjectMap.IsEmpty())
        {
            return Result;
        }

        auto Repoint = [&](CObject* Current) -> CObject*
        {
            if (Current == nullptr)
            {
                return nullptr;
            }
            const auto Found = ObjectMap.Find(Current);
            if (Found == nullptr)
            {
                return Current;
            }
            ++Result.ReferencesPatched;
            return (*Found);
        };

        // Raw is safe here because every original is pinned above, so none dies while the walk is still going.
        TVector<CObject*> Live;
        ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base != nullptr && !Base->HasAnyFlag(OF_MarkedDestroy))
            {
                Live.Add(static_cast<CObject*>(Base));
            }
        });

        for (CObject* Object : Live)
        {
            if (ObjectMap.Contains(Object))
            {
                continue;   // an original on its way out, so what it points at no longer matters
            }
            FObjectReferenceVisitor::VisitStruct(Object->GetClass(), Object, Repoint);
            Object->VisitAdditionalObjectReferences(Repoint);
            ++Result.ObjectsScanned;
        }

        Result.ProvidersVisited = FObjectReferenceProviders::ForEach([&Repoint](IObjectReferenceProvider& Provider)
        {
            Provider.VisitObjectReferences(Repoint);
        });

        // Captured as handles, since destroying one original can free another one in the same map.
        TVector<TPair<FObjectHandle, CObject*>> Doomed;
        Doomed.Reserve(ObjectMap.Num());
        for (const auto& [Old, New] : ObjectMap)
        {
            if (Old->IsA<CClass>())
            {
                continue;   // a retired class is taken out by whoever minted it, not here
            }
            Doomed.Emplace(Object::Internal::GetHandleByObject(Old), Old);
        }

        // An original nothing reaches any more dies here, which the handle check below then skips.
        Pins.Reset();

        FString Stranded;
        for (const TPair<FObjectHandle, CObject*>& Entry : Doomed)
        {
            if (Object::Internal::ResolveHandle(Entry.Key) != Entry.Value)
            {
                continue;   // an earlier destroy in this loop already took it
            }

            // Forcing it down while a strong reference survives would dangle that holder. The original is
            // already renamed aside and resolves to nothing, so leaving it to its refcount is safe, and a
            // reference the walk could not reach keeps working until its owner lets go.
            if (Object::Internal::GetStrongRefCountByIndex(Entry.Key.Index) == 0)
            {
                Entry.Value->ForceDestroyNow();
            }
            else
            {
                ++Result.OriginalsOutlivingTheSwap;
                if (Result.OriginalsOutlivingTheSwap <= 4)
                {
                    Stranded += Stranded.IsEmpty() ? "" : ", ";
                    Stranded += *Entry.Value->GetName();
                }
            }
        }

        if (Result.OriginalsOutlivingTheSwap > 0)
        {
            LOG_WARN("Reinstancer: {} original(s) are still strongly referenced and stay alive ({}). Whatever "
                     "holds them is not reachable through reflection and has no reference provider, so it is "
                     "still pointing at the old object.",
                     Result.OriginalsOutlivingTheSwap, Stranded);
        }

        Result.Replaced = Move(ObjectMap);
        return Result;
    }
}
