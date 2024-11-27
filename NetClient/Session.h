#pragma once

struct Session
{
	static constexpr LONG RELEASE_FLAG = 0x80000000;
	SOCKET sock_ = INVALID_SOCKET;
	ULONGLONG id_;
	ULONGLONG lastRecvTime;
	LONG lSendBufNum_;
	BOOL bDisconnectCalled_;
	MYOVERLAPPED connectOverlapped;
	MYOVERLAPPED sendOverlapped;
	MYOVERLAPPED recvOverlapped;
	MYOVERLAPPED disconnectOverlapped;
	LONG IoCnt_;
	CLockFreeQueue<Packet*> sendPacketQ_;
	BOOL bSendingInProgress_;
	Packet* pSendPacketArr_[50];
	RingBuffer recvRB_;
	BOOL Init(ULONGLONG ullClientID, SHORT shIdx)
	{
		bSendingInProgress_ = FALSE;
		InterlockedExchange(&id_, ((ullClientID << 16) ^ shIdx));
		lastRecvTime = GetTickCount64();
		bDisconnectCalled_ = FALSE;
		lSendBufNum_ = 0;
		recvRB_.ClearBuffer();
		return TRUE;
	}


	Session()
		:IoCnt_{ Session::RELEASE_FLAG | 0 }
	{}

	__forceinline static short GET_SESSION_INDEX(ULONGLONG id)
	{
		return id & 0xFFFF;
	}

};

