#include "RuntimePCH.h"
#include "ScriptDataStruct.h"

#include "ScriptStruct.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectUtils.h"
#include "Core/Object/Package/Package.h"
#include "Scripting/DotNet/DotNetHost.h"

namespace Lumina
{
    namespace
    {
        /**
         * Resolves the native struct a marker named, rejecting anything that cannot legally be a super.
         *
         * A minted layout is built from its own fields starting at offset zero, so a base carrying data
         * would be overlapped by them. The bases this mechanism is for are empty markers by design; a
         * non-empty one is a mistake worth naming rather than a layout that silently corrupts.
         */
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

            // Fields, not bytes: an empty C++ struct still reports sizeof 1 so that its instances have
            // distinct addresses, and that padding byte is nothing a derived layout can collide with.
            // What would actually collide is a declared property, anywhere up the base's own chain.
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
        // The registry holds the only strong refs, so releasing them is the teardown: the refcount drops
        // to zero and the object array destroys each one. Anything that cached a pointer across this is
        // caught by the generation bump below rather than by trying to keep zombies readable.
        Types.clear();
        ++Generation;
    }

    void FScriptDataStructRegistry::Refresh()
    {
        TVector<DotNet::FScriptStructTypeDesc> Descs;
        DotNet::GatherScriptStructTypes(Descs);

        // Everything from the previous generation goes, including types that still exist by name: their
        // fields may have changed, and a layout cannot be edited in place. Callers re-resolve through the
        // generation counter rather than being notified.
        Clear();

        for (const DotNet::FScriptStructTypeDesc& Desc : Descs)
        {
            const FName ScriptTypeName(Desc.ScriptTypeName.c_str());
            const FName BaseName(Desc.NativeBaseName.c_str());

            if (ScriptTypeName.IsNone())
            {
                continue;
            }

            // A script type never shadows a native one. Both live in the flat name space an asset stores,
            // so allowing it would make a stored name mean different things depending on load order.
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

            // The inheritance edge every consumer filters on. Safe after BuildFromSchema because the base
            // is empty, which ResolveNativeBase is what guarantees.
            Minted->SetSuperStruct(Base);

            // The identity that outlives this object. An asset stores this, never the object's name, so a
            // re-mint under a different address still resolves.
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
