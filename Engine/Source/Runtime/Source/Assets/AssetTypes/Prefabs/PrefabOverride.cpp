#include "RuntimePCH.h"
#include "PrefabOverride.h"

#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"

namespace Lumina::PrefabOverride
{
    namespace
    {
        // Iterated without walking GetSuperStruct, since CStruct::Link already spliced the super onto the tail.

        // An opaque struct has an empty property chain, so its bytes still copy through the leaf path.
        bool StructHasReflectedProperties(CStruct* Struct)
        {
            return Struct != nullptr && Struct->LinkedProperty != nullptr;
        }

        // Such a component exposes no reflected leaf, so it is treated as one atomic leaf instead.
        bool HasSerializableLeaf(CStruct* Struct)
        {
            for (FProperty* Property = Struct->LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
            {
                if (Property->ShouldSerialize())
                {
                    return true;
                }
            }
            return false;
        }

        bool SerializeWholeValue(CStruct* Struct, const void* Value, TVector<uint8>& Out)
        {
            FStructOps* Ops = Struct->GetStructOps();
            if (Ops == nullptr || !Ops->HasSerializer())
            {
                return false;
            }
            FMemoryWriter Writer(Out);
            FObjectProxyArchiver Proxy(Writer, false);
            Ops->Serialize(Proxy, const_cast<void*>(Value));
            return true;
        }

        bool WholeValueDiffers(CStruct* Struct, const void* Instance, const void* Prefab)
        {
            FStructOps* Ops = Struct->GetStructOps();
            if (Ops != nullptr && Ops->HasEquality())
            {
                return !Ops->Equals(Instance, Prefab);
            }

            TVector<uint8> InstanceBytes;
            TVector<uint8> PrefabBytes;
            if (!SerializeWholeValue(Struct, Instance, InstanceBytes) || !SerializeWholeValue(Struct, Prefab, PrefabBytes))
            {
                return false;   // nothing comparable, so treat it as inherited rather than invent an override
            }
            return InstanceBytes != PrefabBytes;
        }

        void CopyWholeValue(CStruct* Struct, void* Dest, const void* Source)
        {
            if (FStructOps* Ops = Struct->GetStructOps(); Ops != nullptr && Ops->HasCopy())
            {
                Ops->Copy(Dest, Source);
            }
        }

        FString JoinPath(const FString& Prefix, const FName& Name)
        {
            if (Prefix.empty())
            {
                return FString(Name.c_str());
            }
            return Prefix + "." + Name.c_str();
        }

        // Recurses into reflected nested structs and treats containers and opaque structs as leaves.
        template<typename Visitor>
        void ForEachLeafPair(CStruct* Struct, void* Inst, const void* Pref, const FString& Prefix, Visitor& Visit)
        {
            for (FProperty* Property = Struct->LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
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

    const FName& WholeValuePath()
    {
        // Not a legal identifier, so it can never collide with a real property path.
        static const FName Path("$Whole");
        return Path;
    }

    void CollectOverriddenLeaves(CStruct* Struct, const void* Instance, const void* Prefab, TVector<FName>& OutPaths)
    {
        if (Struct == nullptr || Instance == nullptr || Prefab == nullptr)
        {
            return;
        }

        if (!HasSerializableLeaf(Struct))
        {
            if (WholeValueDiffers(Struct, Instance, Prefab))
            {
                OutPaths.push_back(WholeValuePath());
            }
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

        if (!HasSerializableLeaf(Struct))
        {
            if (OverriddenPaths.find(WholeValuePath()) == OverriddenPaths.end())
            {
                CopyWholeValue(Struct, Instance, Prefab);
            }
            return;
        }

        auto Visit = [&](FProperty* Property, void* Inst, const void* Pref, const FString& Path)
        {
            // An overridden leaf keeps the instance value rather than pulling the prefab's.
            if (OverriddenPaths.find(FName(Path.c_str())) != OverriddenPaths.end())
            {
                return;
            }
            Property->CopyCompleteValue_InContainer(Inst, Pref);
        };

        ForEachLeafPair(Struct, Instance, Prefab, FString(), Visit);
    }

    void ApplyOverriddenLeaves(CStruct* Struct, void* Dest, const void* Authored, const THashSet<FName>& OverriddenPaths)
    {
        if (Struct == nullptr || Dest == nullptr || Authored == nullptr)
        {
            return;
        }

        if (!HasSerializableLeaf(Struct))
        {
            if (OverriddenPaths.find(WholeValuePath()) != OverriddenPaths.end())
            {
                CopyWholeValue(Struct, Dest, Authored);
            }
            return;
        }

        auto Visit = [&](FProperty* Property, void* Dst, const void* Src, const FString& Path)
        {
            if (OverriddenPaths.find(FName(Path.c_str())) == OverriddenPaths.end())
            {
                return;
            }
            Property->CopyCompleteValue_InContainer(Dst, Src);
        };

        ForEachLeafPair(Struct, Dest, Authored, FString(), Visit);
    }
}
