#pragma once

#include "Containers/Name.h"
#include "Containers/Span.h"
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CStruct;
    class FProperty;
    class FPropertyArena;
}

namespace Lumina
{
    enum class EFunctionFlags : uint32
    {
        None            = 0,

        // no implicit object argument, so Invoke takes a null context
        Static          = BIT(0),

        // declared const, which is what lets a const object be the context
        Const           = BIT(1),

        // has a vtable slot, so a derived type can replace the body the thunk reaches
        Virtual         = BIT(2),

        // a script subclass of the owning type may override it
        Scriptable      = BIT(3),

        // callable from script rather than only from native
        ScriptCallable  = BIT(4),

        // returns something, so ReturnParam is set
        HasReturn       = BIT(5),
    };

    ENUM_CLASS_FLAGS(EFunctionFlags);

    /**
     * A reflected function: its parameters as FProperty, and a thunk that calls the real one.
     *
     * Deliberately not a CObject. Unreal's UFunction is one, which buys a name, an outer and a class at the
     * cost of an object-array slot, a refcount and a vtable per function; none of that is wanted for what is
     * a call descriptor. This is a plain type in the owning struct's property arena, sized like an FProperty
     * and trivially destructible, so a type's whole reflected surface is still one allocation.
     *
     * Parameters are FProperty because that is what makes every reflected type callable for free: the frame
     * is described by offsets and the properties themselves know how to construct, destruct, copy and
     * serialize their kind. Adding a property type does not teach this anything.
     */
    class FFunction
    {
    public:

        /** Calls the real function, unpacking Frame into typed arguments. Generated per function. */
        using FNativeFuncPtr = void (*)(void* Context, void* Frame);

        /** The alignment every frame is given, so a frame never has to carry its own. */
        static constexpr size_t kFrameAlignment = alignof(std::max_align_t);

        NODISCARD const FName& GetFunctionName() const { return Name; }

        NODISCARD CStruct* GetOwnerStruct() const { return OwnerStruct; }

        /** Parameters in declaration order, the return value last when there is one. */
        NODISCARD TSpan<FProperty* const> GetParams() const { return TSpan<FProperty* const>(Params, NumParams); }

        /** Arguments only, so a caller can walk what it has to fill. */
        NODISCARD TSpan<FProperty* const> GetArguments() const
        {
            return TSpan<FProperty* const>(Params, HasReturn() ? NumParams - 1u : NumParams);
        }

        NODISCARD FProperty* GetReturnParam() const { return ReturnParam; }

        NODISCARD uint16 GetParmsSize() const { return ParmsSize; }

        NODISCARD EFunctionFlags GetFunctionFlags() const { return Flags; }

        NODISCARD bool IsStatic() const         { return EnumHasAnyFlags(Flags, EFunctionFlags::Static); }
        NODISCARD bool IsConst() const          { return EnumHasAnyFlags(Flags, EFunctionFlags::Const); }
        NODISCARD bool IsVirtual() const        { return EnumHasAnyFlags(Flags, EFunctionFlags::Virtual); }
        NODISCARD bool IsScriptable() const     { return EnumHasAnyFlags(Flags, EFunctionFlags::Scriptable); }
        NODISCARD bool IsScriptCallable() const { return EnumHasAnyFlags(Flags, EFunctionFlags::ScriptCallable); }
        NODISCARD bool HasReturn() const        { return EnumHasAnyFlags(Flags, EFunctionFlags::HasReturn); }

        /** Brings a caller-owned frame of GetParmsSize() bytes up to a valid, default set of arguments. */
        RUNTIME_API void InitializeFrame(void* Frame) const;

        /** Tears the frame back down. Every path out of a call has to reach this, or a string param leaks. */
        RUNTIME_API void DestructFrame(void* Frame) const;

        /** Frame must have been initialized and filled. Context is the object, or null for a static. */
        RUNTIME_API void Invoke(void* Context, void* Frame) const;

    private:

        friend class FFunctionBuilder;

        FName             Name;
        CStruct*          OwnerStruct = nullptr;
        FProperty* const* Params      = nullptr;
        FProperty*        ReturnParam = nullptr;
        FNativeFuncPtr    Thunk       = nullptr;
        EFunctionFlags    Flags       = EFunctionFlags::None;
        uint16            NumParams   = 0;
        uint16            ParmsSize   = 0;
    };

    struct FFunctionParams;

    /** Assembles an FFunction in a struct's property arena. The one writer of FFunction's members. */
    class FFunctionBuilder
    {
    public:

        /** Null when the declaration is inconsistent, which is reported rather than half-built. */
        RUNTIME_API static FFunction* Build(FPropertyArena& Arena, CStruct* Owner, const FFunctionParams& Params,
                                            TSpan<FProperty* const> OrderedParams);
    };

    /**
     * A frame for one call, constructed and destructed with the parameters in it.
     *
     * Small frames stay on the stack, which is every reflected signature in practice; a larger one falls back
     * to the heap rather than capping what can be called.
     */
    class FFunctionFrame
    {
    public:

        RUNTIME_API explicit FFunctionFrame(const FFunction& InFunction);
        RUNTIME_API ~FFunctionFrame();

        LE_NO_COPYMOVE(FFunctionFrame);

        NODISCARD void* GetMemory() const { return Memory; }

        /** Typed access to one argument, by its index in GetParams(). UB if T is not the parameter's type. */
        template<typename T>
        T& At(size_t Index) const
        {
            return *reinterpret_cast<T*>(static_cast<uint8*>(Memory) + OffsetOf(Index));
        }

        /** The return slot, which is only valid after Invoke. UB if T is not the return type. */
        template<typename T>
        T& Return() const
        {
            return *reinterpret_cast<T*>(static_cast<uint8*>(Memory) + ReturnOffset());
        }

        RUNTIME_API void Invoke(void* Context) const;

    private:

        RUNTIME_API size_t OffsetOf(size_t Index) const;
        RUNTIME_API size_t ReturnOffset() const;

        static constexpr size_t kInlineBytes = 128;

        const FFunction& Function;
        void*            Memory = nullptr;
        alignas(FFunction::kFrameAlignment) uint8 Inline[kInlineBytes];
    };
}
