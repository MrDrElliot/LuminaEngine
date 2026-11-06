using System;
using JetBrains.Annotations;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace LuminaSharp;

public abstract class ViewModel
{
    // Null while the model is not registered; set by UIDataModel.
    internal UIDataModel? Binding;

    public bool IsBound => Binding != null && Binding.IsValid;

    // Pushes to the view only when the value changed, as in set => Set(ref _field, value).
    protected void Set<T>(ref T Field, T Value, [CallerMemberName] string Name = "")
    {
        if (EqualityComparer<T>.Default.Equals(Field, Value))
        {
            return;
        }
        Field = Value;
        Binding?.OnPropertyChanged(Name);
    }

    // For computed values or fields mutated without Set.
    protected void NotifyChanged([CallerMemberName] string Name = "")
    {
        Binding?.OnPropertyChanged(Name);
    }

    // Prefer the push-on-change Set over calling this every frame.
    public void Refresh()
    {
        Binding?.PushAll();
    }
}
