#pragma once
#include <winnt.h>
enum EVENT
{
	ON_RECV,
	DISCONNECT,
	RELEASE_SESSION,
	DISCONNECTEX_POST,
	DISCONNECT_PROC,
	CONNECT_PROC,
	SEND_PROC,
};
//#define MEMLOG

struct MemLog
{
	EVENT event;
	unsigned long long Cnt;
	DWORD threadId;
	unsigned long long sessionID;
};

constexpr int ARRAY_SIZE = 5000000;

int MemoryLog(EVENT event, ULONGLONG sessionID);
void MemLogWriteToFile(int lastIdx);
void MemLogRead(MemLog* memLogBuffer, unsigned long long* pOutCounter, int* pOutLastIdx);

#ifdef MEMLOG
#define MEMORY_LOG(event, sessionID) MemoryLog(event,sessionID)
#define MEMORY_LOG_WRITE_TO_FILE(lastIdx) MemLogWriteToFile(lastIdx)
#else
#define MEMORY_LOG(event, sessionID)
#define MEMORY_LOG_WRITE_TO_FILE(lastIdx)
#endif
