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
	// Release �� Session�� ����ȭ ���� ����
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
	// ���⼭���� �ٽ� Connect�� ���������� ������ ������ �� �Լ� ���ϴܿ� �;���
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
	// Null terminated String ���� ������ InetPtonW��������
	ipStr[stringLen] = 0;

	// sockAddr_ �ʱ�ȭ, ConnectEx���� ���
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

	// IOCP �ڵ� ����
	hcp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, IOCP_ACTIVE_THREAD_NUM_);
	if (!hcp_)
	{
		LOG(L"ONOFF", SYSTEM, TEXTFILE, L"CreateIoCompletionPort Fail ErrCode : %u", WSAGetLastError());
		__debugbreak();
	}
	LOG(L"ONOFF", SYSTEM, TEXTFILE, L"Create IOCP OK!");


	// ���� 17��Ʈ�� ������ ������Ʈ�� 16�� ���ϰ� �Ǵ³����� ������� ū�׸��̴�.
	if (!CAddressTranslator::CheckMetaCntBits())
		__debugbreak();

	pSessionArr_ = new Session[maxSession_];
	for (int i = maxSession_ - 1; i >= 0; --i)
		DisconnectStack_.Push(i);

	// ���Ϲ̸� ����
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

		// IOCP�� ������ �̸� ����صα� CompletionKey�� sock_�� ����� Session, ���߿� ConnectEx�� DisconnectEx�� ȣ��Ǹ� ���
		CreateIoCompletionPort((HANDLE)pSession->sock_, hcp_, (ULONG_PTR)pSession, 0);
	}

	// IOCP ��Ŀ������ ����(CREATE_SUSPENDED)
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

	// �̹� RELEASE �������̰ų� RELEASE�� ���
	if ((IoCnt & Session::RELEASE_FLAG) == Session::RELEASE_FLAG)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// RELEASE �Ϸ��� �ٽ� ���ǿ� ���� �ʱ�ȭ�� �Ϸ�Ȱ�� �� ��Ȱ��
	if (id != pSession->id_)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// ���ڵ�
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

	// �̹� RELEASE �������̰ų� RELEASE�� ���
	if ((IoCnt & Session::RELEASE_FLAG) == Session::RELEASE_FLAG)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// RELEASE �Ϸ��� �ٽ� ���ǿ� ���� �ʱ�ȭ�� �Ϸ�Ȱ�� �� ��Ȱ��
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

	// RELEASE������ Ȥ�� ����Ϸ�
	if ((IoCnt & Session::RELEASE_FLAG) == Session::RELEASE_FLAG)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// RELEASE�� ��Ȱ����� �Ǿ�����
	if (id != pSession->id_)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// Disconnect 1ȸ ����

	if (InterlockedExchange((LONG*)&pSession->bDisconnectCalled_, TRUE) == TRUE)
	{
		if (InterlockedDecrement(&pSession->IoCnt_) == 0)
			ReleaseSession(pSession);
		return;
	}

	// ���� �����޴ٸ� ���� ���ǿ� ���ؼ� RELEASE ���� ȣ����� ������������ ����ȴ�
	CancelIoEx((HANDLE)pSession->sock_, &pSession->sendOverlapped.overlapped);
	CancelIoEx((HANDLE)pSession->sock_, &pSession->recvOverlapped.overlapped);

	// CancelIoExȣ��� ���ؼ� RELEASE�� ȣ��Ǿ���� ������ �������� InterlockedIncrement ������ ȣ���� �ȵ� ��� ����û��
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

	// ���� ���� TRUE�� �ٲ۴�. ���� TRUE���ٸ� ��ȯ���� TRUE�ϰ��̸� �׷��ٸ� ���� SEND �������̱� ������ �׳� ����������
	// �� ���ǹ��� ��ġ�� ���Ͽ� Out�� �ٲ��� ���������� ����ȴ�.
	// ������ SendPost ������ü�� Send�Ϸ����� �������� ��쿡�� in�� ��ġ�� SendPacket���� ���ؼ� �ٲ���� �ִ�.
	// iUseSize�� ���ϴ� ���������� DirectDequeueSize�� ���� �޶������ִ�.
	if (InterlockedExchange((LONG*)&pSession->bSendingInProgress_, TRUE) == TRUE)
		return TRUE;

	// SendPacket���� in�� �Űܼ� UseSize�� 0���� Ŀ���������� Send�Ϸ������� �����ؼ� Out�� �ű�� �÷��� ���� Recv�Ϸ����� �����尡 ���� SendPost�� ������ �÷��׸� �����Ѱ�� UseSize�� 0�̳��´�.
	// ���⼭ flag�� �ٽ� FALSE�� �ٲپ����� �ʾƼ� ����߻�
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

		// �ݼ��������� ȣ��Ǵ� �Լ��� ���� �� ���ڵ��� üũ�� Ȯ��
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

		// ����ȭ���ۿ� ������뾲��
		memcpy(sp->pBuffer_, &header, sizeof(Packet::LanHeader));
		pSession->recvRB_.MoveOutPos(sizeof(LanHeader));

		// ���̷ε� ����ȭ���۷� ����
		pSession->recvRB_.Dequeue(sp->GetPayloadStartPos<Lan>(), header.payloadLen_);
		sp->MoveWritePos(header.payloadLen_);

		pSession->lastRecvTime = GetTickCount64();
		OnRecv(pSession->id_, sp.GetPacket());
	}
	RecvPost(pSession);
}

