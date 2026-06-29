namespace LuminaSharp;

/// <summary>This world's network role. Mirrors the native Lumina::ENetMode.</summary>
public enum ENetMode
{
    Standalone = 0,
    Client = 1,
    ListenServer = 2,
    DedicatedServer = 3,
}

/// <summary>
/// A world's networking interface (<c>World.Net</c>). The role/mode queries; the convenience booleans are
/// derived from <see cref="Mode"/>. Game thread only.
/// Forwards to flat <c>LuminaSharp_Net_*</c> shims in the Runtime module (DotNetGameplay.cpp).
/// </summary>
public readonly unsafe partial struct Net
{
    internal readonly ulong Handle;

    internal Net(ulong Handle)
    {
        this.Handle = Handle;
    }

    /// <summary>This world's network role.</summary>
    public ENetMode Mode => GetMode();

    /// <summary>True on the authority (a listen or dedicated server).</summary>
    public bool IsServer => Mode is ENetMode.ListenServer or ENetMode.DedicatedServer;

    /// <summary>True on a connected client.</summary>
    public bool IsClient => Mode == ENetMode.Client;

    /// <summary>True when the world isn't networked.</summary>
    public bool IsStandalone => Mode == ENetMode.Standalone;

    /// <summary>True when running as a client or server.</summary>
    public bool IsNetworked => Mode != ENetMode.Standalone;

    /// <summary>Server-side count of currently connected clients; 0 on clients and standalone.</summary>
    public int ConnectedClients => GetConnectedClients();

    // ---- native bindings ----

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Net_GetMode")]
    private partial ENetMode GetMode();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Net_GetConnectedClients")]
    private partial int GetConnectedClients();
}
