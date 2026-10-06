using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace LuminaSharp;

// Exposes a value, a collection of [Bind] items or a data-event-* command to RML, and a writable value is two-way.
[AttributeUsage(AttributeTargets.Property | AttributeTargets.Field | AttributeTargets.Method, Inherited = true)]
public sealed class BindAttribute : Attribute
{
    /// <summary>Override the name RML binds to (defaults to the property name).</summary>
    public string? Name { get; }

    public BindAttribute(string? Name = null)
    {
        this.Name = Name;
    }
}

/// <summary>
/// Marks a parameterless method on a <see cref="ViewModel"/> as a command callable from RML with
/// <c>data-event-click="MethodName()"</c> (or any other <c>data-event-*</c>). The method runs on the game
/// thread when the event fires.
/// </summary>
[AttributeUsage(AttributeTargets.Method, Inherited = true)]
public sealed class BindCommandAttribute : Attribute
{
    /// <summary>Override the name RML binds to (defaults to the method name).</summary>
    public string? Name { get; }

    public BindCommandAttribute(string? Name = null)
    {
        this.Name = Name;
    }
}

/// <summary>
/// Base class for an MVVM view-model bound to an RmlUi document. Expose <see cref="BindAttribute">[Bind]</see>
/// properties and <see cref="BindCommandAttribute">[BindCommand]</see> methods, then register the model with
/// <see cref="UI.AddModel"/> BEFORE loading the document that references it. The view updates automatically
/// when a bound property changes through <see cref="Set{T}"/>; commands flow back from <c>data-event-*</c>.
/// <code>
/// public sealed class HudModel : ViewModel
/// {
///     private int _health = 100;
///     [Bind] public int Health { get => _health; set => Set(ref _health, value); }
///     [BindCommand] public void Respawn() { /* ... */ }
/// }
/// </code>
/// </summary>
// The name an .rml's data-model attribute uses for a ViewModel or UIScript. The class name when absent.
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class DataModelAttribute : Attribute
{
    public DataModelAttribute(string Name)
    {
        this.Name = Name;
    }

    public string Name { get; }

    internal static string NameOf(Type Type) => (Attribute.GetCustomAttribute(Type, typeof(DataModelAttribute), false) as DataModelAttribute)?.Name ?? Type.Name;
}

public abstract class ViewModel
{
    /// <summary>The live binding, or null while the model is not registered. Set by <see cref="UIDataModel"/>.</summary>
    internal UIDataModel? Binding;

    /// <summary>True once this view-model is bound to a document and pushing changes to the view.</summary>
    public bool IsBound => Binding != null && Binding.IsValid;

    /// <summary>
    /// Setter helper for a bound property: assigns the backing field and, if the value actually changed,
    /// pushes it to the view. Use it in property setters: <c>set =&gt; Set(ref _field, value);</c>.
    /// </summary>
    protected void Set<T>(ref T Field, T Value, [CallerMemberName] string Name = "")
    {
        if (EqualityComparer<T>.Default.Equals(Field, Value))
        {
            return;
        }
        Field = Value;
        Binding?.OnPropertyChanged(Name);
    }

    /// <summary>
    /// Force a refresh of one bound property by name (for computed values, or fields mutated without
    /// <see cref="Set{T}"/>). Defaults to the calling property.
    /// </summary>
    protected void NotifyChanged([CallerMemberName] string Name = "")
    {
        Binding?.OnPropertyChanged(Name);
    }

    /// <summary>Re-push every bound property to the view. Cheap for a handful of variables; prefer
    /// <see cref="Set{T}"/> (push-on-change) over calling this every frame to avoid needless work.</summary>
    public void Refresh()
    {
        Binding?.PushAll();
    }
}
