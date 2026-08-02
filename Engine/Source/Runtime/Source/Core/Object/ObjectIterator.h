#pragma once
#include "ObjectArray.h"
#include "ObjectBase.h"
#include "Containers/Array.h"

namespace Lumina
{
    class CObjectBase;

    template<typename T>
    requires !std::is_same_v<T, CObjectBase>
    class TObjectIterator
    {
    public:
        using ValueType = T*;

        explicit TObjectIterator(bool bIncludeDerivedClasses = true)
            : Index(0)
        {
            GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
            {
                if (!Object)
                {
                    return;
                }

                if (bIncludeDerivedClasses)
                {
                    if (Object->IsA<T>())
                    {
                        AllObjects.push_back(Object);
                    }
                }
                else
                {
                    if (Object->GetClass() == T::StaticClass())
                    {
                        AllObjects.push_back(Object);
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
            return Index < AllObjects.size();
        }

    private:
        void AdvanceToNextValid()
        {
            while (Index < AllObjects.size() && AllObjects[Index] == nullptr)
            {
                ++Index;
            }
        }

        TFixedVector<CObjectBase*, 10> AllObjects;
        size_t Index;
    };
}

