using System.Reflection;
using Lumina;

namespace LuminaSharp;

// A miss is not cached, so a type asked for before its class is minted still resolves on a later call.
public static class NativeClass<T> where T : NativeObject
{
    private static readonly string RegisteredName = NativeTypeName.Of<T>();

    private static TSubclassOf<T> Resolved;

    public static TSubclassOf<T> Of
    {
        get
        {
            if (!Resolved.IsValid)
            {
                Resolved = TSubclassOf<T>.FromName(RegisteredName);
            }
            return Resolved;
        }
    }
}
