using System;

namespace LuminaSharp;

// The old name of Engine, kept for one version so game scripts can move over.
[Obsolete("Renamed to Engine.")]
public static class Game
{
    public static Lumina.CWorld World => Engine.World;

    public static bool InWorld => Engine.InWorld;

    public static void OpenLevel(string Url) => Engine.OpenLevel(Url);

    public static void Quit() => Engine.Quit();

    public static Lumina.CGameInstance? Instance => Engine.Instance;

    public static T? GetInstance<T>() where T : Lumina.CGameInstance => Engine.GetInstance<T>();
}
