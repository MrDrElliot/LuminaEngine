#pragma once

// Force-included in every TU.
//
// The per-module export macros are NOT declared here. LuminaBuildTool defines <NAME>_API on the
// command line for the module being compiled and for every shared library it can see, working out
// dllexport vs dllimport from the dependency graph. That is what lets a game or plugin module have
// one at all: a list in this header could only ever name the modules that ship with the engine, and
// nothing out of tree can add itself to it.
//
// What remains here is the one macro that cannot be derived that way.

#ifndef REFLECTION_PARSER

// C#<->native interop thunks (the reflector-generated LuminaSharp_* thunks + the hand-written
// LUMINA_DOTNET_EXPORT ones) are resolved by NAME at runtime via GetProcAddress / NativeLibrary.TryGetExport,
// never linked at compile time. They must therefore live in an export table in EVERY build mode: their
// owning module's DLL in modular, or the exe itself in monolithic (Shipping whole-archives every module
// .obj into the exe, and an exe can carry an export table). So this is ALWAYS dllexport, regardless of
// LUMINA_MONOLITHIC and regardless of which module's TU the thunk lands in.
#define LUMINA_SCRIPT_API DLL_EXPORT

#else // REFLECTION_PARSER

	// The libclang frontend never sees __declspec; keep the interop macro empty while parsing.
	#define LUMINA_SCRIPT_API

#endif // REFLECTION_PARSER
