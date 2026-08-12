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
