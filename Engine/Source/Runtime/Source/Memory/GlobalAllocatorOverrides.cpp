// Global operator new/delete replacement, auto-added to every linked image (one definition per
// DLL/EXE, same mechanism as EASTLImpl.cpp -- see PerImageSourceFiles in Lumina.BuildRules.cs).
//
// This has to be per IMAGE, not per module, because replacing the global operators is a link-time
// decision made once per binary: a DLL without a definition of its own silently binds to the CRT's.
// That is not a local matter, because pointers cross images constantly -- an EnTT pool grown from a
// game module and freed when Runtime tears the registry down, an FStructOps built by whichever
// module declared the struct and owned by a CStruct that Runtime destroys. Whichever side ran in a
// non-replacing image hands its counterpart a pointer from the wrong heap, and rpmalloc faults
// unmapping an address it never reserved.
//
// It used to live in IMPLEMENT_MODULE, which only some images call -- NetworkingRuntime,
// NetworkingEditor, NsightPerfEditor, GameplayExtrasRuntime and the launcher all went without.
// Adding it here instead means an image cannot opt out by forgetting a macro.
//
// Both sides route to Memory::Malloc/Memory::Free, which are exported from Runtime, so every image
// ends up sharing Runtime's one rpmalloc instance no matter where the allocation happened.

#include "Memory/Memory.h"

DECLARE_MODULE_ALLOCATOR_OVERRIDES()
