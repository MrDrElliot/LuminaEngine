#pragma once

#include <eastl/utility.h>

namespace Lumina
{
    template<typename TFunc>
    class TDefer
    {
    public:

        explicit TDefer(TFunc&& InFunc): Func(eastl::forward<TFunc>(InFunc)) {}
        ~TDefer() { Func(); }

        TDefer(const TDefer&) = delete;
        TDefer& operator=(const TDefer&) = delete;
        TDefer(TDefer&&) = delete;
        TDefer& operator=(TDefer&&) = delete;
        

    private:

        TFunc Func;
    };
    
    struct DeferHelper
    {
        template<typename F>
        TDefer<F> operator+(F&& f)
        {
            return TDefer<F>(eastl::forward<F>(f));
        }
    };


#define DEFER_CONCAT_IMPL(x, y) x##y
#define DEFER_CONCAT(x, y) DEFER_CONCAT_IMPL(x, y)
#define DEFER auto DEFER_CONCAT(_defer_, __LINE__) = DeferHelper() + [&]()
}