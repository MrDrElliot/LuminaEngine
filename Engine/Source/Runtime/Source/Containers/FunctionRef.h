#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "ContainerTraits.h"
#include "Invoke.h"

namespace Lumina::Containers
{
    template <typename TSignature>
    class TFunctionRef;

    /** A non-owning view of something callable; it never allocates and must not outlive its target. */
    template <typename TRet, typename... TArgs>
    class TFunctionRef<TRet(TArgs...)>
    {
        template <typename TCallable>
        static constexpr bool bIsValidCallable =
            !std::is_same_v<std::remove_cvref_t<TCallable>, TFunctionRef> &&
            !std::is_same_v<std::remove_cvref_t<TCallable>, std::nullptr_t> &&
            std::is_invocable_r_v<TRet, TCallable&, TArgs...>;

        // A plain function is passed by reference, and only reinterpret_cast turns a function pointer into void*.
        template <typename T>
        static void* ToTarget(T* Ptr) noexcept
        {
            if constexpr (std::is_function_v<T>)
            {
                return reinterpret_cast<void*>(Ptr);
            }
            else
            {
                return const_cast<void*>(static_cast<const void*>(Ptr));
            }
        }

        template <typename TCallable>
        static TRet InvokeThunk(void* Target, TArgs&&... Args)
        {
            if constexpr (std::is_function_v<TCallable>)
            {
                return InvokeR<TRet>(*reinterpret_cast<TCallable*>(Target), std::forward<TArgs>(Args)...);
            }
            else
            {
                return InvokeR<TRet>(*static_cast<TCallable*>(Target), std::forward<TArgs>(Args)...);
            }
        }

    public:

        using result_type = TRet;

        TFunctionRef() noexcept = default;
        TFunctionRef(std::nullptr_t) noexcept {}

        template <typename TCallable>
        requires (bIsValidCallable<TCallable>)
        TFunctionRef(TCallable&& Callable) noexcept
            : Thunk(&InvokeThunk<std::remove_reference_t<TCallable>>)
            , Target(ToTarget(std::addressof(Callable)))
        {
        }

        TFunctionRef(const TFunctionRef&) noexcept = default;
        TFunctionRef& operator=(const TFunctionRef&) noexcept = default;
        TFunctionRef(TFunctionRef&&) noexcept = default;
        TFunctionRef& operator=(TFunctionRef&&) noexcept = default;
        ~TFunctionRef() = default;

        TRet operator()(TArgs... Args) const
        {
            LUMINA_CONTAINER_CHECK(Thunk != nullptr);
            return Thunk(Target, std::forward<TArgs>(Args)...);
        }

        NODISCARD explicit operator bool() const noexcept { return Thunk != nullptr; }
        NODISCARD bool IsSet() const noexcept { return Thunk != nullptr; }

        NODISCARD friend bool operator==(const TFunctionRef& Function, std::nullptr_t) noexcept
        {
            return Function.Thunk == nullptr;
        }

    private:

        TRet (*Thunk)(void* Target, TArgs&&... Args) = nullptr;
        void* Target = nullptr;
    };

    template <typename TRet, typename... TArgs>
    TFunctionRef(TRet (*)(TArgs...)) -> TFunctionRef<TRet(TArgs...)>;
}

namespace Lumina
{
    template <typename TSignature>
    using TFunctionRef = Containers::TFunctionRef<TSignature>;
}
