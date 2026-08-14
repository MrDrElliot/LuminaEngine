#include <cstdlib>
#include <stdio.h>
#include "EASTL/allocator.h"

namespace eastl
{
    allocator g_defaultAllocator;

    allocator* GetDefaultAllocator()
    {
        return &g_defaultAllocator;
    }

    allocator* SetDefaultAllocator(allocator* pAllocator)
    {
        return &g_defaultAllocator;
    }

    allocator::allocator(const char* EASTL_NAME(pName))
    {
        #if LE_DEBUG
        mpName = pName;
        #endif
    }

    allocator::allocator(const allocator& EASTL_NAME(alloc))
    {
        #if LE_DEBUG
        mpName = EASTL_ALLOCATOR_DEFAULT_NAME;
        #endif
    }

    allocator::allocator(const allocator&, const char* EASTL_NAME(pName))
    {
        #if LE_DEBUG
        mpName = pName;
        #endif
    }

    allocator& allocator::operator=(const allocator& EASTL_NAME(alloc))
    {
        return *this;
    }

    const char* allocator::get_name() const
    {
        return EASTL_ALLOCATOR_DEFAULT_NAME;
    }

    void allocator::set_name(const char* EASTL_NAME(pName))
    {
        // Implement set_name logic
    }

    void* allocator::allocate(size_t n, int flags)
    {
        return malloc(n);
    }

    void* allocator::allocate(size_t n, size_t alignment, size_t offset, int flags)
    {
        return malloc(n);
    }

    void allocator::deallocate(void* p, size_t)
    {
        free(p);
    }

    bool operator==(allocator const&, allocator const&) { return true; }
    bool operator!=(allocator const&, allocator const&) { return false; }
}

// Required for EASTL debug operator new[]
void* operator new[](size_t size, const char* pName, int flags, unsigned int debugFlags, const char* file, int line) 
{
    return malloc(size);
}

// Custom definition of aligned new[] operator for EASTL
void* operator new[](size_t size, size_t alignment, size_t /*alignmentOffset*/, const char* /*pName*/, int /*flags*/, unsigned /*debugFlags*/, const char* /*file*/, int /*line*/)
{
    EASTL_ASSERT(alignment <= 8); // Ensure alignment is within supported limits
    return malloc(size); // Using system new for the aligned allocation
}

#if !EASTL_EASTDC_VSNPRINTF
// Custom Vsnprintf implementation for platforms that do not have EASTL's VSnprintf
int Vsnprintf8(char* pDestination, size_t n, const char* pFormat, va_list arguments)
{
#ifdef _MSC_VER
    return _vsnprintf(pDestination, n, pFormat, arguments);
#else
    return vsnprintf(pDestination, n, pFormat, arguments);
#endif
}

int Vsnprintf16(char16_t* pDestination, size_t n, const char16_t* pFormat, va_list arguments)
{
#ifdef _MSC_VER
    return _vsnwprintf((wchar_t*)pDestination, n, (wchar_t*)pFormat, arguments);
#else
    (void)pDestination;
    (void)n;
    (void)pFormat;
    (void)arguments;

    return -1;
#endif
}
#endif // !EASTL_EASTDC_VSNPRINTF

