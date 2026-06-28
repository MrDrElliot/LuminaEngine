using System;

namespace LuminaSharp;

/// <summary>Declares a prior name this element was serialized under, so saved data survives a rename. Repeatable.</summary>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = true)]
public sealed class AliasAttribute : Attribute
{
    public AliasAttribute(string Name)
    {
        this.Name = Name;
    }

    /// <summary>A prior name, a full type name on a class or a member name on a property.</summary>
    public string Name { get; }
}

/// <summary>Resets this [Property] (or every [Property] on the class) to its default on a C# hot reload instead of carrying the previous value into the reloaded instance.</summary>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class SkipHotReloadAttribute : Attribute
{
}

/// <summary>
/// Exposes a script field/property to the editor (and saves it).
/// </summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class PropertyAttribute : Attribute
{
    /// <summary>Inspector group.</summary>
    public string? Category { get; set; }

    /// <summary>Hover help.</summary>
    public string? Tooltip { get; set; }

    /// <summary>Display label override (defaults to the member name).</summary>
    public string? Name { get; set; }

    /// <summary>Slider/drag lower bound (NaN = unbounded).</summary>
    public float Min { get; set; } = float.NaN;

    /// <summary>Slider/drag upper bound (NaN = unbounded).</summary>
    public float Max { get; set; } = float.NaN;

    /// <summary>Unit suffix shown after the value (e.g. "m/s").</summary>
    public string? Units { get; set; }

    /// <summary>Draw a color picker for a Vector3/Vector4 value instead of drag fields.</summary>
    public bool Color { get; set; }

    public bool HasMin => !float.IsNaN(Min);
    public bool HasMax => !float.IsNaN(Max);
}

/// <summary>
/// Persists a field/property with the entity without exposing it in the inspector.
/// </summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class SerializeAttribute : Attribute
{
}

/// <summary>Marks a field/property as never serialized or shown, even if otherwise eligible.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class HideAttribute : Attribute
{
}

/// <summary>Marks a [Property] field as an instanced (polymorphic) object. The inspector shows a type
/// picker of the concrete classes deriving from the field's declared type and edits the chosen
/// instance's own [Property] members inline. Opt-in only; an unmarked field is never instanced.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class InstancedAttribute : Attribute
{
}

/// <summary>Resolves a component-wrapper field/property (a <see cref="NativeStruct"/> subclass) and caches it
/// on the script before <see cref="EntityScript.OnReady"/>, adding the component if missing. Caching the handle
/// skips the per-frame Registry.Get; valid until the component is removed.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class RequireComponentAttribute : Attribute
{
}

/// <summary>Exposes a parameterless method as a clickable button in the script component's inspector.
/// Clicking it invokes the method on the live script instance (only while the game is running). Methods
/// taking arguments are ignored with a warning.</summary>
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
public sealed class ButtonAttribute : Attribute
{
    public ButtonAttribute()
    {
    }

    public ButtonAttribute(string Label)
    {
        this.Label = Label;
    }

    /// <summary>Button text override (defaults to the method name).</summary>
    public string? Label { get; set; }

    /// <summary>Hover help.</summary>
    public string? Tooltip { get; set; }
}
