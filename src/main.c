/* Silicord - native Discord client for Windows. No C runtime: entry point is `entry`. */
#include <windows.h>
#include "console.h"

void entry(void)
{
    con_init();
    con_print("\r\n");
    con_banner();
    con_print("\r\n  silicord " SILICORD_VERSION " - not connected yet, see docs/ROADMAP.md\r\n\r\n");
    ExitProcess(0);
}
