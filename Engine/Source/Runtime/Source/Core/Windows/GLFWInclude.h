#pragma once

// Centralized GLFW include. GLFW forgets to undef APIENTRY (unlike WINGDIAPI/CALLBACK),
// leaking __stdcall that clashes with the Windows headers (C4005). Mirror GLFW's own cleanup.
#include "GLFW/glfw3.h"

#ifdef GLFW_APIENTRY_DEFINED
    #undef APIENTRY
    #undef GLFW_APIENTRY_DEFINED
#endif
