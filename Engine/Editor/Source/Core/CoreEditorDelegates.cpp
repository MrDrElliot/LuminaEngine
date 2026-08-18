#include "EditorPCH.h"
#include "CoreEditorDelegates.h"

namespace Lumina
{
    TMulticastDelegate<void, CObject*>                 FCoreEditorDelegates::OnAssetPreSave;
    TMulticastDelegate<void, CObject*>                 FCoreEditorDelegates::OnAssetSaved;
    TMulticastDelegate<void, CObject*>                 FCoreEditorDelegates::OnAssetCreated;
    TMulticastDelegate<void, FStringView>              FCoreEditorDelegates::OnAssetDeleted;
    TMulticastDelegate<void, FStringView, FStringView> FCoreEditorDelegates::OnAssetRenamed;
    TMulticastDelegate<void>                           FCoreEditorDelegates::OnProjectLoaded;
    TMulticastDelegate<void, CWorld*>                  FCoreEditorDelegates::OnPIEBegin;
    TMulticastDelegate<void, CWorld*>                  FCoreEditorDelegates::OnPIEEnd;
}
