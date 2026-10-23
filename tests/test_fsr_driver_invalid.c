// Valid PE fixture with no SDK lookup import; runtime initialization must always fail.
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
