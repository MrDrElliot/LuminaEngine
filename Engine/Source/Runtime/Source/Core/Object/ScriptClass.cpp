#include "RuntimePCH.h"
#include "ScriptClass.h"

#include "Core/Reflection/Type/LuminaTypes.h"

IMPLEMENT_INTRINSIC_CLASS(CScriptClass, CClass, RUNTIME_API)

namespace Lumina
{
    CClass* CScriptClass::GetMetaClass() const
    {
        return StaticClass();
    }

    // A C# class minted under a C# parent stacks its block after the parent's, so the whole chain is the object's storage.
    template<typename TFunc>
    static void ForEachScriptClassInChain(const CScriptClass* Class, TFunc&& Func)
    {
        for (const CScriptClass* Current = Class; Current != nullptr; Current = ToScriptClass(Current->GetSuperClass()))
        {
            Func(*Current);
        }
    }

    bool CScriptClass::ConstructScriptProperties(void* Object) const
    {
        if (Object == nullptr)
        {
            return false;
        }

        bool bAny = false;
        uint8* Base = static_cast<uint8*>(Object);
        const CObject* Defaults = GetDefaultObjectIfCreated();
        ForEachScriptClassInChain(this, [&](const CScriptClass& Class)
        {
            bAny |= !Class.ScriptProperties.empty();
            for (FProperty* Property : Class.ScriptLifecycleProperties)
            {
                Property->ConstructValue(Base + Property->Offset);
            }
            if (Defaults != nullptr && Defaults != Object)
            {
                const uint8* DefaultBase = reinterpret_cast<const uint8*>(Defaults);
                for (FProperty* Property : Class.ScriptProperties)
                {
                    Property->CopyCompleteValue(Base + Property->Offset, DefaultBase + Property->Offset);
                }
            }
        });
        return bAny;
    }

    void CScriptClass::DestructScriptProperties(void* Object) const
    {
        if (Object == nullptr)
        {
            return;
        }
        ForEachScriptClassInChain(this, [&](const CScriptClass& Class)
        {
            for (FProperty* Property : Class.ScriptLifecycleProperties)
            {
                Property->DestructValue(static_cast<uint8*>(Object) + Property->Offset);
            }
        });
    }
}
namespace Lumina
{
    const FFunction* CScriptClass::FindScriptOverride(const FName& Name) const
    {
        const auto It = ScriptOverrideFunctions.find(Name);
        return It != ScriptOverrideFunctions.end() ? It->second : nullptr;
    }
}
