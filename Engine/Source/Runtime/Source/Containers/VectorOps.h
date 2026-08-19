#pragma once

#include <algorithm>
#include <utility>

#include "Vector.h"

namespace Lumina
{
    /** What the VectorFindIndex family returns when the value is not there. */
    inline constexpr int32 kInvalidVectorIndex = -1;

    template<typename T>
    concept ContiguousContainer = requires(const T & t)
    {
        { t.data() } -> std::convertible_to<const void*>;
        { t.size() } -> std::convertible_to<size_t>;
    };

    template<typename T>
    concept TriviallyComparableVector =
        requires(const T& v) {
        { v.data() } -> std::convertible_to<const void*>;
        { v.size() } -> std::convertible_to<size_t>;
        typename T::value_type;
        } &&
        std::is_trivially_copyable_v<typename T::value_type>;


    template<typename T>
    inline typename TVector<T>::const_iterator VectorFind( TVector<T> const& vector, T const& value )
    {
        return std::find( vector.begin(), vector.end(), value );
    }

    template<typename T, typename V, typename Predicate>
    inline typename TVector<T>::const_iterator VectorFind( TVector<T> const& vector, V const& value, Predicate predicate )
    {
        return std::find( vector.begin(), vector.end(), value, std::forward<Predicate>( predicate ) );
    }

    template<typename T, typename V>
    inline void TVectorRemove(T& Vector, const V& Value)
    {
        auto it = std::find(Vector.begin(), Vector.end(), Value);
        if (it != Vector.end())
        {
            Vector.erase(it);
        }
    }

    template<typename T>
    inline void VectorRemoveAtIndex(TVector<T>& Vector, uint32 Index)
    {
        Vector.erase(Vector.begin() + Index);
    }

    template<typename T>
    inline typename TVector<T>::iterator VectorFind( TVector<T>& vector, T const& value )
    {
        return std::find( vector.begin(), vector.end(), value );
    }

    template<typename T, typename V, typename Predicate>
    inline typename TVector<T>::iterator VectorFind( TVector<T>& vector, V const& value, Predicate predicate )
    {
        return std::find( vector.begin(), vector.end(), value, std::forward<Predicate>( predicate ) );
    }

    template<typename T, typename V>
    inline bool VectorContains( TVector<T> const& vector, V const& value )
    {
        return std::find( vector.begin(), vector.end(), value ) != vector.end();
    }

    // Usage: VectorContains( vector, value, [] ( T const& typeRef, V const& valueRef ) { ... } );
    template<typename T, typename V, typename Predicate>
    inline bool VectorContains( TVector<T> const& vector, V const& value, Predicate predicate )
    {
        return std::find( vector.begin(), vector.end(), value, std::forward<Predicate>( predicate ) ) != vector.end();
    }

    template<typename T, typename V, typename Predicate>
    inline bool VectorContains( TVector<T> const& vector, Predicate predicate )
    {
        return std::find_if( vector.begin(), vector.end(), std::forward<Predicate>( predicate ) ) != vector.end();
    }

    template<typename T>
    inline int32_t VectorFindIndex( TVector<T> const& vector, T const& value )
    {
        auto iter = std::find( vector.begin(), vector.end(), value );
        if ( iter == vector.end() )
        {
            return kInvalidVectorIndex;
        }
        else
        {
            return (int32_t) ( iter - vector.begin() );
        }
    }

    template<typename T, typename V, typename Predicate>
    inline int32_t VectorFindIndex( TVector<T> const& vector, V const& value, Predicate predicate )
    {
        auto iter = std::find( vector.begin(), vector.end(), value, predicate );
        if ( iter == vector.end() )
        {
            return kInvalidVectorIndex;
        }
        else
        {
            return (int32_t) ( iter - vector.begin() );
        }
    }

    template<typename T, typename V, typename Predicate>
    inline int32_t VectorFindIndex( TVector<T> const& vector, Predicate predicate )
    {
        auto iter = std::find_if( vector.begin(), vector.end(), predicate );
        if ( iter == vector.end() )
        {
            return kInvalidVectorIndex;
        }
        else
        {
            return (int32_t) (iter - vector.begin());
        }
    }

    template <typename T>
    NODISCARD constexpr bool VectorsAreEqual(const T& A, const T& B)
    {
        if (A.size() != B.size())
        {
            return false;
        }
        
        return std::equal(A.begin(), A.end(), B.begin());
    }

    template <typename T>
    requires(std::is_trivially_copyable_v<typename T::value_type>)
    NODISCARD bool VectorsAreTriviallyEqual(const T& A, const T& B)
    {
        if (A.size() != B.size())
        {
            return false;
        }
    
        using ValueType = T::value_type;
        return std::memcmp(A.data(), B.data(), A.size() * sizeof(ValueType)) == 0;
    }

    template<typename T, typename V, size_t S>
    NODISCARD bool VectorContains(TFixedVector<T, S> const& vector, V const& value)
    {
        return std::find( vector.begin(), vector.end(), value ) != vector.end();
    }

    template<typename T, size_t S, typename V, typename Predicate>
    NODISCARD bool VectorContains(TFixedVector<T, S> const& vector, V const& value, Predicate predicate)
    {
        return std::find( vector.begin(), vector.end(), value, std::forward<Predicate>( predicate ) ) != vector.end();
    }

    template<typename T, typename V, size_t S>
    NODISCARD typename TFixedVector<T, S>::const_iterator VectorFind(TFixedVector<T, S> const& vector, V const& value)
    {
        return std::find( vector.begin(), vector.end(), value );
    }

    // Find an element in a vector
    template<typename T, typename V, size_t S, typename Predicate>
    NODISCARD typename TFixedVector<T, S>::const_iterator VectorFind(TFixedVector<T, S> const& vector, V const& value, Predicate predicate)
    {
        return std::find( vector.begin(), vector.end(), value, std::forward<Predicate>( predicate ) );
    }

    template<typename T, typename V, size_t S>
    NODISCARD typename TFixedVector<T, S>::iterator VectorFind(TFixedVector<T, S>& vector, V const& value)
    {
        return std::find( vector.begin(), vector.end(), value );
    }

    // Find an element in a vector
    // Require non-const versions since we might want to modify the result
    template<typename T, typename V, size_t S, typename Predicate>
    NODISCARD typename TFixedVector<T, S>::iterator VectorFind(TFixedVector<T, S>& vector, V const& value, Predicate predicate )
    {
        return std::find( vector.begin(), vector.end(), value, std::forward<Predicate>( predicate ) );
    }

    template<typename T, typename V, size_t S>
    NODISCARD int32 VectorFindIndex(TFixedVector<T, S> const& vector, V const& value )
    {
        auto iter = std::find( vector.begin(), vector.end(), value );
        if ( iter == vector.end() )
        {
            return kInvalidVectorIndex;
        }
        else
        {
            return ( int32_t) ( iter - vector.begin() );
        }
    }

    template<typename T, typename V, size_t S, typename Predicate>
    NODISCARD int32 VectorFindIndex(TFixedVector<T, S> const& vector, V const& value, Predicate predicate )
    {
        auto iter = std::find( vector.begin(), vector.end(), value, predicate );
        if ( iter == vector.end() )
        {
            return kInvalidVectorIndex;
        }
        else
        {
            return ( int32_t) ( iter - vector.begin() );
        }
    }

    template<typename T>
    inline void VectorEmplaceBackUnique( TVector<T>& vector, T&& item )
    {
        if ( !VectorContains( vector, item ) )
        {
            vector.emplace_back( item );
        }
    }

    template<typename T>
    inline void VectorEmplaceBackUnique( TVector<T>& vector, T const& item )
    {
        if ( !VectorContains( vector, item ) )
        {
            vector.emplace_back( item );
        }
    }

    template<typename T, size_t S>
    inline void VectorEmplaceBackUnique(TFixedVector<T,S>& vector, T&& item )
    {
        if ( !VectorContains( vector, item ) )
        {
            vector.emplace_back( item );
        }
    }

    template<typename T, size_t S>
    inline void VectorEmplaceBackUnique(TFixedVector<T, S>& vector, T const& item )
    {
        if ( !VectorContains( vector, item ) )
        {
            vector.emplace_back( item );
        }
    }
}
