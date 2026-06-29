#pragma once

// Force-included in every TU; resolves DLL export/import macros from the per-module *_EXPORTS define.
// To add a module, add a block below.

#ifndef REFLECTION_PARSER

// Monolithic builds link statically, so define the macros empty before the per-module blocks skip them.
#ifdef LUMINA_MONOLITHIC
	#define RUNTIME_API
	#define EDITOR_API
	#define SANDBOX_API
#endif

// C#<->native interop thunks (the reflector-generated LuminaSharp_* thunks + the hand-written
// LUMINA_DOTNET_EXPORT ones) are resolved by NAME at runtime via GetProcAddress / NativeLibrary.TryGetExport,
// never linked at compile time. They must therefore live in an export table in EVERY build mode: their
// owning module's DLL in modular, or the exe itself in monolithic (Shipping whole-archives every module
// .obj into the exe, and an exe can carry an export table). So this is ALWAYS dllexport, regardless of
// LUMINA_MONOLITHIC and regardless of which module's TU the thunk lands in.
#define LUMINA_SCRIPT_API DLL_EXPORT

// Runtime
#ifndef RUNTIME_API
	#ifdef RUNTIME_EXPORTS
		#define RUNTIME_API DLL_EXPORT
	#else
		#define RUNTIME_API DLL_IMPORT
	#endif
#endif

// Editor
#ifndef EDITOR_API
	#ifdef EDITOR_EXPORTS
		#define EDITOR_API DLL_EXPORT
	#else
		#define EDITOR_API DLL_IMPORT
	#endif
#endif

// Sandbox
#ifndef SANDBOX_API
	#ifdef SANDBOX_EXPORTS
		#define SANDBOX_API DLL_EXPORT
	#else
		#define SANDBOX_API DLL_IMPORT
	#endif
#endif

#else // REFLECTION_PARSER

	// The libclang frontend never sees __declspec; keep the interop macro empty while parsing.
	#define LUMINA_SCRIPT_API

#endif // REFLECTION_PARSER
