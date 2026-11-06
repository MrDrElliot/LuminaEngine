; Diagnostics this analyzer can report. Roslyn requires the list (RS2008) so a rule cannot be added,
; renamed or silently dropped without it showing up in a diff.
;
; These are validation only. The accessors themselves are produced by LuminaSharp.ScriptPropertyRewriter
; inside the engine's own compilation, which runs at script load; without an analyzer saying the same thing
; at the declaration, a bad member would look fine in the IDE and fail only on reload.

### New Rules

Rule ID | Category    | Severity | Notes
--------|-------------|----------|--------------------------------------------------------------------------
LUM0101 | LuminaSharp | Error    | [Property] type cannot be viewed over native storage.
LUM0102 | LuminaSharp | Error    | [Property] container initialized; it is a view over storage native owns.
LUM0103 | LuminaSharp | Error    | [Property] declared as a partial property; declare it as a field.
LUM0104 | LuminaSharp | Error    | Stored property on an engine object is not an auto-property, so native storage would never back it.
LUM0105 | LuminaSharp | Error    | Specifier does not apply to this kind of member, as it does not in C++.
LUM0106 | LuminaSharp | Error    | Specifiers conflict, as they do in a C++ PROPERTY(...).
LUM0110 | LuminaSharp | Error    | Script struct must be partial so the engine can add its native handle.
LUM0111 | LuminaSharp | Error    | Script struct must be declared at namespace scope.
LUM0112 | LuminaSharp | Warning  | Class derives from an engine class without [Reflect], so the engine never registers it.
LUM0120 | LuminaSharp | Error    | Component held by value; it only exists in its entity's storage, so reach it through a ref or a TComponentRef.
LUM0121 | LuminaSharp | Error    | Component created as a value; add it to an entity instead.
LUM0130 | LuminaSharp | Error    | [Function] name declared more than once; native and the dispatcher look it up by name.
LUM0201 | LuminaSharp | Error    | Write through the indexer of a TVector whose element is marshalled.
LUM0301 | LuminaSharp | Error    | Instance [NativeCall] on a type with no Handle that is not a mutable unmanaged struct.
LUM0302 | LuminaSharp | Error    | [NativeCall] name declared more than once in a type; its binding is named after the method.
LUM0303 | LuminaSharp | Error    | [NativeCall] parameter passed by ref, in or out.
LUM0304 | LuminaSharp | Error    | Two different methods share one [ManagedExport] name.
LUM0305 | LuminaSharp | Error    | [ManagedExport] method is not static with a Stdcall [UnmanagedCallersOnly].
LUM0306 | LuminaSharp | Error    | [NativeCall] in a non-partial or generic type chain, or on a generic method.
