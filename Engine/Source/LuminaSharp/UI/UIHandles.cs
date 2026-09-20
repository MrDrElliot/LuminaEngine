namespace Lumina;

// Hand-written halves of the Reflector's UI handle mirrors, so validity reads off the type itself.

public partial struct FUIDocument
{
    public bool IsValid => Handle != 0;
}

public partial struct FUIElement
{
    public bool IsValid => Handle != 0;
}

public partial struct FUIEventListener
{
    public bool IsValid => Handle != 0;
}

public partial struct FUIDataModel
{
    public bool IsValid => Handle != 0;
}
