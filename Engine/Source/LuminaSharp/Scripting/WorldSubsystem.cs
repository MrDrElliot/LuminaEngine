using LuminaSharp;

namespace Lumina;

// A service owned by one world, and the place world scoped gameplay data belongs.
public partial class CWorldSubsystem
{
    // Valid from OnInitialize onwards, read from the native object that owns the value for both languages.
    protected CWorld World => GetWorld();
}
