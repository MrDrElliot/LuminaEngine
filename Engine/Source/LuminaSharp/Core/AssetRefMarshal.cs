namespace LuminaSharp;

/// <summary>
/// Reads and writes an asset-reference <c>[Property]</c> as the virtual path it is stored as natively.
///
/// Every asset-reference type (Lumina.FSoftObjectPath, TSoftObjectPtr&lt;T&gt;, TObjectPtr&lt;T&gt;) implements
/// <see cref="IAssetRef"/>, and the native side stores all of them as one FSoftObjectPath. So the rewriter
/// recognises them by that interface rather than by name, and a new asset-reference type needs no change
/// there: implement the interface and it works.
///
/// The generic constraint is what makes it correct for a struct: <c>where T : struct, IAssetRef</c> compiles
/// the interface calls as CONSTRAINED calls on the local, so <see cref="Read{T}"/> mutates the value it is
/// about to return rather than a boxed copy that would be thrown away.
/// </summary>
public static class AssetRefMarshal
{
    /// <summary>Builds an asset reference from the path native holds.</summary>
    public static T Read<T>(string Path) where T : struct, IAssetRef
    {
        T Value = default;
        Value.SetFromPath(Path ?? "");
        return Value;
    }

    /// <summary>The path to store natively for an asset reference.</summary>
    public static string Write<T>(T Value) where T : struct, IAssetRef
    {
        return Value.GetPath() ?? "";
    }
}
