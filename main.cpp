#include "stdafx.h"
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <iostream>
#include <commctrl.h>
#include <strsafe.h>

#include "pluginapi.h"

// NSIS vars
unsigned int g_stringsize;
stack_t **g_stacktop;
TCHAR *g_variables;

// Used in DlgProc and RunModalDialog
HINSTANCE gDllInstance;

// NSIS stack

const TCHAR* const NSISCALL popstring()
{
	stack_t *th;
	if (!g_stacktop || !*g_stacktop) return nullptr;
	th = (*g_stacktop);

	size_t strLen = _tcsclen(th->text);
	TCHAR* buf = new TCHAR[strLen + 1];
	_tcsncpy_s(buf, strLen + 1, th->text, strLen);
	buf[strLen] = 0;

	*g_stacktop = th->next;
	GlobalFree((HGLOBAL)th);

	return buf;
}

int NSISCALL popstring(TCHAR *str)
{
	stack_t *th;
	if (!g_stacktop || !*g_stacktop) return 1;
	th = (*g_stacktop);
	if (str) _tcsncpy_s(str, g_stringsize, th->text, g_stringsize);
	*g_stacktop = th->next;
	GlobalFree((HGLOBAL)th);
	return 0;
}

int NSISCALL popstringn(TCHAR *str, int maxlen)
{
	stack_t *th;
	if (!g_stacktop || !*g_stacktop) return 1;
	th = (*g_stacktop);
	if (str) _tcsncpy_s(str, maxlen ? maxlen : g_stringsize, th->text, g_stringsize);
	*g_stacktop = th->next;
	GlobalFree((HGLOBAL)th);
	return 0;
}

void NSISCALL pushstring(const TCHAR *str)
{
	stack_t *th;
	if (!g_stacktop) return;
	th = (stack_t*)GlobalAlloc(GPTR, (sizeof(stack_t) + (g_stringsize) * sizeof(TCHAR)));
	_tcsncpy_s(th->text, g_stringsize, str, g_stringsize);
	th->text[g_stringsize - 1] = 0;
	th->next = *g_stacktop;
	*g_stacktop = th;
}

TCHAR* NSISCALL getuservariable(const int varnum)
{
	if (varnum < 0 || varnum >= __INST_LAST) return NULL;
	return g_variables + varnum * g_stringsize;
}

void NSISCALL setuservariable(const int varnum, const TCHAR *var)
{
	if (var != NULL && varnum >= 0 && varnum < __INST_LAST)
		_tcsncpy_s(g_variables + varnum * g_stringsize, g_stringsize, var, g_stringsize);
}

///////////////////////////////////////////////////////////////////////
// API
///////////////////////////////////////////////////////////////////////

#define NSISFUNC(name) extern "C" void __declspec(dllexport) name(HWND hWndParent, int string_size, TCHAR* variables, stack_t** stacktop, extra_parameters* extra)

NSISFUNC(Test)
{
	EXDLL_INIT();

	auto buf = popstring();

	if (buf) {
		delete[] buf;
	}

	pushstring(TEXT("error"));
	pushstring(TEXT("OK"));
}

BOOL WINAPI DllMain(
	_In_ HINSTANCE hinstDLL,
	_In_ DWORD     fdwReason,
	_In_ LPVOID    lpvReserved
)
{
	if (hinstDLL) {
		gDllInstance = hinstDLL;
	}

	return TRUE;
}