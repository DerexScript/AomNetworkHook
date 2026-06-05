#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wchar.h>
#include <wctype.h>

#include "MinHook.h"

typedef int (WSAAPI *WSAIoctl_t)(
    SOCKET s,
    DWORD dwIoControlCode,
    LPVOID lpvInBuffer,
    DWORD cbInBuffer,
    LPVOID lpvOutBuffer,
    DWORD cbOutBuffer,
    LPDWORD lpcbBytesReturned,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine
);

static WSAIoctl_t Real_WSAIoctl = nullptr;
static HINSTANCE g_instance = nullptr;
static DWORD g_adapterIp = 0;

static void TrimInPlaceW(wchar_t* text)
{
    if (!text)
        return;

    wchar_t* start = text;
    while (*start && iswspace(*start))
        start++;

    wchar_t* end = start + lstrlenW(start);
    while (end > start && iswspace(*(end - 1)))
        end--;

    *end = L'\0';

    if (start != text)
        MoveMemory(text, start, (lstrlenW(start) + 1) * sizeof(wchar_t));
}

static void StripQuotesInPlaceW(wchar_t* text)
{
    if (!text)
        return;

    TrimInPlaceW(text);

    int len = lstrlenW(text);
    if (len >= 2 && text[0] == L'"' && text[len - 1] == L'"')
    {
        MoveMemory(text, text + 1, (len - 2) * sizeof(wchar_t));
        text[len - 2] = L'\0';
    }
}

static bool ContainsNoCaseW(const wchar_t* text, const wchar_t* needle)
{
    if (!text || !needle || !needle[0])
        return false;

    for (; *text; text++)
    {
        const wchar_t* a = text;
        const wchar_t* b = needle;

        while (*a && *b && towlower(*a) == towlower(*b))
        {
            a++;
            b++;
        }

        if (!*b)
            return true;
    }

    return false;
}

static bool GetModuleDirectoryW(HINSTANCE module, wchar_t* out, DWORD outChars)
{
    if (!out || outChars == 0)
        return false;

    out[0] = L'\0';

    DWORD len = GetModuleFileNameW(module, out, outChars);
    if (len == 0 || len >= outChars)
        return false;

    for (wchar_t* p = out + lstrlenW(out); p > out; p--)
    {
        if (*(p - 1) == L'\\' || *(p - 1) == L'/')
        {
            *(p - 1) = L'\0';
            return true;
        }
    }

    lstrcpynW(out, L".", outChars);
    return true;
}

static bool BuildIniPathW(wchar_t* out, DWORD outChars)
{
    wchar_t dir[MAX_PATH] = {};

    if (!GetModuleDirectoryW(g_instance, dir, sizeof(dir) / sizeof(dir[0])))
        return false;

    int needed = lstrlenW(dir) + lstrlenW(L"\\AomNetworkPatch.ini") + 1;
    if (!out || needed > (int)outChars)
        return false;

    wsprintfW(out, L"%s\\AomNetworkPatch.ini", dir);
    return true;
}

static void ReadConfiguredAdapterName(wchar_t* adapterName, DWORD adapterNameChars)
{
    if (!adapterName || adapterNameChars == 0)
        return;

    adapterName[0] = L'\0';

    wchar_t iniPath[MAX_PATH] = {};
    if (BuildIniPathW(iniPath, sizeof(iniPath) / sizeof(iniPath[0])))
    {
        GetPrivateProfileStringW(
            L"AomNetworkPatch",
            L"Adapter",
            L"Hamachi",
            adapterName,
            adapterNameChars,
            iniPath
        );
    }

    StripQuotesInPlaceW(adapterName);

    if (adapterName[0] == L'\0')
        lstrcpynW(adapterName, L"Hamachi", adapterNameChars);
}

static bool IsUsableIpv4(DWORD ip)
{
    if (ip == 0 || ip == INADDR_NONE)
        return false;

    unsigned long host = ntohl(ip);
    return (host >> 24) != 127;
}

static bool AdapterMatches(const IP_ADAPTER_ADDRESSES* adapter, const wchar_t* wanted)
{
    if (!adapter || !wanted || !wanted[0])
        return false;

    if (ContainsNoCaseW(adapter->FriendlyName, wanted))
        return true;

    if (ContainsNoCaseW(adapter->Description, wanted))
        return true;

    wchar_t adapterName[512] = {};
    if (adapter->AdapterName)
    {
        MultiByteToWideChar(
            CP_ACP,
            0,
            adapter->AdapterName,
            -1,
            adapterName,
            sizeof(adapterName) / sizeof(adapterName[0])
        );
    }

    return ContainsNoCaseW(adapterName, wanted);
}

static bool ResolveConfiguredAdapterIp()
{
    wchar_t wanted[256] = {};
    ReadConfiguredAdapterName(wanted, sizeof(wanted) / sizeof(wanted[0]));

    ULONG size = 0;
    ULONG result = GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &size);
    if (result != ERROR_BUFFER_OVERFLOW || size == 0)
        return false;

    IP_ADAPTER_ADDRESSES* adapters = (IP_ADAPTER_ADDRESSES*)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        size
    );

    if (!adapters)
        return false;

    result = GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &size);
    if (result != NO_ERROR)
    {
        HeapFree(GetProcessHeap(), 0, adapters);
        return false;
    }

    bool found = false;

    for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter && !found; adapter = adapter->Next)
    {
        if (!AdapterMatches(adapter, wanted))
            continue;

        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
        {
            if (!unicast->Address.lpSockaddr ||
                unicast->Address.iSockaddrLength < (int)sizeof(sockaddr_in) ||
                unicast->Address.lpSockaddr->sa_family != AF_INET)
            {
                continue;
            }

            sockaddr_in* addr = (sockaddr_in*)unicast->Address.lpSockaddr;
            if (!IsUsableIpv4(addr->sin_addr.S_un.S_addr))
                continue;

            g_adapterIp = addr->sin_addr.S_un.S_addr;
            found = true;
            break;
        }
    }

    HeapFree(GetProcessHeap(), 0, adapters);
    return found;
}

static bool FillConfiguredRoutingInterface(LPVOID buffer, DWORD bufferBytes, LPDWORD bytesReturned)
{
    const DWORD required = sizeof(sockaddr_in);

    if (bytesReturned)
        *bytesReturned = required;

    if (!buffer || bufferBytes < required || g_adapterIp == 0)
        return false;

    sockaddr_in* addr = (sockaddr_in*)buffer;
    ZeroMemory(addr, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = 0;
    addr->sin_addr.S_un.S_addr = g_adapterIp;
    return true;
}

static int WSAAPI Hook_WSAIoctl(
    SOCKET s,
    DWORD dwIoControlCode,
    LPVOID lpvInBuffer,
    DWORD cbInBuffer,
    LPVOID lpvOutBuffer,
    DWORD cbOutBuffer,
    LPDWORD lpcbBytesReturned,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine
)
{
    int result = Real_WSAIoctl(
        s,
        dwIoControlCode,
        lpvInBuffer,
        cbInBuffer,
        lpvOutBuffer,
        cbOutBuffer,
        lpcbBytesReturned,
        lpOverlapped,
        lpCompletionRoutine
    );

    if (dwIoControlCode != SIO_ROUTING_INTERFACE_QUERY || g_adapterIp == 0)
        return result;

    if (FillConfiguredRoutingInterface(lpvOutBuffer, cbOutBuffer, lpcbBytesReturned))
        return 0;

    if (lpvOutBuffer && lpcbBytesReturned)
    {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    return result;
}

static void SignalReadyEvent()
{
    wchar_t eventName[128] = {};
    wsprintfW(eventName, L"AomNetworkHookReady_%lu", GetCurrentProcessId());

    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (event)
    {
        SetEvent(event);
        CloseHandle(event);
    }
}

static DWORD WINAPI InitHookThread(LPVOID)
{
    ResolveConfiguredAdapterIp();

    HMODULE ws2 = LoadLibraryW(L"ws2_32.dll");
    if (g_adapterIp != 0 && ws2 && MH_Initialize() == MH_OK)
    {
        if (MH_CreateHookApi(
            L"ws2_32.dll",
            "WSAIoctl",
            (LPVOID)&Hook_WSAIoctl,
            (LPVOID*)&Real_WSAIoctl
        ) == MH_OK)
        {
            MH_EnableHook(MH_ALL_HOOKS);
        }
    }

    SignalReadyEvent();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_instance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);

        HANDLE thread = CreateThread(nullptr, 0, InitHookThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }

    return TRUE;
}
