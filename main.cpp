#include "stdafx.h"
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <iostream>
#include <commctrl.h>
#include <strsafe.h>

#include "pluginapi.h"

#include "AsyncTaskQueue.hpp"

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
// HTTP
///////////////////////////////////////////////////////////////////////

static const TCHAR* DEFAULT_USER_AGENT = TEXT("NSIS HTTP Client");

typedef bool(*http_response_callback_t)(void* data, const TCHAR* headers, const void* buffer, size_t buffer_len);

static BOOL HttpRequest(
	const TCHAR* verb,
	const TCHAR* url,
	const TCHAR* user_agent = NULL,
	const TCHAR* request_headers_crlf = NULL,
	const size_t request_headers_crlf_chars_len = 0,
	const void* request_content = NULL,
	const size_t request_content_bytes_len = 0,
	http_response_callback_t responseCallback = NULL,
	void* responseCallbackData = NULL)
{
	HINTERNET hInternet = NULL;
	HINTERNET hConnect = NULL;
	HINTERNET hRequest = NULL;
	BOOL bResult = FALSE;
	
	URL_COMPONENTS urlComponents;
	memset(&urlComponents, 0, sizeof(urlComponents));
	urlComponents.dwStructSize = sizeof(urlComponents);
	urlComponents.dwHostNameLength = 1024;
	urlComponents.dwUserNameLength = 1024;
	urlComponents.dwPasswordLength = 1024;
	urlComponents.dwUrlPathLength = 32768;

	urlComponents.lpszHostName = new TCHAR[urlComponents.dwHostNameLength];
	urlComponents.lpszUserName = new TCHAR[urlComponents.dwUserNameLength];
	urlComponents.lpszPassword = new TCHAR[urlComponents.dwPasswordLength];
	urlComponents.lpszUrlPath = new TCHAR[urlComponents.dwUrlPathLength];

	if (!InternetCrackUrl(url, _tcsclen(url), ICU_DECODE, &urlComponents)) {
		goto failed_parse_url;
	}

	hInternet =
		InternetOpen(
			user_agent ? user_agent : DEFAULT_USER_AGENT,
			INTERNET_OPEN_TYPE_PRECONFIG,
			NULL,
			NULL,
			0);

	if (!hInternet) {
		goto failed_internet_open;
	}

	hConnect =
		InternetConnect(
			hInternet,
			urlComponents.lpszHostName,
			urlComponents.nPort,
			urlComponents.dwUserNameLength ? urlComponents.lpszUserName : NULL,
			urlComponents.dwPasswordLength ? urlComponents.lpszPassword : NULL,
			INTERNET_SERVICE_HTTP,
			0,
			0);

	if (!hConnect) {
		goto failed_session_connect;
	}

	hRequest =
		HttpOpenRequest(
			hConnect,
			verb,
			urlComponents.lpszUrlPath,
			NULL,
			NULL,
			NULL,
			INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_RELOAD,
			NULL);

	bResult =
		HttpSendRequest(
			hRequest,
			request_headers_crlf,
			request_headers_crlf_chars_len,
			(LPVOID)request_content,
			request_content_bytes_len);

	if (!bResult) {
		goto http_request_failed;
	}

	if (responseCallback) {
		DWORD headersBufSize = 0L;
		HttpQueryInfo(hRequest, HTTP_QUERY_RAW_HEADERS_CRLF, NULL, &headersBufSize, NULL);
		headersBufSize += sizeof(TCHAR);
		TCHAR* headers = new TCHAR[headersBufSize / sizeof(TCHAR)];
		
		if (!HttpQueryInfo(hRequest, HTTP_QUERY_RAW_HEADERS_CRLF, headers, &headersBufSize, NULL)) {
			headers[0] = 0;
		}

		const size_t BUFFER_LEN = 32768;
		char* buffer = new char[BUFFER_LEN];
		
		DWORD bytesRead = 0;
		while (InternetReadFile(hRequest, buffer, BUFFER_LEN, &bytesRead)) {
			if (!responseCallback(responseCallbackData, headers, buffer, bytesRead)) {
				break;
			}

			if (bytesRead < BUFFER_LEN) {
				break;
			}
		}

		delete[] headers;
		delete[] buffer;
	}

http_request_failed:
	InternetCloseHandle(hRequest);

failed_open_request:
	InternetCloseHandle(hConnect);

failed_session_connect:
	InternetCloseHandle(hInternet);

failed_internet_open:
failed_parse_url:
	delete[] urlComponents.lpszHostName;
	delete[] urlComponents.lpszUserName;
	delete[] urlComponents.lpszPassword;
	delete[] urlComponents.lpszUrlPath;

	return bResult;
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

int main(void)
{
	const TCHAR* REQUEST_HEADERS = "Content-Type: text/json";

	std::string request_content;
	request_content = "{}";

	HttpRequest(
		TEXT("POST"),
		TEXT("http://localhost:3000"),
		NULL, // user-agent
		REQUEST_HEADERS,
		_tcslen(REQUEST_HEADERS),
		request_content.c_str(),
		request_content.size(),
		[](void* data, const TCHAR* headers, const void* buffer, const size_t buffer_len) -> bool {
			std::cout << TEXT("Headers: ") << headers << std::endl;

			char* buf = (char*)buffer;
			buf[buffer_len] = 0;
			std::cout << "Buffer:" << std::endl << buf << std::endl;
			return true;
		},
		NULL
	);

	return 0;
}