#pragma once

#include "Containers/Name.h"
#include "Core/Reflection/Type/Function.h"
#include "Scripting/ScriptExports.h"
#include "ScriptExports.h"

namespace Lumina
{
    class CScriptClass;
    class CScriptStruct;
}

namespace Lumina::Scripting
{
    // Mints a function a script type declared onto Class, laid out by the struct emitter. LayoutRecord owns its element descriptions, and the caller links.
    RUNTIME_API FFunction* MintScriptFunction(CScriptClass& Class, CScriptStruct& LayoutRecord, const FName& Name,
                                             const FScriptExportSchema& ParamSchema, int32 ReturnIndex,
                                             FFunction::FNativeFuncPtr Thunk, EFunctionFlags ExtraFlags = EFunctionFlags::None,
                                             const FScriptExportMeta& Meta = {});
}

namespace Lumina::Scripting
{
    // The one thunk every script function is minted with, forwarding the managed instance, the function and the frame to managed.
    RUNTIME_API void ScriptFunctionThunk(const FFunction& Function, void* Context, void* Frame);

    // Mints an override of Base onto Class whose body is the script's, sharing Base's parameters since they already describe the frame.
    RUNTIME_API FFunction* MintScriptOverride(CScriptClass& Class, const FFunction& Base,
                                             FFunction::FNativeFuncPtr Thunk);

    // Points Function's dispatch straight at the generated managed entry point.
    RUNTIME_API void PublishManagedInvoker(const FFunction& Function, void* Invoker, const int32* Offsets);

    // Drops every published entry point, since the code behind them goes away with the generation.
    RUNTIME_API void ClearManagedInvokers();
}
