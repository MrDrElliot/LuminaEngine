#include "RuntimePCH.h"
#include "Function.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/PropertyArena.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Log/Log.h"

namespace Lumina
{
    static_assert(sizeof(FFunction) == 48, "FFunction changed size; re-check its member order.");
    static_assert(std::is_trivially_destructible_v<FFunction>,
        "FFunction lives in the property arena, which destroys properties and leaves everything else alone.");

    void FFunction::InitializeFrame(void* Frame) const
    {
        DEBUG_ASSERT(Frame != nullptr);

        // Zeroed first, so a trivially-constructible parameter is already its default and only the kinds
        // that own memory need the virtual call below.
        Memory::Memzero(Frame, ParmsSize);

        for (uint16 Index = 0; Index < NumParams; ++Index)
        {
            FProperty* Param = Params[Index];
            if (Param->OwnsStorage())
            {
                Param->ConstructValue(Param->GetValuePtr<void>(Frame));
            }
        }
    }

    void FFunction::DestructFrame(void* Frame) const
    {
        DEBUG_ASSERT(Frame != nullptr);

        // Reverse of construction order, matching how every other holder of properties tears down.
        for (uint16 Index = NumParams; Index > 0; --Index)
        {
            FProperty* Param = Params[Index - 1];
            if (Param->OwnsStorage())
            {
                Param->DestructValue(Param->GetValuePtr<void>(Frame));
            }
        }
    }

    void FFunction::Invoke(void* Context, void* Frame) const
    {
        if (Thunk == nullptr)
        {
            LOG_ERROR("Invoke: reflected function '{}' has no thunk, so there is nothing to call", Name);
            return;
        }

        // A static takes no object, and anything else cannot run without one.
        if (Context == nullptr && !IsStatic())
        {
            LOG_ERROR("Invoke: reflected function '{}' needs an object and was given none", Name);
            return;
        }

        Thunk(*this, Context, Frame);
    }

    FFunction* FFunctionBuilder::Build(FPropertyArena& Arena, CStruct* Owner, const FFunctionParams& Params,
                                      TSpan<FProperty* const> OrderedParams)
    {
        return BuildCore(Arena, Owner, FName(Params.Name), Params.Flags, OrderedParams,
                         Params.ReturnIndex, Params.ParmsSize, Params.Thunk);
    }

    FFunction* FFunctionBuilder::BuildMinted(FPropertyArena& Arena, CStruct* Owner, const FName& Name,
                                             EFunctionFlags Flags, TSpan<FProperty* const> OrderedParams,
                                             int32 ReturnIndex, uint16 ParmsSize,
                                             FFunction::FNativeFuncPtr Thunk)
    {
        return BuildCore(Arena, Owner, Name, Flags, OrderedParams, ReturnIndex, ParmsSize, Thunk);
    }

    FFunction* FFunctionBuilder::BuildCore(FPropertyArena& Arena, CStruct* Owner, const FName& Name,
                                           EFunctionFlags Flags, TSpan<FProperty* const> OrderedParams,
                                           int32 ReturnIndex, uint16 ParmsSize, FFunction::FNativeFuncPtr Thunk)
    {
        const uint16 NumParams = (uint16)OrderedParams.size();

        if (ReturnIndex >= (int32)NumParams)
        {
            LOG_ERROR("Reflected function '{}' names parameter {} as its return, and it has {}",
                Name, ReturnIndex, NumParams);
            return nullptr;
        }

        // Every parameter has to fit the frame, or a call writes past it.
        for (FProperty* Param : OrderedParams)
        {
            if (Param->Offset + Param->GetElementSize() > ParmsSize)
            {
                LOG_ERROR("Reflected function '{}': parameter '{}' at offset {} size {} does not fit a "
                          "{}-byte frame", Name, Param->GetPropertyName(), Param->Offset,
                          Param->GetElementSize(), ParmsSize);
                return nullptr;
            }
        }

        void* Storage = Arena.AllocateBytes(sizeof(FFunction), alignof(FFunction));
        FFunction* Function = new (Storage) FFunction();

        FProperty** ParamArray = nullptr;
        if (NumParams != 0)
        {
            ParamArray = (FProperty**)Arena.AllocateBytes(sizeof(FProperty*) * NumParams, alignof(FProperty*));
            for (uint16 Index = 0; Index < NumParams; ++Index)
            {
                ParamArray[Index] = OrderedParams[Index];

                // A parameter answers for the function's owner, so a walker can get back to the type.
                ParamArray[Index]->OwnerStruct = Owner;
            }
        }

        Function->Name        = Name;
        Function->OwnerStruct = Owner;
        Function->Params      = ParamArray;
        Function->ReturnParam = (ReturnIndex >= 0) ? ParamArray[ReturnIndex] : nullptr;
        Function->Thunk       = Thunk;
        Function->Flags       = Flags;
        Function->NumParams   = NumParams;
        Function->ParmsSize   = ParmsSize;

        // The flag and the pointer are two spellings of the same fact, so they cannot be allowed to disagree.
        if (Function->ReturnParam != nullptr)
        {
            Function->Flags |= EFunctionFlags::HasReturn;
        }
        else
        {
            Function->Flags &= ~EFunctionFlags::HasReturn;
        }

        return Function;
    }

    FFunctionFrame::FFunctionFrame(const FFunction& InFunction)
        : Function(InFunction)
    {
        const size_t Size = Function.GetParmsSize();

        Memory = (Size <= kInlineBytes)
            ? static_cast<void*>(Inline)
            : Memory::Malloc(Size, FFunction::kFrameAlignment);

        Function.InitializeFrame(Memory);
    }

    FFunctionFrame::~FFunctionFrame()
    {
        Function.DestructFrame(Memory);

        if (Memory != Inline)
        {
            Memory::Free(Memory);
        }
    }

    void FFunctionFrame::Invoke(void* Context) const
    {
        Function.Invoke(Context, Memory);
    }

    size_t FFunctionFrame::OffsetOf(size_t Index) const
    {
        const TSpan<FProperty* const> Params = Function.GetParams();
        DEBUG_ASSERT(Index < Params.size());
        return (size_t)Params[Index]->Offset;
    }

    size_t FFunctionFrame::ReturnOffset() const
    {
        const FProperty* Return = Function.GetReturnParam();
        DEBUG_ASSERT(Return != nullptr);
        return (size_t)Return->Offset;
    }
}
