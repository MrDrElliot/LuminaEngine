
#ifdef RMLUI_TRACY_MEMORY_PROFILING
	#include <cstdlib>
	#include <stddef.h>
	#include <tracy/public/tracy/Tracy.hpp>

void* operator new(size_t n)
{
	// Overload global new and delete for memory inspection
	void* ptr = std::malloc(n);
	TracyAlloc(ptr, n);
	return ptr;
}

void operator delete(void* ptr) noexcept
{
	TracyFree(ptr);
	std::free(ptr);
}

#endif
