#pragma once

// GLFW leaks APIENTRY as __stdcall, which clashes with the Windows headers (C4005).

// Unconditional, or glfwCreateWindowSurface is declared only where a Vulkan header came first.
#define GLFW_INCLUDE_VULKAN

#include "GLFW/glfw3.h"

#ifdef GLFW_APIENTRY_DEFINED
    #undef APIENTRY
    #undef GLFW_APIENTRY_DEFINED
#endif
