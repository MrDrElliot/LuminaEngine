namespace LuminaSharp;

// Gameplay CPU profiler for the game thread, near free unless the editor Gameplay Profiler is recording.
public static class Profiler
{
    public static bool Enabled => Lumina.CGameplayProfilerLibrary.IsProfilerEnabled();

    // Prefer Sample, which closes the scope even when an exception is thrown.
    public static void Begin(string Name) => Lumina.CGameplayProfilerLibrary.BeginRegisteredScope(ScopeId(Name));

    // Names are registered natively once, after which a sample sends only an id. Concurrent, since parallel OnUpdate samples too.
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, int> ScopeIds = new();

    private static int ScopeId(string Name)
    {
        return ScopeIds.GetOrAdd(Name, static Key => Lumina.CGameplayProfilerLibrary.RegisterScope(Key));
    }

    public static void End() => Lumina.CGameplayProfilerLibrary.EndScope();

    public static Scope Sample(string Name) => new Scope(Name);

    public readonly ref struct Scope
    {
        // Decided once at the open, so the close always matches whatever the open did.
        private readonly bool bOpen;

        public Scope(string Name)
        {
            bOpen = Native.IsProfilingScopes;
            if (bOpen)
            {
                Lumina.CGameplayProfilerLibrary.BeginRegisteredScope(ScopeId(Name));
            }
        }

        public void Dispose()
        {
            if (bOpen)
            {
                Lumina.CGameplayProfilerLibrary.EndScope();
            }
        }
    }
}
