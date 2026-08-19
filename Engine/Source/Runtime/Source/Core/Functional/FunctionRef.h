#pragma once



namespace Lumina
{
    
    template<typename TSignature>
    class TFunctionRef;

    template<typename TRet, typename... TArgs>
    class TFunctionRef<TRet(TArgs...)> 
    {
        TRet (*Callback)(intptr_t Callable, TArgs... Args) = nullptr;
        intptr_t Callable = 0;

        template<typename TCallable>
        static TRet CallbackFn(intptr_t callable, TArgs... Args)
        {
            return (*reinterpret_cast<TCallable*>(callable))(std::forward<TArgs>(Args)...);
        }

        template<typename TCallable>
        struct IsValidCallable
        {
            static constexpr bool value =
                !std::is_same_v<std::decay_t<TCallable>, TFunctionRef> &&
                !std::is_same_v<std::decay_t<TCallable>, std::nullptr_t> &&
                std::is_invocable_r_v<TRet, TCallable&, TArgs...>;
        };

    public:
        
        TFunctionRef() = default;
        ~TFunctionRef() = default;
        
        TFunctionRef(std::nullptr_t) noexcept {}

        template <typename TCallable>
        TFunctionRef(TCallable&& InCallable) noexcept
        requires (IsValidCallable<TCallable>::value)
        : Callback(CallbackFn<std::remove_reference_t<TCallable>>)
        , Callable(reinterpret_cast<intptr_t>(std::addressof(InCallable)))
        {}

        TFunctionRef(const TFunctionRef&) = default;
        
        TFunctionRef(TFunctionRef&&) = default;
        
        TFunctionRef& operator=(const TFunctionRef&) = delete;
        TFunctionRef& operator=(TFunctionRef&&) = delete;
        TFunctionRef& operator=(std::nullptr_t) = delete;

        TRet operator()(TArgs... Args) const 
        {
            return Callback(Callable, std::forward<TArgs>(Args)...);
        }

        explicit operator bool() const noexcept { return Callback != nullptr; }

        friend bool operator==(const TFunctionRef& lhs, std::nullptr_t) noexcept
        {
            return lhs.Callback == nullptr;
        }

        friend bool operator==(std::nullptr_t, const TFunctionRef& rhs) noexcept
        {
            return rhs.Callback == nullptr;
        }

        friend bool operator!=(const TFunctionRef& lhs, std::nullptr_t) noexcept
        {
            return lhs.Callback != nullptr;
        }

        friend bool operator!=(std::nullptr_t, const TFunctionRef& rhs) noexcept
        {
            return rhs.Callback != nullptr;
        }
    };

    template<typename TRet, typename... TArgs>
    TFunctionRef(TRet(*)(TArgs...)) -> TFunctionRef<TRet(TArgs...)>;
    
}
