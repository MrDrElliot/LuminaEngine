namespace LuminaSharp;

/// <summary>
/// Lightweight gameplay CPU profiler. Wrap any gameplay work in a scope and it shows up,  aggregated by
/// name, with call count and inclusive/self ms,  in the editor's Gameplay Profiler tool. Near-zero cost
/// when nobody is recording (<see cref="Enabled"/> is false). Game thread only.
///
/// Entity script and system <c>OnUpdate</c> are auto-profiled by their type name, so per-script timings
/// appear with no work. Use <see cref="Sample"/> to break a hot method into sub-scopes:
/// <code>using (Profiler.Sample("Perception")) { ... }</code>
/// </summary>
public static class Profiler
{
    /// <summary>True while the editor Gameplay Profiler is open (or recording is otherwise enabled).</summary>
    public static bool Enabled => Lumina.CGameplayProfilerLibrary.IsProfilerEnabled();

    /// <summary>Open a named scope. Pair with <see cref="End"/>; prefer <see cref="Sample"/> for exception safety.</summary>
    public static void Begin(string Name) => Lumina.CGameplayProfilerLibrary.BeginRegisteredScope(ScopeId(Name));

    // Names are registered natively once, after which a sample sends only an id.
    private static readonly System.Collections.Generic.Dictionary<string, int> ScopeIds = new();

    private static int ScopeId(string Name)
    {
        if (!ScopeIds.TryGetValue(Name, out int Id))
        {
            Id = Lumina.CGameplayProfilerLibrary.RegisterScope(Name);
            ScopeIds[Name] = Id;
        }
        return Id;
    }

    /// <summary>Close the most recently opened scope.</summary>
    public static void End() => Lumina.CGameplayProfilerLibrary.EndScope();

    /// <summary>
    /// A <c>using</c>-scoped timer that closes when the block exits (including on exceptions):
    /// <c>using (Profiler.Sample("Pathfind")) { ... }</c>.
    /// </summary>
    public static Scope Sample(string Name) => new Scope(Name);

    /// <summary>The disposable returned by <see cref="Sample"/>. Stack-only (a ref struct); do not store it.</summary>
    public readonly ref struct Scope
    {
        public Scope(string Name) => Lumina.CGameplayProfilerLibrary.BeginRegisteredScope(ScopeId(Name));
        public void Dispose() => Lumina.CGameplayProfilerLibrary.EndScope();
    }
}
