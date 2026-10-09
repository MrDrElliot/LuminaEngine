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

namespace Lumina
{
    namespace
    {
        FName MakeRetiredName(const FName& Original)
        {
            static uint32 Counter = 0;
            const FString Base = FString("RETIRED_") + Original.c_str();
            return FName(Base.c_str(), ++Counter);
        }
    }

    void FObjectReinstancer::MapClass(CClass* Old, CClass* New)
    {
        if (Old == nullptr || New == nullptr || Old == New)
        {
            return;
        }

        ClassMap.insert_or_assign(Old, New);
    }

    CObject* FObjectReinstancer::BuildReplacement(CObject* Old, CClass* NewClass)
    {
        const FName        Name    = Old->GetName();
        CPackage* const    Package = Old->GetPackage();
        const FGuid        Guid    = Old->GetGUID();
        const EObjectFlags Flags   = Old->GetFlags();

        // Name and GUID are both the instance's identity to whatever resolves one, so the replacement takes
        // both and the original steps aside on each, rather than the other way round.
        Old->Rename(MakeRetiredName(Name), Package);
        Old->HandleGUIDChange(FGuid::New());

        CObject* New = NewObject(NewClass, Package, Name, Guid, Flags);
        if (New == nullptr)
        {
            Old->HandleGUIDChange(Guid);
            Old->Rename(Name, Package);
            return nullptr;
        }

        // Tagged, so a property the new shape added, dropped or moved is matched by name and the rest of the
        // values survive a layout change that a raw copy could not.
        TVector<uint8> Bytes;
        {
            FMemoryWriter Writer(Bytes);
            FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
            Old->GetClass()->SerializeTaggedProperties(Ar, Old);
        }
        {
            FMemoryReader Reader(Bytes);
            FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
            NewClass->SerializeTaggedProperties(Ar, New);
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
        if (ClassMap.empty())
        {
            return Result;
        }

        TVector<CObject*> Originals;
        GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base == nullptr || Base->HasAnyFlag(OF_MarkedDestroy) || Base->HasAnyFlag(OF_DefaultObject))
            {
                return;
            }
            if (ClassMap.find(Base->GetClass()) != ClassMap.end())
            {
                Originals.push_back(static_cast<CObject*>(Base));
            }
        });

        // Repointing an original's last holder drops it to zero, so each one is held until the walk is done with it.
        TVector<TStrongObjectPtr<CObject>> Pins(Originals.begin(), Originals.end());

        for (CObject* Old : Originals)
        {
            CClass* const NewClass = ClassMap.find(Old->GetClass())->second;
            const FObjectHandle OldHandle = GObjectArray.GetHandleByObject(Old);
            if (CObject* New = BuildReplacement(Old, NewClass))
            {
                ObjectMap.insert_or_assign(Old, New);

                // A weak reference is not a property, so no walk can reach it. Redirecting the handle is what
                // makes one follow the instance instead of reading as destroyed.
                GObjectArray.AddReinstanceRedirect(OldHandle, GObjectArray.GetHandleByObject(New));
                ++Result.InstancesReplaced;
            }
        }

        // The classes go in the same table, so a TSubclassOf or a TSubStructOf naming the retired class is
        // repointed by the very same walk that repoints the instances.
        for (const auto& [Old, New] : ClassMap)
        {
            ObjectMap.insert_or_assign(static_cast<CObject*>(Old), static_cast<CObject*>(New));
        }

        if (ObjectMap.empty())
        {
            return Result;
        }

        auto Repoint = [&](CObject* Current) -> CObject*
        {
            if (Current == nullptr)
            {
                return nullptr;
            }
            const auto Found = ObjectMap.find(Current);
            if (Found == ObjectMap.end())
            {
                return Current;
            }
            ++Result.ReferencesPatched;
            return Found->second;
        };

        // Raw is safe here because every original is pinned above, so none dies while the walk is still going.
        TVector<CObject*> Live;
        GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base != nullptr && !Base->HasAnyFlag(OF_MarkedDestroy))
            {
                Live.push_back(static_cast<CObject*>(Base));
            }
        });

        for (CObject* Object : Live)
        {
            if (ObjectMap.find(Object) != ObjectMap.end())
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
        Doomed.reserve(ObjectMap.size());
        for (const auto& [Old, New] : ObjectMap)
        {
            if (Old->IsA<CClass>())
            {
                continue;   // a retired class is taken out by whoever minted it, not here
            }
            Doomed.emplace_back(GObjectArray.GetHandleByObject(Old), Old);
        }

        // An original nothing reaches any more dies here, which the handle check below then skips.
        Pins.clear();

        FString Stranded;
        for (const TPair<FObjectHandle, CObject*>& Entry : Doomed)
        {
            if (GObjectArray.ResolveHandle(Entry.first) != Entry.second)
            {
                continue;   // an earlier destroy in this loop already took it
            }

            // Forcing it down while a strong reference survives would dangle that holder. The original is
            // already renamed aside and resolves to nothing, so leaving it to its refcount is safe, and a
            // reference the walk could not reach keeps working until its owner lets go.
            if (GObjectArray.GetStrongRefCountByIndex(Entry.first.Index) == 0)
            {
                Entry.second->ForceDestroyNow();
            }
            else
            {
                ++Result.OriginalsOutlivingTheSwap;
                if (Result.OriginalsOutlivingTheSwap <= 4)
                {
                    Stranded += Stranded.empty() ? "" : ", ";
                    Stranded += Entry.second->GetName().c_str();
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

        return Result;
    }
}
