#include "RuntimePCH.h"
#include "ScriptFunctionMint.h"

#include "Core/Object/ScriptClass.h"
#include "Core/Reflection/ReflectionDesc.h"
#include "Log/Log.h"
#include "ScriptStruct.h"
#include "ScriptableObject.h"
#include "DotNet/DotNetHost.h"
#include "Core/Object/Object.h"

namespace Lumina::Scripting
{
    FFunction* MintScriptFunction(CScriptClass& Class, CScriptStruct& LayoutRecord, const FName& Name,
                                  const FScriptExportSchema& ParamSchema, int32 ReturnIndex,
                                  FFunction::FNativeFuncPtr Thunk, EFunctionFlags ExtraFlags,
                                  const FScriptExportMeta& Meta)
    {
        TVector<FProperty*> Params;
        Params.Reserve(ParamSchema.Fields.Num());

        const CScriptStruct::FEmittedLayout Layout = LayoutRecord.EmitLayoutInto(&Class, 0, ParamSchema, &Params);

        // A field the emitter could not describe is dropped, which would shift every parameter after it.
        if (Params.Num() != ParamSchema.Fields.Num())
        {
            LOG_ERROR("Script function '{}' on '{}': {} of {} parameters could be described, so it is not minted",
                Name, Class.GetName(), Params.Num(), ParamSchema.Fields.Num());
            return nullptr;
        }

        if (Layout.EndOffset > UINT16_MAX)
        {
            LOG_ERROR("Script function '{}' on '{}': a {}-byte frame is past what a parameter offset can address",
                Name, Class.GetName(), Layout.EndOffset);
            return nullptr;
        }

        // Viewing the schema's strings is enough, since the builder copies them into the class's arena.
        TVector<FString> Keys;
        TVector<FMetadataEntry> Metadata;
        Keys.Reserve(Meta.Entries.Num());
        Metadata.Reserve(Meta.Entries.Num());
        for (const FScriptExportMetaArg& Entry : Meta.Entries)
        {
            Keys.Add(Entry.Key.ToString());
            Metadata.Emplace(*Keys.Last(), *Entry.Value);
        }

        FFunction* Function = FFunctionBuilder::BuildMinted(Class.GetPropertyArena(), &Class, Name,
            EFunctionFlags::ScriptCallable | ExtraFlags, Params, ReturnIndex, (uint16)Layout.EndOffset, Thunk,
            TSpan<const FMetadataEntry>(Metadata.GetData(), Metadata.Num()));

        if (Function == nullptr)
        {
            return nullptr;
        }

        Class.AddFunction(Function);

        return Function;
    }
}

namespace Lumina::Scripting
{
    FFunction* MintScriptOverride(CScriptClass& Class, const FFunction& Base, FFunction::FNativeFuncPtr Thunk)
    {
        FFunction* Override = FFunctionBuilder::BuildMinted(Class.GetPropertyArena(), &Class,
            Base.GetFunctionName(),
            Base.GetFunctionFlags() | EFunctionFlags::ScriptImplemented | EFunctionFlags::ScriptCallable,
            Base.GetParams(), Base.HasReturn() ? Base.GetParams().Num() - 1 : -1,
            Base.GetParmsSize(), Thunk);

        if (Override == nullptr)
        {
            return nullptr;
        }

        Class.AddFunction(Override);
        Class.ScriptOverrideFunctions.Add(Base.GetFunctionName(), Override);
        return Override;
    }

    void ScriptFunctionThunk(const FFunction& Function, void* Context, void* Frame)
    {
        CObject* Object = static_cast<CObject*>(Context);
        if (Object == nullptr)
        {
            LOG_ERROR("Script function '{}' was invoked with no object to call it on", Function.GetFunctionName());
            return;
        }

        // Resolved once; the host returns null until the assembly carrying it is up, so this keeps asking.
        static void* Dispatcher = nullptr;
        if (Dispatcher == nullptr)
        {
            Dispatcher = DotNet::ResolveManagedExport("InvokeScriptFunction");
        }

        if (Dispatcher == nullptr)
        {
            LOG_ERROR("Script function '{}' cannot run: the managed dispatcher is not available",
                Function.GetFunctionName());
            return;
        }

        void* Instance = Scriptable::GetOrCreateInstance(Object);
        if (Instance == nullptr)
        {
            LOG_ERROR("Script function '{}': '{}' has no managed instance to call it on",
                Function.GetFunctionName(), Object->GetName());
            return;
        }

        // Straight into the generated entry point once one is published, skipping the dispatcher entirely.
        if (void* Invoker = Function.GetManagedInvoker())
        {
            using FDirect = void (*)(void*, void*, const int32*);
            reinterpret_cast<FDirect>(Invoker)(Instance, Frame, Function.GetManagedOffsets());
            return;
        }

        using FDispatch = void (*)(void*, const void*, void*);
        reinterpret_cast<FDispatch>(Dispatcher)(Instance, &Function, Frame);
    }

    namespace
    {
        // Every function given a managed invoker, so a reload can drop them before the code they point at unloads.
        TVector<const FFunction*> GPublishedInvokers;
    }

    void PublishManagedInvoker(const FFunction& Function, void* Invoker, const int32* Offsets)
    {
        if (Function.GetManagedInvoker() == nullptr)
        {
            GPublishedInvokers.Add(&Function);
        }
        Function.SetManagedInvoker(Invoker, Offsets);
    }

    void ClearManagedInvokers()
    {
        for (const FFunction* Function : GPublishedInvokers)
        {
            Function->SetManagedInvoker(nullptr, nullptr);
        }
        GPublishedInvokers.Reset();
    }
}
