#include "SpriteSheetFactory.h"

namespace Lumina
{
    CObject* CSpriteSheetFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CSpriteSheet>(Package, Name);
    }
}
