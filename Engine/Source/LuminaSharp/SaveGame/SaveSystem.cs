using System;
using System.IO;
using System.Linq;
using Lumina;

namespace LuminaSharp;

// One file per slot, in the project's Saved/SaveGames while developing and the player's app data once shipped.
public static class SaveSystem
{
    public static T Create<T>() where T : CSaveGame
    {
        return ObjectFactory.New<T>() ?? throw new InvalidOperationException($"Could not create save game {typeof(T).Name}.");
    }

    // Replaces what the slot held, through a temporary file, so a crash mid-write leaves the old save intact.
    public static bool Save(CSaveGame Game, string Slot, int UserIndex = 0)
    {
        return CSaveGameLibrary.SaveGameToSlot(Game, Slot, UserIndex);
    }

    // Null when the slot is empty, unreadable, or holds a different type.
    public static T? Load<T>(string Slot, int UserIndex = 0) where T : CSaveGame
    {
        return CSaveGameLibrary.LoadGameFromSlot(Slot, UserIndex) as T;
    }

    // Captures the running world into Game and writes it at the end of the frame, safe from any system.
    public static void RequestSave(CSaveGame Game, string Slot, int UserIndex = 0)
    {
        CSaveGameLibrary.RequestSaveGame(Engine.World, Game, Slot, UserIndex);
    }

    // Loads the slot and restores the running world at the end of the frame. Systems hear it in OnWorldRestored.
    public static void RequestLoad(string Slot, int UserIndex = 0)
    {
        CSaveGameLibrary.RequestLoadGame(Engine.World, Slot, UserIndex);
    }

    public static T LoadOrCreate<T>(string Slot, int UserIndex = 0) where T : CSaveGame
    {
        return Load<T>(Slot, UserIndex) ?? Create<T>();
    }

    public static bool Exists(string Slot, int UserIndex = 0)
    {
        return CSaveGameLibrary.DoesSaveGameExist(Slot, UserIndex);
    }

    public static bool Delete(string Slot, int UserIndex = 0)
    {
        return CSaveGameLibrary.DeleteGameInSlot(Slot, UserIndex);
    }

    public static string GetDirectory(int UserIndex = 0)
    {
        return CSaveGameLibrary.GetSaveGameDirectory(UserIndex);
    }

    public static string GetSlotPath(string Slot, int UserIndex = 0)
    {
        return CSaveGameLibrary.GetSlotFilePath(Slot, UserIndex);
    }

    // Most recently saved first.
    public static string[] ListSlots(int UserIndex = 0)
    {
        string Folder = GetDirectory(UserIndex);
        if (!System.IO.Directory.Exists(Folder))
        {
            return Array.Empty<string>();
        }
        return new DirectoryInfo(Folder).GetFiles("*.sav")
            .OrderByDescending(File => File.LastWriteTimeUtc)
            .Select(File => Path.GetFileNameWithoutExtension(File.Name))
            .ToArray();
    }
}
