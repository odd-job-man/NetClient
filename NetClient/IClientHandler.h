#pragma once

struct Session;
class Packet;
class SmartPacket;
class IClientHandler
{
	virtual void OnRecv(ULONGLONG id, Packet* pPacket) = 0;
	virtual void OnRelease(ULONGLONG id) = 0;
	virtual void OnError(ULONGLONG id, int errorType, Packet* pRcvdPacket) = 0;
private:
	virtual BOOL SendPost(Session* pSession) = 0;
	virtual BOOL RecvPost(Session* pSession) = 0;
	virtual void ReleaseSession(Session* pSession) = 0;

};
