using System;

namespace LuminaSharp;

// The C# spelling of REFLECT(HostOnly, ClientOnly, Cosmetic). Every one present must hold, so [ClientOnly, Cosmetic] means a client that draws.

// Runs only on the host, a dedicated server or a standalone game, the peers whose word is final.
[AttributeUsage(AttributeTargets.Class, Inherited = true, AllowMultiple = false)]
public sealed class HostOnlyAttribute : Attribute
{
}

// Runs only on a client joined to a host, never on the host or in a standalone game.
[AttributeUsage(AttributeTargets.Class, Inherited = true, AllowMultiple = false)]
public sealed class ClientOnlyAttribute : Attribute
{
}

// Runs only where something is drawn or played, never on a dedicated server, a bot or a headless process.
[AttributeUsage(AttributeTargets.Class, Inherited = true, AllowMultiple = false)]
public sealed class CosmeticAttribute : Attribute
{
}
