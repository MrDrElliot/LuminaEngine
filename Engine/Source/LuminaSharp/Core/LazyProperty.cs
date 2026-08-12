namespace LuminaSharp;

/// <summary>
/// A once-resolved reflected-property offset, for the accessors ScriptPropertyRewriter emits.
///
/// Why not a <c>static readonly</c> initializer, which is what the Reflector emits for native components:
/// a script class is MINTED at load, and its properties are appended to it after the class object exists but
/// before anything can use it. A static initializer would fire during type initialization, which can happen
/// while the engine is still describing the type (the schema pass), and resolve against a class that has no
/// properties yet -- caching a bad offset forever and logging an error per property per load. Resolving on
/// first real access moves that to a point where the class is complete, and it is still exactly one resolve.
/// </summary>
public struct LazyPropertyOffset
{
    private nint Value;
    private bool bResolved;

    public nint Get(string Type, string Property)
    {
        if (!bResolved)
        {
            Value = NativeBindings.PropertyOffset(Type, Property);
            bResolved = true;
        }
        return Value;
    }
}

/// <summary>The FProperty* token counterpart of <see cref="LazyPropertyOffset"/>, resolved on the same
/// terms and for the same reason.</summary>
public struct LazyPropertyToken
{
    private System.IntPtr Value;
    private bool bResolved;

    public System.IntPtr Get(string Type, string Property)
    {
        if (!bResolved)
        {
            Value = NativeBindings.FindProperty(Type, Property);
            bResolved = true;
        }
        return Value;
    }
}
