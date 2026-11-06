#include "RuntimePCH.h"
#include "Function.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/PropertyArena.h"
#include "Core/Reflection/ReflectionDesc.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Log/Log.h"

namespace Lumina
{
    // 48 of description plus the two pointers script dispatch calls through, so a function stays one cache line.
    static_assert(sizeof(FFunction) == 64, "FFunction changed size; re-check its member order.");
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

    const FMetadataEntry* FFunction::FindMetadata(FStringView Key) const
    {
        for (uint16 Index = 0; Index < NumMetadata; ++Index)
        {
            if (EqualsIgnoreCase(Key, FStringView(Metadata[Index].Key)))
            {
                return &Metadata[Index];
            }
        }
        return nullptr;
    }

    FCStringView FFunction::GetMetadata(FStringView Key) const
    {
        const FMetadataEntry* Entry = FindMetadata(Key);
        return Entry != nullptr ? FCStringView(Entry->Value) : FCStringView();
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

    FFunction* FFunctionBuilder::Build(FPropertyArena& Arena, CStruct* Owner, const FFunctionDesc& Desc,
                                      TSpan<FProperty* const> OrderedParams)
    {
        return BuildCore(Arena, Owner, FName(Desc.Name), Desc.Flags, OrderedParams,
                         Desc.ReturnIndex, Desc.FrameSize, Desc.Thunk, TSpan<const FMetadataEntry>(Desc.Metadata, Desc.NumMetadata));
    }

    FFunction* FFunctionBuilder::BuildMinted(FPropertyArena& Arena, CStruct* Owner, const FName& Name,
                                             EFunctionFlags Flags, TSpan<FProperty* const> OrderedParams,
                                             int32 ReturnIndex, uint16 ParmsSize,
                                             FFunction::FNativeFuncPtr Thunk, TSpan<const FMetadataEntry> Metadata)
    {
        // A script's strings go away with its schema, so the entries are copied into the arena that outlives the function.
        auto CopyString = [&Arena](const char* Text)
        {
            const size_t Length = Text != nullptr ? std::char_traits<char>::length(Text) : 0;
            char* Copy = (char*)Arena.AllocateBytes(Length + 1, alignof(char));
            Memory::Memcpy(Copy, Text != nullptr ? Text : "", Length + 1);
            return (const char*)Copy;
        };

        FMetadataEntry* Owned = nullptr;
        if (!Metadata.IsEmpty())
        {
            Owned = (FMetadataEntry*)Arena.AllocateBytes(sizeof(FMetadataEntry) * Metadata.Num(), alignof(FMetadataEntry));
            for (int32 Index = 0; Index < Metadata.Num(); ++Index)
            {
                Owned[Index] = FMetadataEntry{ CopyString(Metadata[Index].Key), CopyString(Metadata[Index].Value) };
            }
        }
        return BuildCore(Arena, Owner, Name, Flags, OrderedParams, ReturnIndex, ParmsSize, Thunk,
                         TSpan<const FMetadataEntry>(Owned, Metadata.Num()));
    }

    FFunction* FFunctionBuilder::BuildCore(FPropertyArena& Arena, CStruct* Owner, const FName& Name,
                                           EFunctionFlags Flags, TSpan<FProperty* const> OrderedParams,
                                           int32 ReturnIndex, uint16 ParmsSize, FFunction::FNativeFuncPtr Thunk,
                                           TSpan<const FMetadataEntry> Metadata)
    {
        const uint16 NumParams = (uint16)OrderedParams.Num();

        // Every reader finds the return as the last parameter, so one anywhere else would be read as an argument.
        if (ReturnIndex >= 0 && ReturnIndex != (int32)NumParams - 1)
        {
            LOG_ERROR("Reflected function '{}' names parameter {} as its return, and only its last of {} can be",
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

                // A parameter answers for the function's owner, so a walker can get back to the type. Only
                // when it has none: an override borrows its base's parameters, and claiming them would
                // retarget the base's own function to whichever subclass was minted last.
                if (ParamArray[Index]->OwnerStruct == nullptr)
                {
                    ParamArray[Index]->OwnerStruct = Owner;
                }
            }
        }

        Function->Name        = Name;
        Function->OwnerStruct = Owner;
        Function->Params      = ParamArray;
        Function->Thunk       = Thunk;
        Function->Flags       = Flags;
        Function->NumParams   = NumParams;
        Function->ParmsSize   = ParmsSize;
        Function->Metadata    = Metadata.GetData();
        Function->NumMetadata = (uint16)Metadata.Num();

        // The flag is the only record of a return, so it follows the index rather than whatever the caller passed.
        if (ReturnIndex >= 0)
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

    size_t FFunctionFrame::OffsetOf(int32 Index) const
    {
        const TSpan<FProperty* const> Params = Function.GetParams();
        DEBUG_ASSERT(Params.IsValidIndex(Index));
        return Params[Index]->Offset;
    }

    size_t FFunctionFrame::ReturnOffset() const
    {
        const FProperty* Return = Function.GetReturnParam();
        DEBUG_ASSERT(Return != nullptr);
        return Return->Offset;
    }
}
