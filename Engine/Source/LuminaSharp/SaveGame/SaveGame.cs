using System;
using LuminaSharp;

namespace Lumina;

public partial class CSaveGame
{
    // Null before the first save.
    public DateTimeOffset? SavedAt => SavedUnixTime > 0 ? DateTimeOffset.FromUnixTimeSeconds(SavedUnixTime) : null;

    public bool Save(string Slot, int UserIndex = 0)
    {
        return SaveSystem.Save(this, Slot, UserIndex);
    }

    public void RequestSave(string Slot, int UserIndex = 0)
    {
        SaveSystem.RequestSave(this, Slot, UserIndex);
    }

    // The world of the script or system that is running.
    public bool CaptureWorld()
    {
        return CaptureWorld(Engine.World);
    }

    public bool RestoreWorld()
    {
        return RestoreWorld(Engine.World);
    }
}
