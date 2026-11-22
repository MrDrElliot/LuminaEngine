#pragma once

// Force-included in every TU. LuminaBuildTool defines the per-module export macros on the command line, so none are declared here.

#ifndef REFLECTION_PARSER

// Interop thunks are resolved by name at runtime, so they need an export table in every build mode and this is always dllexport.
#define LUMINA_SCRIPT_API DLL_EXPORT

#else // REFLECTION_PARSER

	// The libclang frontend never sees __declspec; keep the interop macro empty while parsing.
	#define LUMINA_SCRIPT_API

#endif // REFLECTION_PARSER
