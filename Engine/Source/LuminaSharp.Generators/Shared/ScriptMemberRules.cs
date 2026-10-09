using System;

namespace LuminaSharp.ScriptProperties;

// The shape of a member as the rules need it, whatever is reading it, Roslyn symbols or System.Reflection.
internal enum EScriptMemberKind
{
    Field,

    // A C# property with a setter, or a get-only one exposing a native-owned view.
    WritableProperty,

    // A get-only property that is not a view, which has nothing native can store.
    ReadOnlyProperty,
}

// What one member becomes natively. Flag values are LuminaSharp.EPropertyFlags bits, kept numeric so this file needs nothing beyond the BCL.
internal readonly struct FScriptMemberRule
{
    public readonly bool bStored;
    public readonly bool bVisible;
    public readonly uint Flags;

    public FScriptMemberRule(bool bInStored, bool bInVisible, uint InFlags)
    {
        bStored = bInStored;
        bVisible = bInVisible;
        Flags = InFlags;
    }
}

// The one copy of which attributes give a script member native storage, read by the type library, the rewriter, the struct layout and the analyzer.
internal static class ScriptMemberRules
{
    public const string Property = "Property";
    public const string Serialize = "Serialize";
    public const string Sync = "Sync";
    public const string SaveGame = "SaveGame";
    public const string Bind = "Bind";
    public const string Hide = "Hide";

    private const uint NoSerializeFlag = 1u << 2;
    private const uint ReplicatedFlag = 1u << 12;
    private const uint SaveGameFlag = 1u << 19;

    // HasAttribute takes a bare name such as "SaveGame", without namespace or Attribute suffix.
    public static FScriptMemberRule Classify(Func<string, bool> HasAttribute, EScriptMemberKind Kind, bool bOwnerIsNativeObject)
    {
        if (Kind == EScriptMemberKind.ReadOnlyProperty || HasAttribute(Hide))
        {
            return default;
        }

        bool bProperty = HasAttribute(Property);
        bool bSync = HasAttribute(Sync);
        bool bSaveGame = HasAttribute(SaveGame);
        bool bSerialize = HasAttribute(Serialize);

        // Only an object has a native block to put a UI binding in, so a plain view model keeps its own.
        bool bBind = bOwnerIsNativeObject && HasAttribute(Bind);

        bool bPersisted = bProperty || bSync || bSaveGame || bSerialize;
        uint Flags = (bSync ? ReplicatedFlag : 0u) | (bSaveGame ? SaveGameFlag : 0u) | (bBind && !bPersisted ? NoSerializeFlag : 0u);
        return new FScriptMemberRule(bPersisted || bBind, bProperty, Flags);
    }

    // Strips the namespace and the Attribute suffix, so a Roslyn name and a reflected type name compare alike.
    public static string BareName(string AttributeName)
    {
        int Dot = AttributeName.LastIndexOf('.');
        string Name = Dot >= 0 ? AttributeName.Substring(Dot + 1) : AttributeName;
        return Name.EndsWith("Attribute", StringComparison.Ordinal) ? Name.Substring(0, Name.Length - "Attribute".Length) : Name;
    }
}
