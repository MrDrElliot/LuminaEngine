namespace LuminaSharp;

// A property offset resolved on first access, since a minted class gains its properties after a static initializer could run.
public struct LazyPropertyOffset
{
    private nint Value;
    private bool bResolved;

    // Read once into a static readonly, which tiered code folds to a constant, and -1 when the class has no such property yet.
    public static nint Resolve(System.Type Owner, string Property) => NativeBindings.TryPropertyOffset(NativeTypeName.Of(Owner), Property);

    // A miss is not cached, since the class may still be gaining properties, and throws rather than reaching field 0.
    public nint Get(System.Type Owner, string Property)
    {
        if (!bResolved)
        {
            string Type = NativeTypeName.Of(Owner);
            int Offset = NativeBindings.TryPropertyOffset(Type, Property);
            if (Offset < 0)
            {
                throw new System.InvalidOperationException($"{Type}.{Property} did not resolve against native reflection, so its C# accessor has no field to reach.");
            }
            Value = Offset;
            bResolved = true;
        }
        return Value;
    }
}

// The FProperty token counterpart of LazyPropertyOffset, resolved on first access for the same reason.
public struct LazyPropertyToken
{
    private System.IntPtr Value;
    private bool bResolved;

    // A miss is not cached either, for the same reason as the offset.
    public System.IntPtr Get(System.Type Owner, string Property)
    {
        if (!bResolved)
        {
            Value = NativeBindings.FindProperty(NativeTypeName.Of(Owner), Property);
            bResolved = Value != System.IntPtr.Zero;
        }
        return Value;
    }
}
