#pragma once

#include "ContainerTraits.h"
#include "Memory/Memory.h"

namespace Lumina
{
    /** What a container needs from an allocator; TryExpand is what the standard model cannot express. */
    template <typename T>
    concept ContainerAllocatorType = requires(void* Ptr, size_t Bytes, size_t Alignment)
    {
        { T::Allocate(Bytes, Alignment) } -> std::same_as<void*>;
        { T::TryExpand(Ptr, Bytes, Bytes) } -> std::same_as<bool>;
        T::Deallocate(Ptr, Bytes, Alignment);
        { T::bIsStateless } -> std::convertible_to<bool>;
    };

    /** rpmalloc through Memory::Malloc, so allocations stay pool-coherent across DLL boundaries. */
    struct FHeapAllocator
    {
        static constexpr bool bIsStateless = true;

        NODISCARD static void* Allocate(size_t Bytes, size_t Alignment)
        {
            return Memory::Malloc(Bytes, Alignment);
        }

        NODISCARD static bool TryExpand(void* Ptr, size_t OldBytes, size_t NewBytes)
        {
            (void)OldBytes;
            return Memory::TryExpandInPlace(Ptr, NewBytes);
        }

        static void Deallocate(void* Ptr, size_t Bytes, size_t Alignment)
        {
            (void)Bytes;
            (void)Alignment;
            void* Local = Ptr;
            Memory::Free(Local);
        }
    };

    /** The calling thread's frame arena; reset at the frame boundary, so Deallocate is a no-op. */
    struct FFrameAllocator
    {
        static constexpr bool bIsStateless = true;

        NODISCARD static void* Allocate(size_t Bytes, size_t Alignment)
        {
            return Memory::FrameAllocate(Bytes, Alignment);
        }

        NODISCARD static bool TryExpand(void* Ptr, size_t OldBytes, size_t NewBytes)
        {
            (void)Ptr;
            (void)OldBytes;
            (void)NewBytes;
            return false;
        }

        static void Deallocate(void* Ptr, size_t Bytes, size_t Alignment)
        {
            (void)Ptr;
            (void)Bytes;
            (void)Alignment;
        }
    };

    /** The calling thread's scratch arena; an enclosing FMemMark reclaims everything, so Deallocate is a no-op. */
    struct FScratchAllocator
    {
        static constexpr bool bIsStateless = true;

        NODISCARD static void* Allocate(size_t Bytes, size_t Alignment)
        {
            return Memory::ScratchAllocate(Bytes, Alignment);
        }

        NODISCARD static bool TryExpand(void* Ptr, size_t OldBytes, size_t NewBytes)
        {
            (void)Ptr;
            (void)OldBytes;
            (void)NewBytes;
            return false;
        }

        static void Deallocate(void* Ptr, size_t Bytes, size_t Alignment)
        {
            (void)Ptr;
            (void)Bytes;
            (void)Alignment;
        }
    };
}
