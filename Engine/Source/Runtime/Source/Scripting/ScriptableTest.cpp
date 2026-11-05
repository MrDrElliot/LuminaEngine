#include "RuntimePCH.h"
#include "ScriptableTest.h"

namespace Lumina
{
    bool CWorldSubsystemTest::bAllowCreation = false;
    bool CEntitySystemTest::bAllowCreation   = false;
    int32 CDestroyCountTest::DestroyCount    = 0;
    bool CSelfDefaultTest::bAskForOwnDefault = false;
}
