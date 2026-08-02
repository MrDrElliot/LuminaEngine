#pragma once


#include <Memory/MemoryConcurrentQueue.h>
#include "concurrentqueue.h"
#include "EASTL/fixed_hash_map.h"
#include "Core/DisableAllWarnings.h"
#include "EASTL/fixed_hash_set.h"
#include "EASTL/fixed_list.h"
#include "EASTL/fixed_slist.h"
#include "EASTL/intrusive_list.h"
#include "EASTL/priority_queue.h"
#include "EASTL/slist.h"
#include "EASTL/vector_map.h"
#include "EASTL/bonus/fixed_ring_buffer.h"
#include "EASTL/bonus/fixed_tuple_vector.h"
#include "EASTL/bonus/ring_buffer.h"
#include "EASTL/bonus/tuple_vector.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#define LUMINA_USE_EASTL 1

PRAGMA_DISABLE_ALL_WARNINGS
#if defined(LUMINA_USE_EASTL)
#include "EASTL/hash_map.h"
#include "EASTL/unordered_map.h"
#include "EASTL/vector.h"
#include "EASTL/fixed_vector.h"
#include "EASTL/array.h"
#include "EASTL/map.h"
#include "EASTL/queue.h"
#include "EASTL/set.h"
#include "EASTL/stack.h"
#include "EASTL/unordered_set.h"
#include "EASTL/bitset.h"
#include "EASTL/bitvector.h"
#include "EASTL/list.h"
#include "EASTL/span.h"
#else
#include <bitset>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#endif

PRAGMA_ENABLE_ALL_WARNINGS


#define InvalidIndex -1

namespace Lumina
{

#ifdef LUMINA_USE_EASTL

    template <size_t N>
    using TBitSet = eastl::bitset<N>;
    
    using FBitVector = eastl::bitvector<>;

    template<typename ... Ts>
    using TTupleVector = eastl::tuple_vector<Ts...>;

    template<size_t Size, bool bAllowOverflow = true, typename ... Ts>
    using TFixedTupleVector = eastl::fixed_tuple_vector<Size, bAllowOverflow, Ts...>;
    
    template <typename T, typename TAllocator = EASTLAllocatorType>
    using TVector = eastl::vector<T, TAllocator>;
    
    template <typename K, typename V>
    using TVectorMap = eastl::vector_map<K, V>;
    
    template <typename T>
    using TSpan = eastl::span<T>;
    
    template <typename T, size_t S, bool bOverflow = true, typename TAllocator = EASTLAllocatorType>
    using TFixedVector = eastl::fixed_vector<T, S, bOverflow, TAllocator>;
    
    template <typename T, size_t S>
    using TArray = eastl::array<T, S>;

    template <typename K, typename V>
    using TUnorderedMap = eastl::unordered_map<K, V>;
    
    template <typename K, typename V>
    using TOrderedMap = eastl::map<K, V>;
    
    template <typename K, typename V, typename H = eastl::hash<K>, typename E = eastl::equal_to<K>, typename TAllocator = EASTLAllocatorType>
    using THashMap = eastl::hash_map<K, V, H, E, TAllocator>;

    template <typename Key, typename T, size_t NodeCount, size_t BucketCount = NodeCount + 1, bool bEnableOverflow = true,
    typename Hash = eastl::hash<Key>, typename Predicate = eastl::equal_to<Key>, bool bCacheHashCode = false, typename OverflowAllocator = EASTLAllocatorType>
    using TFixedHashMap = eastl::fixed_hash_map<Key, T, NodeCount, BucketCount, bEnableOverflow, Hash, Predicate, bCacheHashCode, OverflowAllocator>;
    
    template <typename K, typename V>
    using TPair = eastl::pair<K, V>;
    
    template <typename T>
    using TList = eastl::list<T>;
    
    template <typename T>
    using TSList = eastl::slist<T>;
    
    using FIntrusiveListNode = eastl::intrusive_list_node;

    template<typename T>
    using TIntrusiveList = eastl::intrusive_list<T>;
    
    template<typename T, size_t S>
    using TFixedList = eastl::fixed_list<T, S>;
    
    template<typename T, size_t S>
    using TFixedSList = eastl::fixed_slist<T, S>;
    
    template <typename T>
    using TSet = eastl::set<T>;
    
    template <typename T, typename H = eastl::hash<T>, typename E = eastl::equal_to<T>>
    using THashSet = eastl::hash_set<T, H>;

    template <typename T, size_t NodeCount, size_t BucketCount = NodeCount + 1, bool bEnableOverflow = true, typename H = eastl::hash<T>, typename E = eastl::equal_to<T>>
    using TFixedHashSet = eastl::fixed_hash_set<T, NodeCount, BucketCount, bEnableOverflow, H>;
    
    template <typename T>
    using TUnorderedSet = eastl::unordered_set<T>;

    template <typename T>
    using TQueue = eastl::queue<T>;

    template<typename T>
    using TPriorityQueue = eastl::priority_queue<T>;

    template<typename T, typename Traits = Memory::FTrackedConcurrentQueueTraits>
    using TConcurrentQueue = moodycamel::ConcurrentQueue<T, Traits>;
    
    template <typename T>
    using TDeque = eastl::deque<T>;
    
    template <typename T, typename C = TVector<T>>
    using TStack = eastl::stack<T, C>;

    template<typename T>
    using TRingBuffer = eastl::ring_buffer<T>;

    template<typename T, size_t N>
    using TFixedRingBuffer = eastl::fixed_ring_buffer<T, N>;
    
#else

    template <size_t N> using TBitSet = std::bitset<N>;
    // no std::bitvector equivalent; fallback to vector<bool>
    using TBitVector = std::vector<bool>;
    template <typename T> using TVector = std::vector<T>;
    template <typename T> using TSpan = std::span<T>;
    template <typename T, size_t S> using TArray = std::array<T, S>;

    template <typename K, typename V> using TUnorderedMap = std::unordered_map<K, V>;
    template <typename K, typename V> using TOrderedMap = std::map<K, V>;
    template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>> using THashMap = std::unordered_map<K, V, H, E>;
    template <typename K, typename V> using TPair = std::pair<K, V>;
    template <typename T> using TList = std::list<T>;
    template <typename T> using TSet = std::set<T>;
    template <typename T> using THashSet = std::unordered_set<T>;
    template <typename T> using TUnorderedSet = std::unordered_set<T>;

    template <typename T> using TQueue = std::queue<T>;
    template <typename T> using TDeque = std::deque<T>;
    template <typename T, typename C = TVector<T>> using TStack = std::stack<T, C>;

    // No std::fixed_vector; fallback to std::array.
    template <typename T, size_t S, bool bOverflow = true> using TFixedVector = std::array<T, S>;

#endif

    using Blob = TVector<uint8>;



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
        return eastl::find( vector.begin(), vector.end(), value );
    }

    template<typename T, typename V, typename Predicate>
    inline typename TVector<T>::const_iterator VectorFind( TVector<T> const& vector, V const& value, Predicate predicate )
    {
        return eastl::find( vector.begin(), vector.end(), value, eastl::forward<Predicate>( predicate ) );
    }

    template<typename T, typename V>
    inline void TVectorRemove(T& Vector, const V& Value)
    {
        auto it = eastl::find(Vector.begin(), Vector.end(), Value);
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
        return eastl::find( vector.begin(), vector.end(), value );
    }

    template<typename T, typename V, typename Predicate>
    inline typename TVector<T>::iterator VectorFind( TVector<T>& vector, V const& value, Predicate predicate )
    {
        return eastl::find( vector.begin(), vector.end(), value, eastl::forward<Predicate>( predicate ) );
    }

    template<typename T, typename V>
    inline bool VectorContains( TVector<T> const& vector, V const& value )
    {
        return eastl::find( vector.begin(), vector.end(), value ) != vector.end();
    }

    // Usage: VectorContains( vector, value, [] ( T const& typeRef, V const& valueRef ) { ... } );
    template<typename T, typename V, typename Predicate>
    inline bool VectorContains( TVector<T> const& vector, V const& value, Predicate predicate )
    {
        return eastl::find( vector.begin(), vector.end(), value, eastl::forward<Predicate>( predicate ) ) != vector.end();
    }

    template<typename T, typename V, typename Predicate>
    inline bool VectorContains( TVector<T> const& vector, Predicate predicate )
    {
        return eastl::find_if( vector.begin(), vector.end(), eastl::forward<Predicate>( predicate ) ) != vector.end();
    }

    template<typename T>
    inline int32_t VectorFindIndex( TVector<T> const& vector, T const& value )
    {
        auto iter = eastl::find( vector.begin(), vector.end(), value );
        if ( iter == vector.end() )
        {
            return InvalidIndex;
        }
        else
        {
            return (int32_t) ( iter - vector.begin() );
        }
    }

    template<typename T, typename V, typename Predicate>
    inline int32_t VectorFindIndex( TVector<T> const& vector, V const& value, Predicate predicate )
    {
        auto iter = eastl::find( vector.begin(), vector.end(), value, predicate );
        if ( iter == vector.end() )
        {
            return InvalidIndex;
        }
        else
        {
            return (int32_t) ( iter - vector.begin() );
        }
    }

    template<typename T, typename V, typename Predicate>
    inline int32_t VectorFindIndex( TVector<T> const& vector, Predicate predicate )
    {
        auto iter = eastl::find_if( vector.begin(), vector.end(), predicate );
        if ( iter == vector.end() )
        {
            return InvalidIndex;
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

    template<typename T, typename V, eastl_size_t S>
    NODISCARD bool VectorContains(TFixedVector<T, S> const& vector, V const& value)
    {
        return eastl::find( vector.begin(), vector.end(), value ) != vector.end();
    }

    template<typename T, eastl_size_t S, typename V, typename Predicate>
    NODISCARD bool VectorContains(TFixedVector<T, S> const& vector, V const& value, Predicate predicate)
    {
        return eastl::find( vector.begin(), vector.end(), value, eastl::forward<Predicate>( predicate ) ) != vector.end();
    }

    template<typename T, typename V, eastl_size_t S>
    NODISCARD typename TFixedVector<T, S>::const_iterator VectorFind(TFixedVector<T, S> const& vector, V const& value)
    {
        return eastl::find( vector.begin(), vector.end(), value );
    }

    // Find an element in a vector
    template<typename T, typename V, eastl_size_t S, typename Predicate>
    NODISCARD typename TFixedVector<T, S>::const_iterator VectorFind(TFixedVector<T, S> const& vector, V const& value, Predicate predicate)
    {
        return eastl::find( vector.begin(), vector.end(), value, eastl::forward<Predicate>( predicate ) );
    }

    template<typename T, typename V, eastl_size_t S>
    NODISCARD typename TFixedVector<T, S>::iterator VectorFind(TFixedVector<T, S>& vector, V const& value)
    {
        return eastl::find( vector.begin(), vector.end(), value );
    }

    // Find an element in a vector
    // Require non-const versions since we might want to modify the result
    template<typename T, typename V, eastl_size_t S, typename Predicate>
    NODISCARD typename TFixedVector<T, S>::iterator VectorFind(TFixedVector<T, S>& vector, V const& value, Predicate predicate )
    {
        return eastl::find( vector.begin(), vector.end(), value, eastl::forward<Predicate>( predicate ) );
    }

    template<typename T, typename V, eastl_size_t S>
    NODISCARD int32 VectorFindIndex(TFixedVector<T, S> const& vector, V const& value )
    {
        auto iter = eastl::find( vector.begin(), vector.end(), value );
        if ( iter == vector.end() )
        {
            return InvalidIndex;
        }
        else
        {
            return ( int32_t) ( iter - vector.begin() );
        }
    }

    template<typename T, typename V, eastl_size_t S, typename Predicate>
    NODISCARD int32 VectorFindIndex(TFixedVector<T, S> const& vector, V const& value, Predicate predicate )
    {
        auto iter = eastl::find( vector.begin(), vector.end(), value, predicate );
        if ( iter == vector.end() )
        {
            return InvalidIndex;
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

    template<typename T, eastl_size_t S>
    inline void VectorEmplaceBackUnique(TFixedVector<T,S>& vector, T&& item )
    {
        if ( !VectorContains( vector, item ) )
        {
            vector.emplace_back( item );
        }
    }

    template<typename T, eastl_size_t S>
    inline void VectorEmplaceBackUnique(TFixedVector<T, S>& vector, T const& item )
    {
        if ( !VectorContains( vector, item ) )
        {
            vector.emplace_back( item );
        }
    }
}

#undef InvalidIndex