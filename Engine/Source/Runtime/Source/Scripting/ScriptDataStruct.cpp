#include "RuntimePCH.h"
#include "ScriptDataStruct.h"

#include "ScriptStruct.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Object/ObjectUtils.h"
#include "Core/Object/Package/Package.h"
#include "Scripting/DotNet/DotNetHost.h"

namespace Lumina
{
    namespace
    {
        // A base carrying data would be overlapped, so a non-empty one is named rather than silently corrupting.
        CStruct* ResolveNativeBase(const FName& BaseName, const FName& ScriptTypeName)
        {
            if (BaseName.IsNone())
            {
                return nullptr;
            }

            CStruct* Base = FindObject<CStruct>(BaseName);
            if (Base == nullptr)
            {
                LOG_WARN("Script data type '{}': native base '{}' does not exist; not minted. "
                    "Its module may not be loaded, or the marker names a type that was removed.",
                    ScriptTypeName, BaseName);
                return nullptr;
            }

            // An empty C++ struct still reports size 1, and that padding byte collides with nothing.
            for (CStruct* Link = Base; Link != nullptr; Link = Link->GetSuperStruct())
            {
                bool bDeclaresFields = false;
                Link->ForEachProperty<FProperty>([&bDeclaresFields](FProperty*) { bDeclaresFields = true; });

                if (bDeclaresFields)
                {
                    LOG_WARN("Script data type '{}': native base '{}' declares fields (on '{}'). "
                        "A script type's fields start at offset zero, so only a marker struct can be a base.",
                        ScriptTypeName, BaseName, Link->GetName());
                    return nullptr;
                }
            }

            return Base;
        }
    }

    FScriptDataStructRegistry& FScriptDataStructRegistry::Get()
    {
        static FScriptDataStructRegistry Instance;
        return Instance;
    }

    void FScriptDataStructRegistry::Clear()
    {
        // The registry holds the only strong refs, so releasing them is the teardown.
        Types.clear();
        ++Generation;
    }

    void FScriptDataStructRegistry::Refresh()
    {
        TVector<DotNet::FScriptStructTypeDesc> Descs;
        DotNet::GatherScriptStructTypes(Descs);

        // A layout cannot be edited in place, so callers re-resolve through the generation counter.
        Clear();

        for (const DotNet::FScriptStructTypeDesc& Desc : Descs)
        {
            const FName ScriptTypeName(Desc.ScriptTypeName.c_str());
            const FName BaseName(Desc.NativeBaseName.c_str());

            if (ScriptTypeName.IsNone())
            {
                continue;
            }

            // Both live in the flat name space an asset stores, so shadowing would make a name load-order dependent.
            if (FindObject<CStruct>(ScriptTypeName) != nullptr)
            {
                LOG_WARN("Script data type '{}' collides with a native struct of the same name; not minted.",
                    ScriptTypeName);
                continue;
            }

            CStruct* Base = ResolveNativeBase(BaseName, ScriptTypeName);
            if (Base == nullptr)
            {
                continue;
            }

            Scripting::FScriptExportSchema Schema;
            TVector<Scripting::FScriptPropertyEntry> Defaults;
            if (!DotNet::GatherScriptStructSchema(Desc.ScriptTypeName, Schema, Defaults))
            {
                LOG_WARN("Script data type '{}': no member schema came back; not minted.", ScriptTypeName);
                continue;
            }

            Schema.NativeBaseName = BaseName;

            FConstructCObjectParams Params(CScriptStruct::StaticClass());
            Params.Name    = ScriptTypeName;
            Params.Flags   = OF_Transient;
            Params.Package = CPackage::GetTransientPackage();
            Params.Guid    = FGuid::New();

            TObjectPtr<CScriptStruct> Minted = static_cast<CScriptStruct*>(StaticAllocateObject(Params));
            if (Minted == nullptr)
            {
                continue;
            }

            CObjectForceRegistration(Minted.Get());

            if (!Minted->BuildFromSchema(Schema, &Defaults))
            {
                LOG_WARN("Script data type '{}': layout could not be built; not minted.", ScriptTypeName);
                continue;   // Minted goes out of scope here; the last strong ref with it.
            }

            // Safe because the base is empty, which ResolveNativeBase is what guarantees.
            Minted->SetSuperStruct(Base);

            // An asset stores this rather than the object's name, so a re-mint still resolves.
            Minted->Metadata.AddValue("ScriptTypeName", Desc.ScriptTypeName.c_str());
            Minted->SetFlag(OF_Transient);

            Types[ScriptTypeName] = std::move(Minted);
        }

        if (!Types.empty())
        {
            LOG_INFO("Minted {} script data type(s).", Types.size());
        }
    }

    CScriptStruct* FScriptDataStructRegistry::Find(const FName& ScriptTypeName) const
    {
        auto It = Types.find(ScriptTypeName);
        return It != Types.end() ? It->second.Get() : nullptr;
    }

    CStruct* ResolveDataStructByName(const FName& Name)
    {
        if (Name.IsNone())
        {
            return nullptr;
        }

        if (CStruct* Native = FindObject<CStruct>(Name))
        {
            return Native;
        }

        return FScriptDataStructRegistry::Get().Find(Name);
    }

    FName DataStructIdentity(const CStruct* Struct)
    {
        if (Struct == nullptr)
        {
            return FName();
        }

        if (const FString* Stable = Struct->Metadata.TryGetMetadata("ScriptTypeName"))
        {
            if (!Stable->empty())
            {
                return FName(Stable->c_str());
            }
        }

        return Struct->GetName();
    }

}
