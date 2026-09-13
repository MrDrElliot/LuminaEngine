#pragma once

#include "Containers/Name.h"
#include "Core/Reflection/Type/Function.h"
#include "ScriptExports.h"

namespace Lumina
{
    class CScriptClass;
    class CScriptStruct;
}

namespace Lumina::Scripting
{
    /**
     * Mints a reflected function a script type declared, and attaches it to Class.
     *
     * The frame is laid out by the same emitter that lays out a script struct, so alignment, container inners
     * and the 64KB offset limit are handled in one place rather than twice. Its parameters are collected rather
     * than attached, since a parameter is not a member of the class that declares the function.
     *
     * LayoutRecord owns the element descriptions the parameters point at, so it has to outlive every call.
     * ReturnIndex is an index into the schema's fields, or -1 for a function that returns nothing.
     *
     * Adds the function without linking, matching how script properties are appended: the mint path unlinks the
     * class, rebuilds its members and functions, and links once at the end.
     */
    RUNTIME_API FFunction* MintScriptFunction(CScriptClass& Class, CScriptStruct& LayoutRecord, const FName& Name,
                                             const FScriptExportSchema& ParamSchema, int32 ReturnIndex,
                                             FFunction::FNativeFuncPtr Thunk);
}
