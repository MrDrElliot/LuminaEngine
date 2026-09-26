using LuminaSharp;

namespace Lumina;

// A blittable payload keeps the Nullable spelling; this carries the rest, where unset and null must stay apart.
public readonly unsafe struct TOptional<T>
{
    private readonly nint Slot;
    private readonly nint Property;

    public TOptional(nint Slot, nint Property)
    {
        this.Slot = Slot;
        this.Property = Property;
    }

    public bool IsValid => Slot != 0 && Property != 0;

    public bool HasValue => IsValid && Native.OptionalValueAt(Slot, Property) != 0;

    public T Value
    {
        get
        {
            nint Payload = IsValid ? Native.OptionalValueAt(Slot, Property) : 0;
            return Payload == 0 ? default! : ElementMarshal.Read<T>(Payload);
        }
    }

    public void Set(T Value)
    {
        if (!IsValid)
        {
            return;
        }

        // A null value engages with a default payload, which is then assigned through its own accessor.
        Native.OptionalSetValueAt(Slot, Property, 0);
        nint Payload = Native.OptionalValueAt(Slot, Property);
        if (Payload != 0)
        {
            ElementMarshal.Write<T>(Payload, Value);
        }
    }

    public void Reset()
    {
        if (IsValid)
        {
            Native.OptionalResetAt(Slot, Property);
        }
    }

    public bool TryGetValue(out T Value)
    {
        nint Payload = IsValid ? Native.OptionalValueAt(Slot, Property) : 0;
        Value = Payload == 0 ? default! : ElementMarshal.Read<T>(Payload);
        return Payload != 0;
    }
}
