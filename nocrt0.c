/*
 * Proj: nocrt0
 * Auth: matveyt
 * Desc: nostdlib entry point for console application (mainCRTStartup)
 * Note: Tested with GCC/MinGW-w64, Pelles C
 */


/** Build instructions:

    -D_UNICODE = compiles 'unicode' version instead of 'ansi'
    -DARGV={own | msvcrt | none} = selects underlying argv[] implementation:
        - own = built-in implementation (default)
        - msvcrt = import from msvcrt.dll
        - none = intended for 'int main(void)' only, UB otherwise

**/


#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stddef.h>

extern int wmain(int,wchar_t**);
extern void __set_app_type(int);
typedef struct { int newmode; } _startupinfo;
extern void __wgetmainargs(int*,wchar_t***,wchar_t***,int,_startupinfo*);

__declspec(noreturn)
void wmainCRTStartup(void)
{
    int argc;
    wchar_t** argv;
    wchar_t** envp;
    _startupinfo si = {0};
    //_CRT_INIT();
    __set_app_type(1); // _CONSOLE_APP
    __wgetmainargs(&argc, &argv, &envp, 0, &si); // On WinXP, ^ in arguments is lost if not quoted.
    ExitProcess(wmain(argc, argv));
}
