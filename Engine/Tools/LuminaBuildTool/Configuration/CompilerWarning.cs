namespace LuminaBuildTool.Configuration;

/// <summary>How a warning is reported. Default leaves the toolchain's own choice alone.</summary>
public enum WarningSeverity
{
    Default,
    Off,
    Warning,
    Fatal,
}

/// <summary>
/// Every warning the engine has an opinion about, named once so rules files never spell a raw
/// -Wname or C#### again. <see cref="CompilerWarningInfo"/> maps each one to its per-toolchain spelling.
/// </summary>
public enum CompilerWarning
{
    // Correctness. These describe code that is a bug, not code that is untidy.
    ReturnType,
    SequencePoint,
    StrictAliasing,
    ClassMemAccess,
    DeleteNonVirtualDtor,
    OverloadedVirtual,
    Reorder,
    Switch,
    NonNullCompare,
    DanglingPointer,
    DanglingReference,
    TautologicalCompare,
    Parentheses,
    Format,
    FormatTruncation,
    Uninitialized,
    MaybeUninitialized,
    SubobjectLinkage,

    // Lifetime and allocation: an object touched outside its lifetime, or freed through the wrong path.
    ReturnLocalAddr,
    UseAfterFree,
    MismatchedNewDelete,
    FreeNonheapObject,
    PlacementNew,

    // Memory intrinsics called with the wrong size, the wrong argument order, or past the end.
    SizeofPointerMemaccess,
    SizeofPointerDiv,
    SizeofArrayArgument,
    MemsetEltSize,
    MemsetTransposedArgs,
    ArrayBounds,
    StringopOverflow,
    StringopTruncation,
    Restrict,
    Nonnull,

    // Expressions that compile to something other than what they read as.
    Address,
    BoolOperation,
    LogicalNotParentheses,
    MisleadingIndentation,
    IntInBoolContext,
    EmptyBody,
    InitSelf,
    SelfMove,
    ImplicitFallthrough,
    DuplicatedCond,
    DuplicatedBranches,
    LogicalOp,

    // C++ inheritance and value semantics.
    NonVirtualDtor,
    SuggestOverride,
    CatchValue,
    PessimizingMove,
    RedundantMove,
    ExtraSemi,

    // Conversions and comparisons.
    SignCompare,
    ConversionLoss,
    ConversionSizeT,

    // Shadowing. GCC's -Wshadow is far broader than MSVC's three codes, so these stay MSVC-only
    // until the engine has been through a shadowing pass on GCC.
    Shadow,
    ShadowLocal,
    ShadowParameter,
    ShadowMember,
    NonstandardExtension,

    // Unused entities.
    UnusedVariable,
    UnusedFunction,
    UnusedButSetVariable,
    UnusedParameter,

    // Portability and layout.
    InvalidOffsetof,
    InterferenceSize,
    Attributes,
    PointerArith,
    Comment,

    // Deprecation.
    DeprecatedDeclarations,
    DeprecatedLiteralOperator,

    // Windows DLL boundary noise, meaningless elsewhere.
    DllInterface,
    DllInterfaceBase,

    // A definition whose declaration carried another module's import macro. Windows-only, and a bug.
    InconsistentDllLinkage,
}

/// <summary>One warning's spelling on each toolchain, plus whether promoting it also enables it.</summary>
public sealed class CompilerWarningInfo
{
    public required CompilerWarning Warning { get; init; }

    /// <summary>GCC and Clang spelling without the -W prefix, or null when neither has one.</summary>
    public string? GccName { get; init; }

    /// <summary>MSVC warning number without the C prefix, or null when MSVC has no equivalent.</summary>
    public string? MsvcCode { get; init; }

    /// <summary>
    /// True when the compiler leaves this warning off until it is asked for. Promoting one of these
    /// to Fatal does not merely change severity, it turns the warning on, so a codebase that has
    /// never seen it can acquire a wall of new errors at once.
    /// </summary>
    public bool bOffByDefault { get; init; }
}

/// <summary>The warning table. Every <see cref="CompilerWarning"/> has exactly one entry.</summary>
public static class CompilerWarnings
{
    private static readonly Dictionary<CompilerWarning, CompilerWarningInfo> Table = Build();

    public static CompilerWarningInfo Get(CompilerWarning Warning) => Table[Warning];

    public static IEnumerable<CompilerWarningInfo> All => Table.Values;

    private static Dictionary<CompilerWarning, CompilerWarningInfo> Build()
    {
        CompilerWarningInfo[] Entries =
        {
            new() { Warning = CompilerWarning.ReturnType,            GccName = "return-type",             MsvcCode = "4715" },
            new() { Warning = CompilerWarning.SequencePoint,         GccName = "sequence-point" },
            new() { Warning = CompilerWarning.StrictAliasing,        GccName = "strict-aliasing" },
            new() { Warning = CompilerWarning.ClassMemAccess,        GccName = "class-memaccess" },
            new() { Warning = CompilerWarning.DeleteNonVirtualDtor,  GccName = "delete-non-virtual-dtor" },
            new() { Warning = CompilerWarning.OverloadedVirtual,     GccName = "overloaded-virtual",      MsvcCode = "4263", bOffByDefault = true },
            new() { Warning = CompilerWarning.Reorder,               GccName = "reorder",                 MsvcCode = "5038", bOffByDefault = true },
            new() { Warning = CompilerWarning.Switch,                GccName = "switch",                  MsvcCode = "4062", bOffByDefault = true },
            new() { Warning = CompilerWarning.NonNullCompare,        GccName = "nonnull-compare" },
            new() { Warning = CompilerWarning.DanglingPointer,       GccName = "dangling-pointer" },
            new() { Warning = CompilerWarning.TautologicalCompare,   GccName = "tautological-compare" },
            new() { Warning = CompilerWarning.Parentheses,           GccName = "parentheses" },
            new() { Warning = CompilerWarning.Format,                GccName = "format",                  MsvcCode = "4477" },
            new() { Warning = CompilerWarning.FormatTruncation,      GccName = "format-truncation" },
            new() { Warning = CompilerWarning.Uninitialized,         GccName = "uninitialized",           MsvcCode = "4700" },
            new() { Warning = CompilerWarning.MaybeUninitialized,    GccName = "maybe-uninitialized",     MsvcCode = "4701", bOffByDefault = true },
            new() { Warning = CompilerWarning.SubobjectLinkage,      GccName = "subobject-linkage" },
            new() { Warning = CompilerWarning.DanglingReference,     GccName = "dangling-reference" },

            new() { Warning = CompilerWarning.ReturnLocalAddr,       GccName = "return-local-addr",       MsvcCode = "4172" },
            new() { Warning = CompilerWarning.UseAfterFree,          GccName = "use-after-free" },
            new() { Warning = CompilerWarning.MismatchedNewDelete,   GccName = "mismatched-new-delete" },
            new() { Warning = CompilerWarning.FreeNonheapObject,     GccName = "free-nonheap-object" },
            new() { Warning = CompilerWarning.PlacementNew,          GccName = "placement-new" },

            new() { Warning = CompilerWarning.SizeofPointerMemaccess, GccName = "sizeof-pointer-memaccess" },
            new() { Warning = CompilerWarning.SizeofPointerDiv,      GccName = "sizeof-pointer-div" },
            new() { Warning = CompilerWarning.SizeofArrayArgument,   GccName = "sizeof-array-argument" },
            new() { Warning = CompilerWarning.MemsetEltSize,         GccName = "memset-elt-size" },
            new() { Warning = CompilerWarning.MemsetTransposedArgs,  GccName = "memset-transposed-args" },
            new() { Warning = CompilerWarning.ArrayBounds,           GccName = "array-bounds" },
            new() { Warning = CompilerWarning.StringopOverflow,      GccName = "stringop-overflow" },
            new() { Warning = CompilerWarning.StringopTruncation,    GccName = "stringop-truncation" },
            new() { Warning = CompilerWarning.Restrict,              GccName = "restrict" },
            new() { Warning = CompilerWarning.Nonnull,               GccName = "nonnull" },

            new() { Warning = CompilerWarning.Address,               GccName = "address" },
            new() { Warning = CompilerWarning.BoolOperation,         GccName = "bool-operation" },
            new() { Warning = CompilerWarning.LogicalNotParentheses, GccName = "logical-not-parentheses" },
            new() { Warning = CompilerWarning.MisleadingIndentation, GccName = "misleading-indentation" },
            new() { Warning = CompilerWarning.IntInBoolContext,      GccName = "int-in-bool-context" },
            new() { Warning = CompilerWarning.EmptyBody,             GccName = "empty-body",              bOffByDefault = true },
            new() { Warning = CompilerWarning.InitSelf,              GccName = "init-self" },
            new() { Warning = CompilerWarning.SelfMove,              GccName = "self-move" },
            new() { Warning = CompilerWarning.ImplicitFallthrough,   GccName = "implicit-fallthrough",    bOffByDefault = true },
            new() { Warning = CompilerWarning.DuplicatedCond,        GccName = "duplicated-cond",         bOffByDefault = true },
            new() { Warning = CompilerWarning.DuplicatedBranches,    GccName = "duplicated-branches",     bOffByDefault = true },
            new() { Warning = CompilerWarning.LogicalOp,             GccName = "logical-op",              bOffByDefault = true },

            new() { Warning = CompilerWarning.NonVirtualDtor,        GccName = "non-virtual-dtor",        bOffByDefault = true },
            new() { Warning = CompilerWarning.SuggestOverride,       GccName = "suggest-override",        bOffByDefault = true },
            new() { Warning = CompilerWarning.CatchValue,            GccName = "catch-value" },
            new() { Warning = CompilerWarning.PessimizingMove,       GccName = "pessimizing-move" },
            new() { Warning = CompilerWarning.RedundantMove,         GccName = "redundant-move" },
            new() { Warning = CompilerWarning.ExtraSemi,             GccName = "extra-semi",              bOffByDefault = true },

            new() { Warning = CompilerWarning.SignCompare,           GccName = "sign-compare",            MsvcCode = "4018" },
            new() { Warning = CompilerWarning.ConversionLoss,        GccName = "conversion",              MsvcCode = "4244", bOffByDefault = true },
            new() { Warning = CompilerWarning.ConversionSizeT,       MsvcCode = "4267" },

            new() { Warning = CompilerWarning.Shadow,                GccName = "shadow",                  bOffByDefault = true },
            new() { Warning = CompilerWarning.ShadowLocal,           MsvcCode = "4456" },
            new() { Warning = CompilerWarning.ShadowParameter,       MsvcCode = "4457" },
            new() { Warning = CompilerWarning.ShadowMember,          MsvcCode = "4458" },
            new() { Warning = CompilerWarning.NonstandardExtension,  MsvcCode = "4238" },

            new() { Warning = CompilerWarning.UnusedVariable,        GccName = "unused-variable",         MsvcCode = "4101" },
            new() { Warning = CompilerWarning.UnusedFunction,        GccName = "unused-function",         MsvcCode = "4505" },
            new() { Warning = CompilerWarning.UnusedButSetVariable,  GccName = "unused-but-set-variable", MsvcCode = "4189" },
            new() { Warning = CompilerWarning.UnusedParameter,       GccName = "unused-parameter",        MsvcCode = "4100", bOffByDefault = true },

            new() { Warning = CompilerWarning.InvalidOffsetof,       GccName = "invalid-offsetof" },
            new() { Warning = CompilerWarning.InterferenceSize,      GccName = "interference-size" },
            new() { Warning = CompilerWarning.Attributes,            GccName = "attributes" },
            new() { Warning = CompilerWarning.PointerArith,          GccName = "pointer-arith" },
            new() { Warning = CompilerWarning.Comment,               GccName = "comment",                 MsvcCode = "4138" },

            new() { Warning = CompilerWarning.DeprecatedDeclarations,    GccName = "deprecated-declarations", MsvcCode = "4996" },
            new() { Warning = CompilerWarning.DeprecatedLiteralOperator, GccName = "deprecated-literal-operator" },

            new() { Warning = CompilerWarning.DllInterface,          MsvcCode = "4251" },
            new() { Warning = CompilerWarning.DllInterfaceBase,      MsvcCode = "4275" },
            new() { Warning = CompilerWarning.InconsistentDllLinkage, MsvcCode = "4273" },
        };

        return Entries.ToDictionary(Entry => Entry.Warning);
    }
}

/// <summary>
/// A warning-name to level map. Rules files write <c>Warnings[CompilerWarning.Switch] = WarningSeverity.Fatal</c>
/// and each toolchain turns that into whichever flag it understands.
/// </summary>
public sealed class WarningSettings
{
    private readonly Dictionary<CompilerWarning, WarningSeverity> Levels = new();

    public WarningSeverity this[CompilerWarning Warning]
    {
        get => Levels.TryGetValue(Warning, out WarningSeverity Level) ? Level : WarningSeverity.Default;
        set => Levels[Warning] = value;
    }

    /// <summary>Applies one level to several warnings at once.</summary>
    public void Set(WarningSeverity Level, params CompilerWarning[] Warnings)
    {
        foreach (CompilerWarning Warning in Warnings)
        {
            Levels[Warning] = Level;
        }
    }

    /// <summary>Every explicitly set entry, ordered so it can be hashed reproducibly.</summary>
    public IEnumerable<KeyValuePair<CompilerWarning, WarningSeverity>> Entries =>
        Levels.OrderBy(Pair => Pair.Key);

    /// <summary>Layers another map over this one; the argument wins where both name a warning.</summary>
    public void Apply(WarningSettings Other)
    {
        foreach (KeyValuePair<CompilerWarning, WarningSeverity> Pair in Other.Levels)
        {
            Levels[Pair.Key] = Pair.Value;
        }
    }
}
