using System.Reflection;
using Lumina;

namespace LuminaSharp;

// A miss is not cached, so a type asked for before its class is minted still resolves on a later call.
public static class NativeClass<T> where T : NativeObject
{
    // A generated wrapper names the native class it stands for; a script class is minted under its full name.
    private static readonly string RegisteredName =
        typeof(T).GetCustomAttribute<NativeTypeAttribute>(inherit: false)?.Name
        ?? typeof(T).FullName
        ?? typeof(T).Name;

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
