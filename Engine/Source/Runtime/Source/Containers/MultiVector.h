#pragma once
#include "Array.h"
#include "Tuple.h"

namespace Lumina
{
    template<typename... Ts>
    class TMultiVector
    {
    public:
        
        template<typename T, typename... TArgs>
        void Emplace(TArgs&&... Args)
        {
            eastl::get<TVector<std::decay_t<T>>>(Tuple).emplace(std::forward<TArgs>(Args)...);
        }

        template<typename T>
        void PushBack(T&& Val)
        {
            eastl::get<TVector<std::decay_t<T>>>(Tuple).push_back(std::forward<T>(Val));
        }

        template<typename TLambda>
        void ForEach(TLambda Lambda)
        {
            Internal_ForEach<TLambda, Ts...>(Lambda);
        }
    
    private:

        template<typename TLambda, typename T>
        void Internal_ForEach(TLambda Lambda)
        {
            TVector<T>& Vector = eastl::get<TVector<T>>(Tuple);
            for (auto It = Vector.begin(); It != Vector.end(); ++It)
            {
                Lambda(*It);
            }
        }

        template<typename TLambda, typename T, typename Arg, typename... Args>
        void Internal_ForEach(TLambda Lambda)
        {
            Internal_ForEach<TLambda, T>(Lambda);
            Internal_ForEach<TLambda, Arg, Args...>(Lambda);
        }

    private:

        TTuple<TVector<Ts>...> Tuple;
    };
}