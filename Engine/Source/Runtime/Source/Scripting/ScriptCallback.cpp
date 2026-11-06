#include "RuntimePCH.h"

#include "ScriptCallback.h"

#include "Scripting/DotNet/DotNetHost.h"

namespace Lumina::Scripting
{
    namespace
    {
        // Lives in LuminaSharp.dll rather than a script assembly, so the resolved pointer survives a reload.
        uint64 CallInvoke(uint64 Token, uint64 Payload)
        {
            static DotNet::TManagedExport<uint64 (*)(void*, uint64)> Invoke("InvokeScriptCallback");
            auto* Fn = DotNet::IsInitialized() ? Invoke.Get() : nullptr;
            return Fn != nullptr ? Fn(reinterpret_cast<void*>(Token), Payload) : 0;
        }
    }

    uint64 InvokeScriptCallback(SScriptCallback Callback, uint64 Payload)
    {
        if (!Callback.IsBound())
        {
            return 0;
        }
        const uint64 Result = CallInvoke(Callback.Token, Payload);
        DotNet::FreeManagedHandle(reinterpret_cast<void*>(Callback.Token));
        return Result;
    }

    uint64 InvokeScriptCallbackRepeating(SScriptCallback Callback, uint64 Payload)
    {
        return Callback.IsBound() ? CallInvoke(Callback.Token, Payload) : 0;
    }

    void ReleaseScriptCallback(SScriptCallback Callback)
    {
        DotNet::FreeManagedHandle(reinterpret_cast<void*>(Callback.Token));
    }
}
