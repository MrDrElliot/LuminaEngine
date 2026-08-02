#pragma once

#include "EASTL/shared_ptr.h"
#include "EASTL/unique_ptr.h"
#include "Memory/Memory.h"


namespace Lumina
{
    template<typename S>
    struct smart_ptr_deleter
    {
        void operator()(S* p) const
        {
            Memory::Delete(p);
        }
    };

    template<typename S> using TSharedPtr =         eastl::shared_ptr<S>;
    template<typename S> using TUniquePtr =         eastl::unique_ptr<S>;
    template<typename S> using TWeakPtr =           eastl::weak_ptr<S>;
    template<typename S> using TSharedFromThis =    eastl::enable_shared_from_this<S>;

    template<typename T, typename... Args>
    requires (std::is_constructible_v<T, Args...>)
    TSharedPtr<T> MakeShared(Args&&... args)
    {
        return eastl::make_shared<T>(std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
    requires (std::is_constructible_v<T, Args...>)
    TUniquePtr<T> MakeUnique(Args&&... args)
    {
        return eastl::make_unique<T>(std::forward<Args>(args)...);
    }
}
