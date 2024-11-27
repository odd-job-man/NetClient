#include <WS2tcpip.h>
#include <locale>

#include "NetClient.h"
#include "Logger.h"
#include "Parser.h"

#include "MemLog.h"

#pragma comment(lib,"LoggerMT.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib,"TextParser.lib")


bool WsaIoctlWrapper(SOCKET sock, GUID guid, LPVOID* pFuncPtr)
{
	DWORD bytes = 0;
	return SOCKET_ERROR != WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), pFuncPtr, sizeof(*pFuncPtr), &bytes, NULL, NULL);
}

void NetClient::SendProc(Session* pSession, DWORD dwNumberOfBytesTransferred)
{
	if (pSession->bDisconnectCalled_ == TRUE)
		return;

	MEMORY_LOG(SEND_PROC, pSession->id_);
	LONG sendBufNum = pSession->lSendBufNum_;
	pSession->lSendBufNum_ = 0;
	for (LONG i = 0; i < sendBufNum; ++i)
	{
		Packet* pPacket = pSession->pSendPacketArr_[i];
		if (pPacket->DecrementRefCnt() == 0)
		{
			PACKET_FREE(pPacket);
		}
	}
	InterlockedExchange((LONG*)&pSession->bSendingInProgress_, FALSE);
	if (pSession->sendPacketQ_.GetSize() > 0)
		SendPost(pSession);
}

void NetClient::ConnectProc(Session* pSession)
{
	MEMORY_LOG(CONNECT_PROC, pSession->id_);
	InterlockedIncrement(&pSession->IoCnt_);
	InterlockedAnd(&pSession->IoCnt_, ~Session::RELEASE_FLAG);

	setsockopt(pSession->sock_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
	//SetZeroCopy(pSession->sock_);
	//SetLinger(pSession->sock_);
	//SetReuseAddr(pSession->sock_);

	pSession->Init(InterlockedIncrement(&ullIdCounter_) - 1, (short)(pSession - pSessionArr_));

	OnConnect(pSession->id_);
	RecvPost(pSession);
}

void NetClient::DisconnectProc(Session* pSession)
{
	MEMORY_LOG(DISCONNECT_PROC, pSession->id_);
	// Release 될 Session의 직렬화 버퍼 정리
	for (LONG i = 0; i < pSession->lSendBufNum_; ++i)
	{
		Packet* pPacket = pSession->pSendPacketArr_[i];
		if (pPacket->DecrementRefCnt() == 0)
		{
			PACKET_FREE(pPacket);
		}
	}

	LONG size = pSession->sendPacketQ_.GetSize();
	for (LONG i = 0; i < size; ++i)
	{
		Packet* pPacket = pSession->sendPacketQ_.Dequeue().value();
		if (pPacket->DecrementRefCnt() == 0)
		{
			PACKET_FREE(pPacket);
		}
	}

	if (pSession->sendPacketQ_.GetSize() > 0)
		__debugbreak();

	OnRelease(pSession->id_);
	// 여기서부터 다시 Connect가 가능해지기 때문에 무조건 이 함수 최하단에 와야함
	DisconnectStack_.Push((short)(pSession - pSessionArr_));
}

unsigned __stdcall NetClient::IOCPWorkerThread(LPVOID arg)
{
	srand(time(nullptr));
	NetClient* pNetClient = (NetClient*)arg;
	while (1)
	{
		MYOVERLAPPED* pOverlapped = nullptr;
		DWORD dwNOBT = 0;
		Session* pSession = nullptr;
		bool bContinue = false;
		bool bConnectSuccess = true;
		BOOL bGQCSRet = GetQueuedCompletionStatus(pNetClient->hcp_, &dwNOBT, (PULONG_PTR)&pSession, (LPOVERLAPPED*)&pOverlapped, INFINITE);
		do
		{
			if (!pOverlapped && !dwNOBT && !pSession)
				return 0;

			if (!bGQCSRet && pOverlapped)
			{
				DWORD errCode = WSAGetLastError();
				if (pOverlapped->why == OVERLAPPED_REASON::CONNECT)
				{
					bContinue = true;
					pNetClient->OnConnectFailed(pSession->id_);
					pNetClient->DisconnectStack_.Push((short)(pSession - pNetClient->pSessionArr_));
					if (errCode != 0)
						LOG(L"ERROR", ERR, TEXTFILE, L"ConnectEx Failed ErrCode : %u", WSAGetLastError());
					continue;
				}
				else if (pOverlapped->why == OVERLAPPED_REASON::DISCONNECT)
				{
					bContinue = true;
					LOG(L"ERROR", ERR, TEXTFILE, L"DisconnectEx Failed ErrCode : %u", WSAGetLastError());
					__debugbreak();
				}
				else
					break;
			}

			switch (pOverlapped->why)
			{
			case OVERLAPPED_REASON::SEND:
				pNetClient->SendProc(pSession, dwNOBT);
				break;
			case OVERLAPPED_REASON::RECV:
				if (!(bGQCSRet && dwNOBT == 0))
				{
					pNetClient->RecvProc(pSession, dwNOBT);
					pNetClient->TempLanRecvProc(pSession, dwNOBT);
				}
				break;
			case OVERLAPPED_REASON::POST:
				break;
			case OVERLAPPED_REASON::CONNECT:
				pNetClient->ConnectProc(pSession);
				break;
			case OVERLAPPED_REASON::DISCONNECT:
				bContinue = true;
				pNetClient->DisconnectProc(pSession);
				break;
			default:
				__debugbreak();
			}

		} while (0);

		if (bContinue)
			continue;

		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
		{
			pNetClient->ReleaseSession(pSession);
		}
	}
	return 0;
}

bool NetClient::SetLinger(SOCKET sock)
{
	linger linger;
	linger.l_linger = 0;
	linger.l_onoff = 1;
	return setsockopt(sock, SOL_SOCKET, SO_LINGER, (char*)&linger, sizeof(linger)) == 0;
}

bool NetClient::SetZeroCopy(SOCKET sock)
{
	DWORD dwSendBufSize = 0;
	return setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&dwSendBufSize, sizeof(dwSendBufSize)) == 0;
}

bool NetClient::SetReuseAddr(SOCKET sock)
{
	DWORD option = 1;
	return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&option, sizeof(option)) == 0;
}

bool NetClient::SetClientBind(SOCKET sock)
{
	SOCKADDR_IN addr;
	ZeroMemory(&addr, sizeof(SOCKADDR_IN));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
	addr.sin_port = ::htons(0);

	if (bind(sock, reinterpret_cast<const SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		DWORD errCode = WSAGetLastError();
		__debugbreak();
		return false;
	}
	return true;
}


NetClient::NetClient()
{
	std::locale::global(std::locale(""));
	char* pStart;
	char* pEnd;

	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	SOCKET dummySock = socket(AF_INET, SOCK_STREAM, 0);
	if (WsaIoctlWrapper(dummySock, WSAID_CONNECTEX, (LPVOID*)&lpfnConnectExPtr_) == false)
	{
		LOG(L"Start", ERR, TEXTFILE, L"WSAIoCtl ConnectEx Failed ErrCode : %u", WSAGetLastError());
		__debugbreak();
	}

	if (WsaIoctlWrapper(dummySock, WSAID_DISCONNECTEX, (LPVOID*)&lpfnDisconnectExPtr_) == false)
	{
		LOG(L"Start", ERR, TEXTFILE, L"WSAIoCtl ConnectEx Failed ErrCode : %u", WSAGetLastError());
		__debugbreak();
	}
	closesocket(dummySock);

	PARSER psr = CreateParser(L"ClientConfig.txt");
	WCHAR ipStr[16];
	GetValue(psr, L"BIND_IP", (PVOID*)&pStart, (PVOID*)&pEnd);
	unsigned long long stringLen = (pEnd - pStart) / sizeof(WCHAR);
	wcsncpy_s(ipStr, _countof(ipStr) - 1, (const WCHAR*)pStart, stringLen);
	// Null terminated String 으로 끝내야 InetPtonW쓸수잇음
	ipStr[stringLen] = 0;

	// sockAddr_ 초기화, ConnectEx에서 사용
	ZeroMemory(&sockAddr_, sizeof(sockAddr_));
	sockAddr_.sin_family = AF_INET;
	InetPtonW(AF_INET, ipStr, &sockAddr_.sin_addr);

	GetValue(psr, L"BIND_PORT", (PVOID*)&pStart, nullptr);
	short SERVER_PORT = (short)_wtoi((LPCWSTR)pStart);
	sockAddr_.sin_port = htons(SERVER_PORT);

	GetValue(psr, L"IOCP_WORKER_THREAD", (PVOID*)&pStart, nullptr);
	IOCP_WORKER_THREAD_NUM_ = (DWORD)_wtoi((LPCWSTR)pStart);

	GetValue(psr, L"IOCP_ACTIVE_THREAD", (PVOID*)&pStart, nullptr);
	IOCP_ACTIVE_THREAD_NUM_ = (DWORD)_wtoi((LPCWSTR)pStart);

	GetValue(psr, L"IS_ZERO_BYTE_SEND", (PVOID*)&pStart, nullptr);
	int bZeroByteSend = _wtoi((LPCWSTR)pStart);

	GetValue(psr, L"SESSION_MAX", (PVOID*)&pStart, nullptr);
	maxSession_ = _wtoi((LPCWSTR)pStart);

	GetValue(psr, L"PACKET_CODE", (PVOID*)&pStart, nullptr);
	Packet::PACKET_CODE = (unsigned char)_wtoi((LPCWSTR)pStart);

	GetValue(psr, L"PACKET_KEY", (PVOID*)&pStart, nullptr);
	Packet::FIXED_KEY = (unsigned char)_wtoi((LPCWSTR)pStart);
	ReleaseParser(psr);

	// IOCP 핸들 생성
	hcp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, IOCP_ACTIVE_THREAD_NUM_);
	if (!hcp_)
	{
		LOG(L"ONOFF", SYSTEM, TEXTFILE, L"CreateIoCompletionPort Fail ErrCode : %u", WSAGetLastError());
		__debugbreak();
	}
	LOG(L"ONOFF", SYSTEM, TEXTFILE, L"Create IOCP OK!");


	// 상위 17비트를 못쓰고 상위비트가 16개 이하가 되는날에는 뻑나라는 큰그림이다.
	if (!CAddressTranslator::CheckMetaCntBits())
		__debugbreak();

	pSessionArr_ = new Session[maxSession_];
	for (int i = maxSession_ - 1; i >= 0; --i)
		DisconnectStack_.Push(i);

	// 소켓미리 생성
	for (int i = 0; i < maxSession_; ++i)
	{
		Session* pSession = pSessionArr_ + i;
		pSession->sock_  = socket(AF_INET, SOCK_STREAM, 0);
		if (pSession->sock_ == INVALID_SOCKET)
			__debugbreak();

		SetClientBind(pSession->sock_);
		SetZeroCopy(pSession->sock_);
		SetLinger(pSession->sock_);
		SetReuseAddr(pSession->sock_);

		// IOCP에 소켓을 미리 등록해두기 CompletionKey는 sock_이 선언된 Session, 나중에 ConnectEx나 DisconnectEx가 호출되면 사용
		CreateIoCompletionPort((HANDLE)pSession->sock_, hcp_, (ULONG_PTR)pSession, 0);
	}

	// IOCP 워커스레드 생성(CREATE_SUSPENDED)
	hIOCPWorkerThreadArr_ = new HANDLE[IOCP_WORKER_THREAD_NUM_];
	for (DWORD i = 0; i < IOCP_WORKER_THREAD_NUM_; ++i)
	{
		hIOCPWorkerThreadArr_[i] = (HANDLE)_beginthreadex(NULL, 0, IOCPWorkerThread, this, CREATE_SUSPENDED, nullptr);
		if (!hIOCPWorkerThreadArr_[i])
		{
			LOG(L"ONOFF", SYSTEM, TEXTFILE, L"MAKE WorkerThread Fail ErrCode : %u", WSAGetLastError());
			__debugbreak();
		}
	}
	LOG(L"ONOFF", SYSTEM, TEXTFILE, L"MAKE IOCP WorkerThread OK Num : %u!", IOCP_WORKER_THREAD_NUM_);
}

NetClient::~NetClient()
{
	WSACleanup();
}


void NetClient::InitialConnect()
{
	while (true)
	{
		auto opt = DisconnectStack_.Pop();
		if (opt.has_value() == false)
			return;

		Session* pSession = pSessionArr_ + opt.value();
		//InterlockedExchange((LONG*)&pSession->bDisconnectCalled_, FALSE);

		ConnectPost(pSession);
	}
}


void NetClient::SendPacket(ULONGLONG id, SmartPacket& sendPacket)
{
	Session* pSession = pSessionArr_ + Session::GET_SESSION_INDEX(id);
	long IoCnt = InterlockedIncrement(&pSession->IoCnt_);

	// 이미 RELEASE 진행중이거나 RELEASE된 경우
	if ((IoCnt & Session::RELEASE_FLAG) == Session::RELEASE_FLAG)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// RELEASE 완료후 다시 세션에 대한 초기화가 완료된경우 즉 재활용
	if (id != pSession->id_)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// 인코딩
	sendPacket->SetHeader<Net>();
	sendPacket->IncreaseRefCnt();
	pSession->sendPacketQ_.Enqueue(sendPacket.GetPacket());
	SendPost(pSession);
	if (InterlockedDecrement(&pSession->IoCnt_) == 0)
		ReleaseSession(pSession);
}

void NetClient::SendPacket_ALREADY_ENCODED(ULONGLONG id, Packet* pPacket)
{
	Session* pSession = pSessionArr_ + Session::GET_SESSION_INDEX(id);
	long IoCnt = InterlockedIncrement(&pSession->IoCnt_);

	// 이미 RELEASE 진행중이거나 RELEASE된 경우
	if ((IoCnt & Session::RELEASE_FLAG) == Session::RELEASE_FLAG)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// RELEASE 완료후 다시 세션에 대한 초기화가 완료된경우 즉 재활용
	if (id != pSession->id_)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	pPacket->IncreaseRefCnt();
	pSession->sendPacketQ_.Enqueue(pPacket);
	SendPost(pSession);
	if (InterlockedDecrement(&pSession->IoCnt_) == 0)
		ReleaseSession(pSession);
}


void NetClient::Disconnect(ULONGLONG id)
{
	MEMORY_LOG(DISCONNECT, id);
	Session* pSession = pSessionArr_ + Session::GET_SESSION_INDEX(id);
	long IoCnt = InterlockedIncrement(&pSession->IoCnt_);

	// RELEASE진행중 혹은 진행완료
	if ((IoCnt & Session::RELEASE_FLAG) == Session::RELEASE_FLAG)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// RELEASE후 재활용까지 되엇을때
	if (id != pSession->id_)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// Disconnect 1회 제한

	if (InterlockedExchange((LONG*)&pSession->bDisconnectCalled_, TRUE) == TRUE)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// 여기 도달햇다면 같은 세션에 대해서 RELEASE 조차 호출되지 않은상태임이 보장된다
	CancelIoEx((HANDLE)pSession->sock_, &pSession->sendOverlapped.overlapped);
	CancelIoEx((HANDLE)pSession->sock_, &pSession->recvOverlapped.overlapped);

	// CancelIoEx호출로 인해서 RELEASE가 호출되엇어야 햇지만 위에서의 InterlockedIncrement 때문에 호출이 안된 경우 업보청산
	if (InterlockedDecrement(&pSession->IoCnt_) == 0)
		ReleaseSession(pSession);
}

bool NetClient::ConnectPost(Session* pSession)
{
	ZeroMemory(&pSession->connectOverlapped.overlapped, sizeof(WSAOVERLAPPED));
	pSession->connectOverlapped.why = OVERLAPPED_REASON::CONNECT;
	BOOL bConnectExRet = lpfnConnectExPtr_(pSession->sock_, (SOCKADDR*)&sockAddr_, sizeof(SOCKADDR_IN), nullptr, 0, NULL, &pSession->connectOverlapped.overlapped);
	if (bConnectExRet == FALSE)
	{
		DWORD errCode = WSAGetLastError();
		if (errCode == WSA_IO_PENDING)
			return true;

		OnConnectFailed(pSession->id_);
		DisconnectStack_.Push((short)(pSession - pSessionArr_));
		LOG(L"ERROR", ERR, TEXTFILE, L"ConnectEx ErrCode : %u", errCode);
		__debugbreak();
		return false;
	}
	return true;
}

bool NetClient::DisconnectExPost(Session* pSession)
{
	MEMORY_LOG(DISCONNECTEX_POST, pSession->id_);
	ZeroMemory(&(pSession->disconnectOverlapped.overlapped), sizeof(WSAOVERLAPPED));
	pSession->disconnectOverlapped.why = OVERLAPPED_REASON::DISCONNECT;
	BOOL bDisconnectExRet = lpfnDisconnectExPtr_(pSession->sock_, &pSession->disconnectOverlapped.overlapped, TF_REUSE_SOCKET, 0);
	if (bDisconnectExRet == FALSE)
	{
		DWORD errCode = WSAGetLastError();
		if (errCode == WSA_IO_PENDING)
			return true;

		LOG(L"ERROR", ERR, TEXTFILE, L"DisconnectEx ErrCode : %u", errCode);
		__debugbreak();
		return false;
	}
	return true;
}

BOOL NetClient::SendPost(Session* pSession)
{
	if (pSession->bDisconnectCalled_ == TRUE)
		return TRUE;

	// 현재 값을 TRUE로 바꾼다. 원래 TRUE엿다면 반환값이 TRUE일것이며 그렇다면 현재 SEND 진행중이기 때문에 그냥 빠저나간다
	// 이 조건문의 위치로 인하여 Out은 바뀌지 않을것임이 보장된다.
	// 하지만 SendPost 실행주체가 Send완료통지 스레드인 경우에는 in의 위치는 SendPacket으로 인해서 바뀔수가 있다.
	// iUseSize를 구하는 시점에서의 DirectDequeueSize의 값이 달라질수있다.
	if (InterlockedExchange((LONG*)&pSession->bSendingInProgress_, TRUE) == TRUE)
		return TRUE;

	// SendPacket에서 in을 옮겨서 UseSize가 0보다 커진시점에서 Send완료통지가 도착해서 Out을 옮기고 플래그 해제 Recv완료통지 스레드가 먼저 SendPost에 도달해 플래그를 선점한경우 UseSize가 0이나온다.
	// 여기서 flag를 다시 FALSE로 바꾸어주지 않아서 멈춤발생
	DWORD dwBufferNum = pSession->sendPacketQ_.GetSize();
	if (dwBufferNum == 0)
	{
		InterlockedExchange((LONG*)&pSession->bSendingInProgress_, FALSE);
		return TRUE;
	}

	WSABUF wsa[50];
	DWORD i;
	for (i = 0; i < 50 && i < dwBufferNum; ++i)
	{
		Packet* pPacket = pSession->sendPacketQ_.Dequeue().value();
		wsa[i].buf = (char*)pPacket->pBuffer_;
		//wsa[i].len = pPacket->GetUsedDataSize() + sizeof(Packet::NetHeader);
		wsa[i].len = pPacket->GetUsedDataSize() + sizeof(Packet::LanHeader);
		pSession->pSendPacketArr_[i] = pPacket;
	}

	InterlockedExchange(&pSession->lSendBufNum_, i);
	InterlockedIncrement(&pSession->IoCnt_);
	ZeroMemory(&(pSession->sendOverlapped.overlapped), sizeof(WSAOVERLAPPED));
	pSession->sendOverlapped.why = OVERLAPPED_REASON::SEND;
	int iSendRet = WSASend(pSession->sock_, wsa, i, nullptr, 0, &pSession->sendOverlapped.overlapped, nullptr);
	if (iSendRet == SOCKET_ERROR)
	{
		DWORD dwErrCode = WSAGetLastError();
		if (dwErrCode == WSA_IO_PENDING)
		{
			if (pSession->bDisconnectCalled_ == TRUE)
			{
				CancelIoEx((HANDLE)pSession->sock_, &pSession->sendOverlapped.overlapped);
				return FALSE;
			}
			return TRUE;
		}

		InterlockedDecrement(&(pSession->IoCnt_));
		if (dwErrCode == WSAECONNRESET)
			return FALSE;

		LOG(L"Disconnect", ERR, TEXTFILE, L"Disconnected By ErrCode : %u", dwErrCode);
		return FALSE;
	}
	return TRUE;
}

BOOL NetClient::RecvPost(Session* pSession)
{
	if (pSession->bDisconnectCalled_ == TRUE)
		return FALSE;

	WSABUF wsa[2];
	wsa[0].buf = pSession->recvRB_.GetWriteStartPtr();
	wsa[0].len = pSession->recvRB_.DirectEnqueueSize();
	wsa[1].buf = pSession->recvRB_.Buffer_;
	wsa[1].len = pSession->recvRB_.GetFreeSize() - wsa[0].len;

	ZeroMemory(&pSession->recvOverlapped.overlapped, sizeof(WSAOVERLAPPED));
	pSession->recvOverlapped.why = OVERLAPPED_REASON::RECV;
	DWORD flags = 0;
	InterlockedIncrement(&pSession->IoCnt_);
	int iRecvRet = WSARecv(pSession->sock_, wsa, 2, nullptr, &flags, &pSession->recvOverlapped.overlapped, nullptr);
	if (iRecvRet == SOCKET_ERROR)
	{
		DWORD dwErrCode = WSAGetLastError();
		if (dwErrCode == WSA_IO_PENDING)
		{
			if (pSession->bDisconnectCalled_ == TRUE)
			{
				CancelIoEx((HANDLE)pSession->sock_, &pSession->recvOverlapped.overlapped);
				return FALSE;
			}
			return TRUE;
		}

		InterlockedDecrement(&(pSession->IoCnt_));
		if (dwErrCode == WSAECONNRESET)
			return FALSE;

		LOG(L"Disconnect", ERR, TEXTFILE, L"Disconnected By ErrCode : %u", dwErrCode);
		return FALSE;
	}
	return TRUE;
}

void NetClient::ReleaseSession(Session* pSession)
{
	MEMORY_LOG(RELEASE_SESSION, pSession->id_);
	if (InterlockedCompareExchange(&pSession->IoCnt_, Session::RELEASE_FLAG | 0, 0) != 0)
		return;

	DisconnectExPost(pSession);
}


void NetClient::RecvProc(Session* pSession, int numberOfBytesTransferred)
{
	using NetHeader = Packet::NetHeader;
	pSession->recvRB_.MoveInPos(numberOfBytesTransferred);
	while (1)
	{
		if (pSession->bDisconnectCalled_ == TRUE)
			return;

		Packet::NetHeader header;
		if (pSession->recvRB_.Peek((char*)&header, sizeof(NetHeader)) == 0)
			break;

		if (header.code_ != Packet::PACKET_CODE)
		{
			Disconnect(pSession->id_);
			return;
		}

		if (pSession->recvRB_.GetUseSize() < sizeof(NetHeader) + header.payloadLen_)
		{
			if (header.payloadLen_ > BUFFER_SIZE)
			{
				Disconnect(pSession->id_);
				return;
			}
			break;
		}

		pSession->recvRB_.MoveOutPos(sizeof(NetHeader));

		SmartPacket sp = PACKET_ALLOC(Net);
		pSession->recvRB_.Dequeue(sp->GetPayloadStartPos<Net>(), header.payloadLen_);
		sp->MoveWritePos(header.payloadLen_);
		memcpy(sp->pBuffer_, &header, sizeof(Packet::NetHeader));

		// 넷서버에서만 호출되는 함수로 검증 및 디코드후 체크섬 확인
		if (sp->ValidateReceived() == false)
		{
			Disconnect(pSession->id_);
			return;
		}

		pSession->lastRecvTime = GetTickCount64();
		OnRecv(pSession->id_, sp.GetPacket());
	}
	RecvPost(pSession);
}

void NetClient::TempLanRecvProc(Session* pSession, int numberOfBytesTransferred)
{
	using LanHeader = Packet::LanHeader;
	pSession->recvRB_.MoveInPos(numberOfBytesTransferred);
	while (1)
	{
		if (pSession->bDisconnectCalled_ == TRUE)
			return;

		Packet::LanHeader header;
		if (pSession->recvRB_.Peek((char*)&header, sizeof(LanHeader)) == 0)
			break;

		if (pSession->recvRB_.GetUseSize() < sizeof(LanHeader) + header.payloadLen_)
			break;

		SmartPacket sp = PACKET_ALLOC(Lan);

		// 직렬화버퍼에 헤더내용쓰기
		memcpy(sp->pBuffer_, &header, sizeof(Packet::LanHeader));
		pSession->recvRB_.MoveOutPos(sizeof(LanHeader));

		// 페이로드 직렬화버퍼로 빼기
		pSession->recvRB_.Dequeue(sp->GetPayloadStartPos<Lan>(), header.payloadLen_);
		sp->MoveWritePos(header.payloadLen_);

		pSession->lastRecvTime = GetTickCount64();
		OnRecv(pSession->id_, sp.GetPacket());
	}
	RecvPost(pSession);
}

