#pragma once
#include <WinSock2.h>
#include <MSWSock.h>
#include <windows.h>
#include <mutex>

struct Session;

class SmartPacket;
class Packet;
struct Session;
#include "CLockFreeStack.h"

class NetClient
{
	NetClient();
	virtual ~NetClient();
	bool Start();
	void InitialConnect();
	void SendPacket(ULONGLONG id, SmartPacket& sendPacket);
	void SendPacket_ALREADY_ENCODED(ULONGLONG id, Packet* pPacket);
	virtual void OnRecv(ULONGLONG id, Packet* pPacket) = 0;
	virtual void OnError(ULONGLONG id, int errorType, Packet* pRcvdPacket) = 0;
	virtual void OnConnect(ULONGLONG id) = 0;
	virtual void OnRelease(ULONGLONG id) = 0;
	virtual void OnConnectFailed() = 0;
	void Disconnect(ULONGLONG id);
private:
	bool ConnectPost(Session* pSession);
	bool DisconnectExPost(Session* pSession);
	BOOL SendPost(Session* pSession);
	BOOL RecvPost(Session* pSession);
	void ReleaseSession(Session* pSession);
	void RecvProc(Session* pSession, int numberOfBytesTransferred);
	void SendProc(Session* pSession, DWORD dwNumberOfBytesTransferred);
	void ConnectProc(Session* pSession);
	void DisconnectProc(Session* pSession);
	friend class Packet;
	static unsigned __stdcall IOCPWorkerThread(LPVOID arg);

	DWORD IOCP_WORKER_THREAD_NUM_ = 0;
	DWORD IOCP_ACTIVE_THREAD_NUM_;
	LONG lSessionNum_;
	LONG maxSession_;
	ULONGLONG ullIdCounter_;
	Session* pSessionArr_;
	HANDLE* hIOCPWorkerThreadArr_;
	CLockFreeStack<short> DisconnectStack_;
	HANDLE hcp_;
	LPFN_CONNECTEX lpfnConnectExPtr_;
	LPFN_DISCONNECTEX lpfnDisconnectExPtr_;
	SOCKADDR_IN sockAddr_;
};

#include "Packet.h"
#include "CLockFreeQueue.h"
#include "RingBuffer.h"
#include "MYOVERLAPPED.h"
#include "Session.h"
