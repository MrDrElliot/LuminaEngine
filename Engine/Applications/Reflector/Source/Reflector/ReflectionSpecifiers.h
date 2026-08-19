#pragma once

#include <clang-c/Index.h>
#include <cstdint>
#include <string>
#include <vector>
#include "Reflector/Utils/MetadataUtils.h"

namespace Lumina::Reflection
{
    // Which reflection macro a specifier may appear inside.
    enum class ESpecifierTarget : uint8_t
    {
        Reflect,
        Property,
        Function,
        ScriptExport,
        Size,
    };

    // How a specifier is written at the call site.
    enum class ESpecifierForm : uint8_t
    {
        Flag,   // bare keyword, PROPERTY(Editable)
        Value,  // key and value, PROPERTY(Category = "Rendering")
        Either, // legal both ways, PROPERTY(Getter) or PROPERTY(Getter = "GetFoo")
    };

    // Which layer reads the specifier after the Reflector has parsed it.
    enum class ESpecifierConsumer : uint8_t
    {
        Reflector, // changes the generated C++
        Runtime,   // read through CStruct/FProperty metadata at runtime
        Editor,    // drives the property grid or another editor surface only
        Script,    // shapes the generated C# binding
    };

    // Rows are Name, Form, Consumer, Doc; a specifier absent from its table warns LRT1009 at its call site.

#define LUMINA_REFLECT_SPECIFIERS(X) \
    X(ReflectedName,        Value,  Reflector, "Registers the type under this name instead of its C++ spelling. Required on a REFLECT'd alias or template instantiation.") \
    X(MinimalAPI,           Flag,   Reflector, "Exports only StaticStruct()/StaticClass() across the module boundary instead of force-exporting every member.") \
    X(Component,            Flag,   Runtime,   "Marks the struct as an ECS component. Registers component meta and opts the EnTT pool into in_place_delete.") \
    X(System,               Flag,   Runtime,   "Marks the struct as an ECS system and registers it with the system registry.") \
    X(Event,                Flag,   Runtime,   "Marks the struct as an ECS event and registers it with the event registry.") \
    X(BitMask,              Flag,   Runtime,   "Marks an enum as a set of bit flags. CEnum::IsBitmaskEnum() reports it and the editor draws checkboxes.") \
    X(ConfigFile,           Value,  Runtime,   "Backs the class with the named config file. The config system loads and saves its properties there.") \
    X(Category,             Value,  Editor,    "Groups the type under a named heading in the component picker and the settings list.") \
    X(DisplayName,          Value,  Editor,    "Overrides the label shown for the type in editor UI.") \
    X(ToolTip,              Value,  Editor,    "Hover text for the type. Filled from the declaration's doc comment when not written by hand.") \
    X(HideInComponentList,  Flag,   Editor,    "Hides the component from the scene editor's add-component list.") \
    X(HideInDetails,        Flag,   Editor,    "Hides the type from the details panel.") \
    X(NotPlaceable,         Flag,   Editor,    "Excludes the class from the node graph's placeable node registry.") \
    X(Scriptable,           Flag,   Script,    "Emits a C# subclassable wrapper. Every reflected virtual on the class becomes overridable from C#.") \
    X(ScriptFastCalls,      Flag,   Script,    "Applies SuppressGCTransition to every binding on the type. FUNCTION(NoSuppressGCTransition) opts one back out.") \
    X(CSharpValueMirror,    Flag,   Script,    "The type has a hand-written blittable C# value struct. The emitter marshals it by value and generates no wrapper.") \
    X(NoCSharp,             Flag,   Script,    "Suppresses C# binding generation for the type.")

#define LUMINA_PROPERTY_SPECIFIERS(X) \
    X(Getter,               Either, Reflector, "Routes reads through the named accessor. Defaults to Get<PropertyName> when no value is given.") \
    X(Setter,               Either, Reflector, "Routes writes through the named accessor. Defaults to Set<PropertyName> when no value is given.") \
    X(ReflectAs,            Value,  Reflector, "Reflects the field as the named type instead of its declared type.") \
    X(NoSerialize,          Flag,   Runtime,   "Excludes the property from serialization.") \
    X(EditorOnly,           Flag,   Runtime,   "The property exists for editor tooling only and is stripped from cooked packages.") \
    X(Replicated,           Flag,   Runtime,   "The property participates in network replication.") \
    X(DuplicateTransient,   Flag,   Runtime,   "Resets the property to its default when the owning object is duplicated.") \
    X(StructBase,           Value,  Runtime,   "Constrains a bare FInstancedStruct to structs deriving from the named base.") \
    X(Editable,             Flag,   Editor,    "Shows the property in the details panel and allows editing.") \
    X(ReadOnly,             Flag,   Editor,    "Shows the property in the details panel with editing disabled. Do not combine with Editable.") \
    X(Category,             Value,  Editor,    "Groups the property under a named heading. Nested headings separate with a pipe.") \
    X(DisplayName,          Value,  Editor,    "Overrides the label shown for the property.") \
    X(ToolTip,              Value,  Editor,    "Hover text for the property. Filled from the declaration's doc comment when not written by hand.") \
    X(ClampMin,             Value,  Editor,    "Lower bound applied to a numeric property by the editor widget.") \
    X(ClampMax,             Value,  Editor,    "Upper bound applied to a numeric property by the editor widget.") \
    X(Delta,                Value,  Editor,    "Per-pixel drag step for a numeric property.") \
    X(Units,                Value,  Editor,    "Unit suffix appended to a numeric property's displayed value.") \
    X(Color,                Flag,   Editor,    "Draws the numeric vector as a color swatch and picker.") \
    X(NoDrag,               Flag,   Editor,    "Disables click-drag editing on a numeric property.") \
    X(Multiline,            Flag,   Editor,    "Draws a string property as a multi-line text box.") \
    X(FilePath,             Flag,   Editor,    "Draws a string property with a file browse button.") \
    X(InputAction,          Flag,   Editor,    "Draws a string property as an input action picker.") \
    X(BonePicker,           Flag,   Editor,    "Draws a string property as a skeleton bone picker.") \
    X(SocketPicker,         Flag,   Editor,    "Draws a string property as a mesh socket picker.") \
    X(CurvePicker,          Flag,   Editor,    "Draws a string property as an animation curve picker.") \
    X(ParameterPicker,      Flag,   Editor,    "Draws a string property as a material parameter picker.") \
    X(ObjectParameterPicker,Flag,   Editor,    "Draws a string property as an object-valued material parameter picker.") \
    X(AssetType,            Value,  Editor,    "Restricts an asset reference picker to the named asset class.") \
    X(RowType,              Value,  Editor,    "Restricts a data table row handle picker to the named row struct.") \
    X(Entity,               Flag,   Editor,    "Draws the property as an entity reference picker.") \
    X(NoReorder,            Flag,   Editor,    "Removes the drag handles from an array property.") \
    X(NoResize,             Flag,   Editor,    "Removes the add and remove buttons from an array property.") \
    X(DefaultCollapsed,     Flag,   Editor,    "Draws a struct or instanced struct property collapsed on first open.") \
    X(EditCondition,        Value,  Editor,    "Disables the property while the expression is false. Terms are Prop, !Prop, Prop == Value or Prop != Value, joined by && or ||.") \
    X(EditConditionHides,   Flag,   Editor,    "Hides the property instead of disabling it while its EditCondition is false.") \
    X(RequiresRecook,       Flag,   Editor,    "Editing the property triggers a recook of the owning asset.") \
    X(ScriptReadOnly,       Flag,   Script,    "The generated C# wrapper emits a getter only, whatever the editor flags say.") \
    X(ScriptWritable,       Flag,   Script,    "The generated C# wrapper emits a setter, whatever the editor flags say.") \
    X(ScriptHidden,         Flag,   Script,    "Emits no C# wrapper member and no native accessor for the property.") \
    X(NotScriptable,        Flag,   Script,    "Alias of ScriptHidden.") \
    X(SkipHotReload,        Flag,   Script,    "Excludes the property from script hot-reload state migration.")

#define LUMINA_FUNCTION_SPECIFIERS(X) \
    X(ToolTip,                  Value, Editor, "Hover text for the function. Filled from the declaration's doc comment when not written by hand.") \
    X(SuppressGCTransition,     Flag,  Script, "Skips the GC transition on the generated C# call. Only valid on a short leaf function that never calls back into managed code.") \
    X(NoSuppressGCTransition,   Flag,  Script, "Opts one function back out of a type-wide REFLECT(ScriptFastCalls).")

#define LUMINA_SCRIPT_EXPORT_SPECIFIERS(X) \
    X(Class,                Value, Script, "Required. The C# static class the free function binds to, optionally namespaced.") \
    X(SuppressGCTransition, Flag,  Script, "Skips the GC transition on the generated C# call. Only valid on a short leaf function.")

    // Injected by the scripting layer at runtime, never authored in a macro, so validation skips them.
#define LUMINA_RUNTIME_INJECTED_METADATA(X) \
    X(Aliases,            Value, Runtime, "Prior C# type names for a script type, written by the host so renamed scripts still resolve.") \
    X(ScriptTypeName,     Value, Runtime, "The C# type name behind a script-backed struct, stable across reloads unlike its object name.") \
    X(ScriptInstanceBase, Flag,  Runtime, "Marks the generated base struct of a script type so pickers offer the concrete subtypes instead.")

    struct FSpecifierInfo
    {
        const char*        Name;
        ESpecifierForm     Form;
        ESpecifierConsumer Consumer;
        const char*        Documentation;
    };

    const char* SpecifierTargetToString(ESpecifierTarget Target);

    // The specifier table for one macro, with its length in OutCount.
    const FSpecifierInfo* GetSpecifiers(ESpecifierTarget Target, uint32_t& OutCount);

    const FSpecifierInfo* FindSpecifier(ESpecifierTarget Target, const std::string& Key);

    // Nearest known specifier within a small edit distance, or nullptr. Turns a typo into a suggestion.
    const FSpecifierInfo* SuggestSpecifier(ESpecifierTarget Target, const std::string& Key);

    // Warns LRT1009 for each specifier missing from the target's table, naming the nearest known one.
    void ValidateSpecifiers(const CXCursor& Cursor, ESpecifierTarget Target, const std::vector<FMetadataPair>& Metadata);
}
