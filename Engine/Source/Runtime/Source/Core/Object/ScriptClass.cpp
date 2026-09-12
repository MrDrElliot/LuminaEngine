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

    bool CScriptClass::ConstructScriptProperties(void* Object) const
    {
        if (Object == nullptr || ScriptProperties.empty())
        {
            return false;
        }

        uint8* Base = static_cast<uint8*>(Object);
        for (FProperty* Property : ScriptLifecycleProperties)
        {
            Property->ConstructValue(Base + Property->Offset);
        }

        if (const CObject* Defaults = GetDefaultObjectIfCreated(); Defaults != nullptr && Defaults != Object)
        {
            const uint8* DefaultBase = reinterpret_cast<const uint8*>(Defaults);
            for (FProperty* Property : ScriptProperties)
            {
                Property->CopyCompleteValue(Base + Property->Offset, DefaultBase + Property->Offset);
            }
        }
        return true;
    }

    void CScriptClass::DiscardRetiredLayout()
    {
        RetiredProperties.clear();
        // The record's arena owns the properties, so releasing it IS the free.
        RetiredRecord = nullptr;
        RetiredIn = 0;
    }

    void CScriptClass::RetireLayout(uint64 Generation)
    {
        // The previous retired generation has survived a whole reload, so nothing can still reach it.
        DiscardRetiredLayout();

        RetiredRecord = LayoutRecord;
        RetiredProperties = ScriptProperties;
        RetiredIn = Generation;

        LayoutRecord = nullptr;
        ScriptProperties.clear();
        ScriptLifecycleProperties.clear();
        bHasAppendedBlock = false;
    }

    void CScriptClass::DestructScriptProperties(void* Object) const
    {
        if (Object == nullptr)
        {
            return;
        }
        for (FProperty* Property : ScriptLifecycleProperties)
        {
            Property->DestructValue(static_cast<uint8*>(Object) + Property->Offset);
        }
    }
}
