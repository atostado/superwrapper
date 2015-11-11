/* Copyright (C) 2003 Louis Thomas. License: http://www.latenighthacking.com/projects/lnhfslicense.html */

#include "stdafx.h"
#include <stdio.h>
#include <malloc.h>
#include <aclapi.h>
//#include <winerror.h>
#include "console.h"
#include "utils.h"

//####################################################################

typedef unsigned int RETVAL;

#define STRINGIFY(A) #A

#define EXIT_OK 0

#define _TeardownLastError(rv, errorsource) \
    { \
        RETVAL rv2__=GetLastError(); \
        CON().PrintLn(_F_(errorsource " failed with 0x%08X.\n", rv2__)); \
        if (EXIT_OK==rv) { \
            rv=rv2__; \
        } \
    }

#define _TeardownIfError(rv, rv2, errorsource) \
    if (EXIT_OK!=rv2) { \
        CON().PrintLn(_F_(errorsource " failed with 0x%08X.\n", rv2)); \
        if (EXIT_OK==rv) { \
            rv=rv2; \
        } \
    }

#define _JumpLastError(rv, label, errorsource) \
    rv=GetLastError(); \
    CON().PrintLn(_F_(errorsource " failed with 0x%08X.\n", rv)); \
    goto label;

#define _JumpLastErrorStr(rv, label, errorsource, str) \
    rv=GetLastError(); \
    CON().PrintLn(_F_( errorsource "(%s) failed with 0x%08X.\n", str, rv)); \
    goto label;

#define _JumpIfError(rv, label, errorsource) \
    if (EXIT_OK!=rv) {\
        CON().PrintLn(_F_( errorsource " failed with 0x%08X.\n", rv)); \
        goto label; \
    }

#define _JumpIfErrorStr(rv, label, errorsource, str) \
    if (EXIT_OK!=rv) {\
        CON().PrintLn(_F_( errorsource "(%s) failed with 0x%08X.\n", str, rv)); \
        goto label; \
    }

#define _JumpError(rv, label, errorsource) \
    CON().PrintLn(_F_( errorsource " failed with 0x%08X.\n", rv)); \
    goto label;

#define _JumpErrorStr(rv, label, errorsource, str) \
    CON().PrintLn(_F_( errorsource "(%s) failed with 0x%08X.\n", str, rv)); \
    goto label;

#define _JumpIfOutOfMemory(rv, label, pointer) \
    if (NULL==(pointer)) { \
        rv=ERROR_NOT_ENOUGH_MEMORY; \
        CON().PrintLn(_F_("Out of memory ('" #pointer "').\n")); \
        goto label; \
    }

#define _Verify(expression, rv, label) \
    if (!(expression)) { \
        CON().PrintLn(_F_("Verify failed: '%s' is false.\n", #expression)); \
        rv=E_UNEXPECTED; \
        goto label; \
    }


//####################################################################

//--------------------------------------------------------------------
void PrintError(DWORD dwError) {
	char * szErrorMessage=NULL;
	DWORD dwResult=FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM, 
		NULL/*ignored*/, dwError, 0/*language*/, (char *)&szErrorMessage, 0/*min-size*/, NULL/*valist*/);
	if (0==dwResult) {
		CON().PrintLn(_F_("(FormatMessage failed)"));
	} else {
		CON().PrintLn(_F_("%s", szErrorMessage));
	}
	if (NULL!=szErrorMessage) {
		LocalFree(szErrorMessage);
	}
}


//--------------------------------------------------------------------
RETVAL StartRemoteThread(HANDLE hRemoteProc, DWORD dwEntryPoint){
    RETVAL rv;

    // must be cleaned up
    HANDLE hRemoteThread=NULL;

    // inject the thread
    hRemoteThread=CreateRemoteThread(hRemoteProc, NULL, 0, (LPTHREAD_START_ROUTINE)dwEntryPoint, (void *)CTRL_C_EVENT, CREATE_SUSPENDED, NULL);
    if (NULL==hRemoteThread) {
        _JumpLastError(rv, error, "CreateRemoteThread");
    }

    // wake up the thread
    if (-1==ResumeThread(hRemoteThread)) {
        _JumpLastError(rv, error, "ResumeThread");
    }

    // wait for the thread to finish
    if (WAIT_OBJECT_0!=WaitForSingleObject(hRemoteThread, INFINITE)) {
        _JumpLastError(rv, error, "WaitForSingleObject");
    }

    // find out what happened
    if (!GetExitCodeThread(hRemoteThread, (DWORD *)&rv)) {
        _JumpLastError(rv, error, "GetExitCodeThread");
    }

    if (STATUS_CONTROL_C_EXIT==rv) {
        CON().PrintLn(_F_("Target process was killed.\n"));
        rv=EXIT_OK;
    } else if (EXIT_OK!=rv) {
        CON().PrintLn(_F_("(remote function) failed with 0x%08X.\n", rv));
        //if (ERROR_INVALID_HANDLE==rv) {
        //    CON().PrintLn(_F_("Are you sure this is a console application?\n");
        //}
    }


error:
    if (NULL!=hRemoteThread) {
        if (!CloseHandle(hRemoteThread)) {
            _TeardownLastError(rv, "CloseHandle");
        }
    }

    return rv;
}

//--------------------------------------------------------------------
void PrintHelp(void) {
    CON().PrintLn(_F_(
        "SendSignal <pid>\n"
        "  <pid> - send ctrl-break to process <pid> (hex ok)\n"
        ));
}

//--------------------------------------------------------------------
RETVAL SetPrivilege(HANDLE hToken, char * szPrivilege, bool bEnablePrivilege) {
    RETVAL rv;

    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!LookupPrivilegeValue(NULL, szPrivilege, &luid)) {
        _JumpLastError(rv, error, "LookupPrivilegeValue");
    }

    tp.PrivilegeCount=1;
    tp.Privileges[0].Luid=luid;
    if (bEnablePrivilege) {
        tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    } else {
        tp.Privileges[0].Attributes=0;
    }

    AdjustTokenPrivileges(hToken, false, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL); // may return true though it failed
    rv=GetLastError();
    _JumpIfError(rv, error, "AdjustTokenPrivileges");

    rv=EXIT_OK;
error:
    return rv;
}

//--------------------------------------------------------------------
RETVAL AdvancedOpenProcess(DWORD dwPid, HANDLE * phRemoteProc) {
    RETVAL rv, rv2;

    #define NEEDEDACCESS    PROCESS_QUERY_INFORMATION|PROCESS_VM_WRITE|PROCESS_VM_READ|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD

    // must be cleaned up
    HANDLE hThisProcToken=NULL;

    // initialize out params
    *phRemoteProc=NULL;
    bool bDebugPriv=false;

    // get a process handle with the needed access
    *phRemoteProc=OpenProcess(NEEDEDACCESS, false, dwPid);
    if (NULL==*phRemoteProc) {
        rv=GetLastError();
        if (ERROR_ACCESS_DENIED!=rv) {
            _JumpError(rv, error, "OpenProcess");
        }
        CON().PrintLn(_F_("Access denied; retrying with increased privileges.\n"));

        // give ourselves god-like access over process handles
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hThisProcToken)) {
            _JumpLastError(rv, error, "OpenProcessToken");
        }

        rv=SetPrivilege(hThisProcToken, SE_DEBUG_NAME, true);
        if (EXIT_OK==rv) {
            bDebugPriv=true;
        }
        _JumpIfErrorStr(rv, error, "SetPrivilege", SE_DEBUG_NAME);

        // get a process handle with the needed access
        *phRemoteProc=OpenProcess(NEEDEDACCESS, false, dwPid);
        if (NULL==*phRemoteProc) {
            _JumpLastError(rv, error, "OpenProcess");
        }
    }

    // success
    rv=EXIT_OK;

error:
    if (ERROR_ACCESS_DENIED==rv && false==bDebugPriv) {
        CON().PrintLn(_F_("You need administrative access (debug privilege) to access this process.\n"));
    }
    if (true==bDebugPriv) {
        rv2=SetPrivilege(hThisProcToken, SE_DEBUG_NAME, false);
        _TeardownIfError(rv, rv2, "SetPrivilege");
    }
    if (NULL!=hThisProcToken) {
        if (!CloseHandle(hThisProcToken)) {
            _TeardownLastError(rv, "CloseHandle");
        }
    }
    return rv;
}

static DWORD g_dwCtrlRoutineAddr=NULL;
static HANDLE g_hAddrFoundEvent=NULL;

//--------------------------------------------------------------------
BOOL WINAPI MyHandler(DWORD dwCtrlType) {
    // test
    //__asm { int 3 };
    if (CTRL_C_EVENT!=dwCtrlType) {
        return FALSE;
    }


    //CON().PrintLn(_F_("Received ctrl-break event\n");
    if (NULL==g_dwCtrlRoutineAddr) {

        // read the stack base address from the TEB
        #define TEB_OFFSET 4
        DWORD * pStackBase;
        __asm { mov eax, fs:[TEB_OFFSET] }
        __asm { mov pStackBase, eax }

        // read the parameter off the stack
        #define PARAM_0_OF_BASE_THEAD_START_OFFSET -3
        g_dwCtrlRoutineAddr=pStackBase[PARAM_0_OF_BASE_THEAD_START_OFFSET];

        // notify that we now have the address
        if (!SetEvent(g_hAddrFoundEvent)) {
            CON().PrintLn(_F_("SetEvent failed with 0x08X.\n", GetLastError()));
        }
    }
    return TRUE;
}


//--------------------------------------------------------------------
RETVAL GetCtrlRoutineAddress(void) {
    RETVAL rv=EXIT_OK;

    // must be cleaned up
    g_hAddrFoundEvent=NULL;

    // create an event so we know when the async callback has completed
    g_hAddrFoundEvent=CreateEvent(NULL, TRUE, FALSE, NULL); // no security, manual reset, initially unsignaled, no name
    if (NULL==g_hAddrFoundEvent) {
        _JumpLastError(rv, error, "CreateEvent");
    }

    // request that we be called on system signals
	if (!SetConsoleCtrlHandler(MyHandler, TRUE)) {
		_JumpLastError(rv, error, "SetConsoleCtrlHandler");
	}

    // generate a signal
	if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
		_JumpLastError(rv, error, "GenerateConsoleCtrlEvent");
	}

    // wait for our handler to be called
    {
        DWORD dwWaitResult=WaitForSingleObject(g_hAddrFoundEvent, INFINITE);
        if (WAIT_FAILED==dwWaitResult) {
            _JumpLastError(rv, error, "WaitForSingleObject");
        }
    }

    _Verify(NULL!=g_dwCtrlRoutineAddr, rv, error);

error:
    if (NULL!=g_hAddrFoundEvent) {
        if (!CloseHandle(g_hAddrFoundEvent)) {
            _TeardownLastError(rv, "CloseHandle");
        }
    }
    return rv;
}

//--------------------------------------------------------------------
int send_signal_main(unsigned int nArgs, char ** rgszArgs) {
    RETVAL rv;

    HANDLE hRemoteProc=NULL;
    HANDLE hRemoteProcToken=NULL;
    bool bSignalThisProcessGroup=false;

    //CON().PrintLn(_F_("test test test\n");

    if (2!=nArgs || (('/'==rgszArgs[1][0] || '-'==rgszArgs[1][0]) 
        && ('H'==rgszArgs[1][1] || 'h'==rgszArgs[1][1] || '?'==rgszArgs[1][1]))) 
    {
        PrintHelp();
        exit(1);
    }

    // check for the special parameter
    char * szPid=rgszArgs[1];
    bSignalThisProcessGroup=('-'==szPid[0]);
    char * szEnd;
    DWORD dwPid=strtoul(szPid, &szEnd, 0);
    if (false==bSignalThisProcessGroup && (szEnd==szPid || 0==dwPid)) {
        CON().PrintLn(_F_("\"%s\" is not a valid PID.\n", szPid));
        rv=ERROR_INVALID_PARAMETER;
        goto error;
    }


    //CON().PrintLn(_F_("Determining address of kernel32!CtrlRoutine...\n");
    rv=GetCtrlRoutineAddress();
    _JumpIfError(rv, error, "GetCtrlRoutineAddress");
    //CON().PrintLn(_F_("Address is 0x%08X.\n", g_dwCtrlRoutineAddr);

    // open the process
    if ('-'==rgszArgs[1][0]) {
        CON().PrintLn(_F_("Sending signal to self...\n"));
        hRemoteProc=GetCurrentProcess();
    } else {
        CON().PrintLn(_F_("Sending signal to process %d...\n", dwPid));
        rv=AdvancedOpenProcess(dwPid, &hRemoteProc);
        _JumpIfErrorStr(rv, error, "AdvancedOpenProcess", rgszArgs[1]);
    }

    rv=StartRemoteThread(hRemoteProc, g_dwCtrlRoutineAddr);
    _JumpIfError(rv, error, "StartRemoteThread");

//done:
    rv=EXIT_OK;
error:
    if (NULL!=hRemoteProc && GetCurrentProcess()!=hRemoteProc) {
        if (!CloseHandle(hRemoteProc)) {
            _TeardownLastError(rv, "CloseHandle");
        }
    }
    if (EXIT_OK!=rv) {
        CON().PrintLn(_F_("0x%08X == ", rv));
        PrintError(rv);
    }
    return rv;
}


void SendSignal(DWORD pid)
{
	char* ssz[2];
	ssz[0] = "";
	CString str;
	str.Format("%d", pid);
	ssz[1] = (char*)(LPCTSTR)str;
	itoa(pid, ssz[1], 10);
	send_signal_main(2, ssz); 
}