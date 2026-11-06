#pragma once
#include "ObjectIteration.h"
#include "ObjectBase.h"
#include "Containers/Vector.h"

namespace Lumina
{
    class CObjectBase;

    template<typename T>
    requires (!std::is_same_v<T, CObjectBase>)
    class TObjectIterator
    {
    public:
        using ValueType = T*;

        explicit TObjectIterator(bool bIncludeDerivedClasses = true)
            : Index(0)
        {
            ForEachObject([&](CObjectBase* Object, int32)
            {
                if (!Object)
                {
                    return;
                }

                if (bIncludeDerivedClasses)
                {
                    if (Object->IsA<T>())
                    {
                        AllObjects.Add(Object);
                    }
                }
                else
                {
                    if (Object->GetClass() == T::StaticClass())
                    {
                        AllObjects.Add(Object);
                    }
                }
            });
            
            AdvanceToNextValid();
        }
        
        TObjectIterator& operator++()
        {
            ++Index;
            AdvanceToNextValid();
            return *this;
        }

        ValueType operator*() const
        {
            return static_cast<T*>(AllObjects[Index]);
        }

        explicit operator bool() const
        {
            return Index < AllObjects.Num();
        }

    private:
        void AdvanceToNextValid()
        {
            while (Index < AllObjects.Num() && AllObjects[Index] == nullptr)
            {
                ++Index;
            }
        }

        TInlineVector<CObjectBase*, 10> AllObjects;
        int32 Index;
    };
}

