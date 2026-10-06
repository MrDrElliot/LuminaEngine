#include "DotNetUI.h"

#include "DotNetHost.h"
#include "Core/Object/Object.h"
#include "Core/Object/ScriptClass.h"
#include "Scripting/ScriptableObject.h"
#include "World/World.h"

namespace Lumina::DotNetUI
{
    namespace
    {
        DotNet::TManagedExport<void (*)(uint64)> GPollModels("PollUIDataModels");
        DotNet::TManagedExport<void (*)(void*)>  GScriptDocumentLoaded("UIScriptDocumentLoaded");
    }

    void PollModels(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }
        if (auto* Fn = GPollModels.Get())
        {
            Fn(reinterpret_cast<uint64>(World));
        }
    }

    void ScriptDocumentLoaded(CObject* Script)
    {
        if (Script == nullptr || ToScriptClass(Script->GetClass()) == nullptr)
        {
            return;
        }
        auto* Fn = GScriptDocumentLoaded.Get();
        void* Handle = Fn != nullptr ? Scriptable::GetOrCreateInstance(Script) : nullptr;
        if (Handle != nullptr)
        {
            Fn(Handle);
        }
    }
}
