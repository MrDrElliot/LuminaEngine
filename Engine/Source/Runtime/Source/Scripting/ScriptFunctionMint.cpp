#include "RuntimePCH.h"
#include "ScriptFunctionMint.h"

#include "Core/Object/ScriptClass.h"
#include "Log/Log.h"
#include "ScriptStruct.h"

namespace Lumina::Scripting
{
    FFunction* MintScriptFunction(CScriptClass& Class, CScriptStruct& LayoutRecord, const FName& Name,
                                  const FScriptExportSchema& ParamSchema, int32 ReturnIndex,
                                  FFunction::FNativeFuncPtr Thunk)
    {
        TVector<FProperty*> Params;
        Params.reserve(ParamSchema.Fields.size());

        const CScriptStruct::FEmittedLayout Layout = LayoutRecord.EmitLayoutInto(&Class, 0, ParamSchema, &Params);

        // A field the emitter could not describe is dropped, which would shift every parameter after it.
        if (Params.size() != ParamSchema.Fields.size())
        {
            LOG_ERROR("Script function '{}' on '{}': {} of {} parameters could be described, so it is not minted",
                Name, Class.GetName(), Params.size(), ParamSchema.Fields.size());
            return nullptr;
        }

        if (Layout.EndOffset > UINT16_MAX)
        {
            LOG_ERROR("Script function '{}' on '{}': a {}-byte frame is past what a parameter offset can address",
                Name, Class.GetName(), Layout.EndOffset);
            return nullptr;
        }

        FFunction* Function = FFunctionBuilder::BuildMinted(Class.GetPropertyArena(), &Class, Name,
            EFunctionFlags::ScriptCallable, Params, ReturnIndex, (uint16)Layout.EndOffset, Thunk);

        if (Function == nullptr)
        {
            return nullptr;
        }

        Class.AddFunction(Function);

        return Function;
    }
}
