/* xinput1_3.dll for APRenewal: every export forwards to APRenewal's d3d9.dll (controller layer in pad.h).
   Replaces GOG's dixi xinput1_3.dll; the game imports XInputGetState / XInputSetState by ordinal. */
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { (void)h; (void)r; (void)p; return TRUE; }
