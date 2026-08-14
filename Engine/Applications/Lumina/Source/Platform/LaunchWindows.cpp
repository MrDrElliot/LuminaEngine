#ifdef _WIN32

#include <Windows.h>
#include <stdlib.h> // __argc / __argv (CRT-provided on Windows).

extern int LuminaMain(int ArgC, char** ArgV);

int WINAPI WinMain(_In_ HINSTANCE hInInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ char* pCmdLine, _In_ int nCmdShow)
{
    return LuminaMain(__argc, __argv);
}

#endif
