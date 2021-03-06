#include "stdafx.h"
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <iostream>
#include <commctrl.h>
#include <strsafe.h>
#include <objbase.h>

#include <functional>
#include <vector>
#include <codecvt>
#include <string>
#include <ctime>

#include "pluginapi.h"
#include "blockingconcurrentqueue.h"

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

INT_PTR NSISCALL popintptr()
{
	INT_PTR result = 0;

	const TCHAR* str = popstring();

	if (str) {
		result = _tstoi(str);
		
		GlobalFree((HGLOBAL)str);
	}
	else {
		result = 0;
	}

	return result;
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

// convert UTF-8 string to wstring
std::wstring utf8_to_wstring(const std::string& str)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
	return myconv.from_bytes(str);
}

// convert wstring to UTF-8 string
std::string wstring_to_utf8(const std::wstring& str)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
	return myconv.to_bytes(str);
}

std::string tchar_to_utf8(const TCHAR* str)
{
#ifdef UNICODE
	return wstring_to_utf8(std::wstring(str));
#else
	return std::string(str);
#endif
}

#ifdef UNICODE
typedef std::wstring tstring;
#else
typedef std::string tstring;
#endif

tstring utf8_to_tstring(const std::string& str)
{
#ifdef UNICODE
	return utf8_to_wstring(str);
#else
	return str;
#endif
}

tstring wstring_to_tstring(const std::wstring& str)
{
#ifdef UNICODE
	return str;
#else
	return wstring_to_utf8(str);
#endif
}

///////////////////////////////////////////////////////////////////////
// Queue
///////////////////////////////////////////////////////////////////////

typedef std::function<void()> taskQueueItem_t;
static moodycamel::BlockingConcurrentQueue<taskQueueItem_t> g_TaskQueue;
static volatile bool g_taskConsumersKeepRunning = true;
static volatile bool g_taskConsumersGracefulShutdown = true;
static std::vector<std::thread> g_taskConsumers;

static void TaskConsumersShutdown(bool graceful)
{
	g_taskConsumersGracefulShutdown = graceful;
	g_taskConsumersKeepRunning = false;

	for (int i = 0; i < g_taskConsumers.size(); ++i) {
		if (g_taskConsumers[i].joinable()) {
			g_taskConsumers[i].join();
		}
	}

	g_taskConsumers.clear();
}

static void TaskConsumersInit(size_t numWorkers)
{
	TaskConsumersShutdown(false);

	g_taskConsumersKeepRunning = true;
	g_taskConsumersGracefulShutdown = true;

	for (int i = 0; i < numWorkers; ++i) {
		g_taskConsumers.emplace_back(std::thread([]() {
			taskQueueItem_t task;

			while (g_taskConsumersKeepRunning || (g_TaskQueue.size_approx() && g_taskConsumersGracefulShutdown)) {
				if (g_TaskQueue.wait_dequeue_timed(task, std::chrono::milliseconds(100))) {
					task();
				}
			}
		}));
	}
}

static void EnqueueTask(taskQueueItem_t task)
{
	g_TaskQueue.enqueue(task);
}

///////////////////////////////////////////////////////////////////////
// HTTP
///////////////////////////////////////////////////////////////////////

static const TCHAR* DEFAULT_USER_AGENT = TEXT("NSIS HTTP Client");

unsigned long g_configConnectTimeoutMilliseconds = 5000;
unsigned long g_configRequestTimeoutMilliseconds = 5000;
unsigned long g_configResponseTimeoutMilliseconds = 5000;
tstring g_userAgent = DEFAULT_USER_AGENT;

typedef std::function<bool(const TCHAR* headers, const void* buffer, size_t buffer_len)> http_response_callback_t;

static BOOL HttpRequest(
	const TCHAR* verb,
	const TCHAR* url,
	const TCHAR* user_agent = NULL,
	const TCHAR* request_headers_crlf = NULL,
	const size_t request_headers_crlf_chars_len = 0,
	const void* request_content = NULL,
	const size_t request_content_bytes_len = 0,
	http_response_callback_t responseCallback = NULL)
{
	HINTERNET hInternet = NULL;
	HINTERNET hConnect = NULL;
	HINTERNET hRequest = NULL;
	BOOL bResult = FALSE;
	
	unsigned long g_configMaxConnectionsPerServer = g_taskConsumers.size() ? g_taskConsumers.size() : 4;
	BOOL g_configHttpDecoding = TRUE;

	URL_COMPONENTS urlComponents;
	memset(&urlComponents, 0, sizeof(urlComponents));
	urlComponents.dwStructSize = sizeof(urlComponents);
	urlComponents.dwSchemeLength = 64;
	urlComponents.dwHostNameLength = 1024;
	urlComponents.dwUserNameLength = 1024;
	urlComponents.dwPasswordLength = 1024;
	urlComponents.dwUrlPathLength = 32768;

	urlComponents.lpszScheme = new TCHAR[urlComponents.dwSchemeLength];
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

	InternetSetOption(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &g_configConnectTimeoutMilliseconds, sizeof(g_configConnectTimeoutMilliseconds));

	InternetSetOption(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &g_configRequestTimeoutMilliseconds, sizeof(g_configRequestTimeoutMilliseconds));
	InternetSetOption(hInternet, INTERNET_OPTION_CONTROL_SEND_TIMEOUT, &g_configRequestTimeoutMilliseconds, sizeof(g_configRequestTimeoutMilliseconds));

	InternetSetOption(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &g_configResponseTimeoutMilliseconds, sizeof(g_configResponseTimeoutMilliseconds));
	InternetSetOption(hInternet, INTERNET_OPTION_CONTROL_RECEIVE_TIMEOUT, &g_configResponseTimeoutMilliseconds, sizeof(g_configResponseTimeoutMilliseconds));
		
	InternetSetOption(hInternet, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &g_configMaxConnectionsPerServer, sizeof(g_configMaxConnectionsPerServer));
	InternetSetOption(hInternet, INTERNET_OPTION_IGNORE_OFFLINE, NULL, 0);

	// https://docs.microsoft.com/en-us/windows/desktop/WinInet/content-encoding
	InternetSetOption(hInternet, INTERNET_OPTION_HTTP_DECODING, &g_configHttpDecoding, sizeof(g_configHttpDecoding));

	hConnect = InternetConnect(
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
			INTERNET_FLAG_NO_CACHE_WRITE |
			INTERNET_FLAG_NO_COOKIES |
			INTERNET_FLAG_NO_UI |
			INTERNET_FLAG_PRAGMA_NOCACHE |
			INTERNET_FLAG_RELOAD | (urlComponents.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_FLAG_SECURE : 0L),
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
			if (!responseCallback(headers, buffer, bytesRead)) {
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
	delete[] urlComponents.lpszScheme;
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

NSISFUNC(HttpPostString)
{
	EXDLL_INIT();

	auto url = popstring();
	auto contentType = popstring();
	auto postContent = popstring();

	EnqueueTask([=]() {
		if (url && contentType && postContent) {
			tstring request_headers = TEXT("Content-Type: "); request_headers  += contentType;
			std::string request_content = tchar_to_utf8(postContent);

			HttpRequest(
				TEXT("POST"),
				url,
				NULL, // user-agent
				request_headers.c_str(),
				request_headers.size(),
				request_content.c_str(),
				request_content.size(),
				NULL);
		}

		if (url) {
			GlobalFree((HGLOBAL)url);
		}
		if (contentType) {
			GlobalFree((HGLOBAL)contentType);
		}
		if (postContent) {
			GlobalFree((HGLOBAL)postContent);
		}
	});

	if (url && contentType && postContent) {
		pushstring(TEXT("ok"));
	}
	else {
		pushstring(TEXT("error"));
	}
}

NSISFUNC(HttpPostStringWait)
{
	EXDLL_INIT();

	auto url = popstring();
	auto contentType = popstring();
	auto postContent = popstring();

	tstring result;

	if (url && contentType && postContent) {
		tstring request_headers = TEXT("Content-Type: "); request_headers += contentType;
		std::string request_content = tchar_to_utf8(postContent);
		std::string response_buffer;

		BOOL bResult = HttpRequest(
			TEXT("POST"),
			url,
			NULL, // user-agent
			request_headers.c_str(),
			request_headers.size(),
			request_content.c_str(),
			request_content.size(),
			[&](const TCHAR* headers, const void* buffer, const size_t buffer_len) -> bool {
				response_buffer.append((char*)buffer, buffer_len);

				return true;
			});

		if (!bResult) {
			result = TEXT("error");
		}
		else {
			result = utf8_to_tstring(response_buffer);
		}
	}

	if (url) {
		GlobalFree((HGLOBAL)url);
	}
	if (contentType) {
		GlobalFree((HGLOBAL)contentType);
	}
	if (postContent) {
		GlobalFree((HGLOBAL)postContent);
	}

	pushstring(result.c_str());
}

NSISFUNC(HttpGetStringWait)
{
	EXDLL_INIT();

	auto url = popstring();
	auto contentType = popstring();
	auto postContent = popstring();

	tstring result;

	if (url && contentType && postContent) {
		std::string response_buffer;

		BOOL bResult = HttpRequest(
			TEXT("GET"),
			url,
			NULL, // user-agent
			NULL, // request_headers.c_str(),
			0, // request_headers.size(),
			NULL, // request_content.c_str(),
			0, // request_content.size(),
			[&](const TCHAR* headers, const void* buffer, const size_t buffer_len) -> bool {
				response_buffer.append((char*)buffer, buffer_len);

				return true;
			});

		if (!bResult) {
			result = TEXT("error");
		}
		else {
			result = utf8_to_tstring(response_buffer);
		}
	}

	if (url) {
		GlobalFree((HGLOBAL)url);
	}
	if (contentType) {
		GlobalFree((HGLOBAL)contentType);
	}
	if (postContent) {
		GlobalFree((HGLOBAL)postContent);
	}

	pushstring(result.c_str());
}

NSISFUNC(HttpSetTimeouts)
{
	EXDLL_INIT();
	
	g_configConnectTimeoutMilliseconds = ((unsigned long)popint()) * 1000L;
	g_configRequestTimeoutMilliseconds = ((unsigned long)popint()) * 1000L;
	g_configResponseTimeoutMilliseconds = ((unsigned long)popint()) * 1000L;
}

NSISFUNC(HttpSetAsyncRequestsConcurrency)
{
	EXDLL_INIT();

	int numWorkers = popint();

	if (numWorkers > 0) {
		TaskConsumersInit(numWorkers);
	}
}

NSISFUNC(HttpFlushAllAsyncRequests)
{
	EXDLL_INIT();

	int numWorkers = g_taskConsumers.size();

	TaskConsumersShutdown(true);

	TaskConsumersInit(numWorkers);
}

NSISFUNC(HttpSetUserAgent)
{
	const TCHAR* userAgent = popstring();

	if (userAgent) {
		g_userAgent = userAgent;

		GlobalFree((HGLOBAL)userAgent);
	}
}

NSISFUNC(GetSecondsSinceEpochStart)
{
	EXDLL_INIT();

	std::time_t time = std::time(nullptr);

#ifdef UNICODE
	pushstring(std::to_wstring(time).c_str());
#else
	pushstring(std::to_string(time).c_str());
#endif
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

	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		break;
	case DLL_PROCESS_DETACH:
		TaskConsumersShutdown(true);
		break;
	}

	return TRUE;
}


#ifdef TARGET_EXE
//
// This is used only in "EXE Debug" configuration
// for easy step-through debugging as an EXE.
//
int main(void)
{
	/*
	{
		std::string postData = "{ \"app_id\": \"3780144397\", \"identity\": \"1E1B8CEE-884E-4ED9-9359-DC5BA786836A\", \"event\": \"Test Server-Side API Event\", \"properties\": { } }";
		const TCHAR* url = TEXT("https://heapanalytics.com/api/track");
		const TCHAR* request_headers = "Content-Type: application/json";
		
		bool result = HttpRequest(
			TEXT("POST"),
			url,
			g_userAgent.c_str(), // user-agent
			request_headers,
			_tcslen(request_headers),
			postData.c_str(),
			postData.size(),
			[](const TCHAR* headers, const void* buffer, const size_t buffer_len) -> bool {
				std::cout << TEXT("Headers: ") << headers << std::endl;

				char* buf = (char*)buffer;
				buf[buffer_len] = 0;
				std::cout << "Buffer:" << std::endl << buf << std::endl;
				return true;
			});

		std::cout << result << std::endl;
	}

	TaskConsumersInit(4);

	for (size_t i = 0; i < 0; ++i) {
		EnqueueTask([=]() {
			const TCHAR* REQUEST_HEADERS = TEXT("Content-Type: text/json");

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
				[](const TCHAR* headers, const void* buffer, const size_t buffer_len) -> bool {
					std::cout << TEXT("Headers: ") << headers << std::endl;

					char* buf = (char*)buffer;
					buf[buffer_len] = 0;
					std::cout << "Buffer:" << std::endl << buf << std::endl;
					return true;
				});
		});
	}

	TaskConsumersShutdown(true);
	*/

	return 0;
}
#endif