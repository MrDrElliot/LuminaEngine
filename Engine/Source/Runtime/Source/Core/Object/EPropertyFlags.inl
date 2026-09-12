// The reflected property-flag vocabulary, single-sourced so the enum, its name table and the C# mirror can
// never drift. LE_PROPERTY_FLAG(Name, BitIndex).
//
// Mirrored by LuminaSharp.EPropertyFlags, which is checked member-by-member at bootstrap against the name
// lookup built from this file. Appending is safe; renumbering an existing flag is not, because a bit is what
// a cooked package and a replicated field already agree on.

LE_PROPERTY_FLAG(Editable,            0)   // shown in the details panel
LE_PROPERTY_FLAG(ReadOnly,            1)   // shown but not editable
LE_PROPERTY_FLAG(NoSerialize,         2)   // never written to a package
LE_PROPERTY_FLAG(Const,               3)   // not writable through reflection at all
LE_PROPERTY_FLAG(Private,             4)
LE_PROPERTY_FLAG(Protected,           5)
LE_PROPERTY_FLAG(SubField,            6)   // an inner of a container, not a member in its own right
LE_PROPERTY_FLAG(Trivial,             7)
LE_PROPERTY_FLAG(EntityHandle,        8)   // a raw entity id prefab instancing has to remap
LE_PROPERTY_FLAG(Builtin,             9)
LE_PROPERTY_FLAG(BulkSerialize,      10)
LE_PROPERTY_FLAG(EditorOnly,         11)   // stripped from cooked packages
LE_PROPERTY_FLAG(Replicated,         12)   // participates in network replication
LE_PROPERTY_FLAG(ScriptReadOnly,     13)   // C# wrapper emits a getter only
LE_PROPERTY_FLAG(ScriptWritable,     14)   // C# wrapper emits a setter even when editor read-only
LE_PROPERTY_FLAG(ScriptHidden,       15)   // no C# wrapper member emitted
LE_PROPERTY_FLAG(DuplicateTransient, 16)   // duplication resets rather than copies
