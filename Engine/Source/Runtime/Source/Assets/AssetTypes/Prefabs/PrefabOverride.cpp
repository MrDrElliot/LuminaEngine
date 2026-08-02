#include "RuntimePCH.h"
#include "PrefabOverride.h"

#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"

namespace Lumina::PrefabOverride
{
    namespace
    {
        // A struct is recursed only if it exposes reflected child properties. Opaque structs
        // (math types, StructOps-only) have an empty property chain and are handled atomically,
        // so their bytes still copy/compare via the leaf path's FStructProperty.
        bool StructHasReflectedProperties(CStruct* Struct)
        {
            for (CStruct* Cur = Struct; Cur != nullptr; Cur = Cur->GetSuperStruct())
            {
                if (Cur->LinkedProperty != nullptr)
                {
                    return true;
                }
            }
            return false;
        }

        FString JoinPath(const FString& Prefix, const FName& Name)
        {
            if (Prefix.empty())
            {
                return FString(Name.c_str());
            }
            return Prefix + "." + Name.c_str();
        }

        // Visits every serializable leaf of Struct in lockstep across an instance + prefab pointer,
        // recursing into reflected nested structs and treating containers / opaque structs as leaves.
        // Visit signature: (FProperty*, void* InstanceContainer, const void* PrefabContainer, const FString& Path).
        template<typename Visitor>
        void ForEachLeafPair(CStruct* Struct, void* Inst, const void* Pref, const FString& Prefix, Visitor& Visit)
        {
            for (CStruct* Cur = Struct; Cur != nullptr; Cur = Cur->GetSuperStruct())
            {
                for (FProperty* Property = Cur->LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
                {
                    if (!Property->ShouldSerialize())
                    {
                        continue;
                    }

                    if (Property->GetType() == EPropertyTypeFlags::Struct)
                    {
                        FStructProperty* StructProp = static_cast<FStructProperty*>(Property);
                        CStruct* Inner = StructProp->GetStruct();
                        if (Inner != nullptr && StructHasReflectedProperties(Inner))
                        {
                            void* InstChild = StructProp->GetValuePtr<void>(Inst);
                            const void* PrefChild = StructProp->GetValuePtr<void>(Pref);
                            ForEachLeafPair(Inner, InstChild, PrefChild, JoinPath(Prefix, Property->Name), Visit);
                            continue;
                        }
                    }

                    Visit(Property, Inst, Pref, JoinPath(Prefix, Property->Name));
                }
            }
        }
    }

    void CollectOverriddenLeaves(CStruct* Struct, const void* Instance, const void* Prefab, TVector<FName>& OutPaths)
    {
        if (Struct == nullptr || Instance == nullptr || Prefab == nullptr)
        {
            return;
        }

        auto Visit = [&](FProperty* Property, void* Inst, const void* Pref, const FString& Path)
        {
            if (!Property->Identical_InContainer(Inst, Pref))
            {
                OutPaths.push_back(FName(Path.c_str()));
            }
        };

        ForEachLeafPair(Struct, const_cast<void*>(Instance), Prefab, FString(), Visit);
    }

    void ApplyInheritedLeaves(CStruct* Struct, void* Instance, const void* Prefab, const THashSet<FName>& OverriddenPaths)
    {
        if (Struct == nullptr || Instance == nullptr || Prefab == nullptr)
        {
            return;
        }

        auto Visit = [&](FProperty* Property, void* Inst, const void* Pref, const FString& Path)
        {
            // Overridden leaf: keep the instance value, do not pull the prefab's.
            if (OverriddenPaths.find(FName(Path.c_str())) != OverriddenPaths.end())
            {
                return;
            }
            Property->CopyCompleteValue_InContainer(Inst, Pref);
        };

        ForEachLeafPair(Struct, Instance, Prefab, FString(), Visit);
    }
}
