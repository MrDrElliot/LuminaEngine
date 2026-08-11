#pragma once

#include "EASTL/shared_ptr.h"
#include "EASTL/unique_ptr.h"
#include "Memory/Memory.h"


namespace Lumina
{
    /**
     * Pairs with Memory::New so both ends of the ownership go through the Runtime-exported allocator.
     */
    template<typename S>
    struct smart_ptr_deleter
    {
        constexpr smart_ptr_deleter() noexcept = default;
        
        template<typename U>
        requires eastl::is_convertible_v<U*, S*>
        smart_ptr_deleter(const smart_ptr_deleter<U>&) noexcept {}

        void operator()(S* p) const
        {
            Memory::Delete(p);
        }
    };

    template<typename S> using TSharedPtr                                           = eastl::shared_ptr<S>;
    template<typename S, typename D = smart_ptr_deleter<S>> using TUniquePtr        = eastl::unique_ptr<S, D>;
    template<typename S> using TWeakPtr                                             = eastl::weak_ptr<S>;
    template<typename S> using TSharedFromThis                                      = eastl::enable_shared_from_this<S>;

    template<typename T, typename... TArgs>
    requires (std::is_constructible_v<T, TArgs...>)
    TSharedPtr<T> MakeShared(TArgs&&... Args)
    {
        return eastl::make_shared<T>(std::forward<TArgs>(Args)...);
    }

    template<typename T, typename... TArgs>
    requires (std::is_constructible_v<T, TArgs...>)
    TUniquePtr<T> MakeUnique(TArgs&&... Args)
    {
        return TUniquePtr<T>(Memory::New<T>(std::forward<TArgs>(Args)...));
    }
}
