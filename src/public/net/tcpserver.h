#pragma once

#include "socketshared.h"
#include "common/thread.h"
#include <wolfssl/ssl.h>

#include <string>
#include <vector>
#include <chrono>

class IExtendedSocket;
class CExtendedSocket;
class IServerListenerTCP;

/**
 * Class that accepts TCP connection
 */
class CTCPServer : public ISocketListenable
{
public:
	CTCPServer();
	~CTCPServer();
	
	bool Start(const std::string& port, int tcpSendBufferSize, bool ssl);
	void Stop();
	void Listen();
	void InitSSLContext();

	IExtendedSocket* Accept(unsigned int id);
	IExtendedSocket* GetExSocketBySocket(SOCKET socket);
	void DisconnectClient(IExtendedSocket* socket);
	std::vector<IExtendedSocket*>& GetClients();

	bool IsRunning();

	void SetListener(IServerListenerTCP* listener);
	void SetCriticalSection(CCriticalSection* criticalSection);

private:
	SOCKET m_Socket;
	bool m_bIsRunning;
	bool m_bSSL;
	int m_nResult;
	CThread m_ListenThread;
	unsigned int m_nNextClientIndex;
	std::vector<IExtendedSocket*> m_Clients;
	IServerListenerTCP* m_pListener;
	CCriticalSection* m_pCriticalSection;

	std::vector<WSAPOLLFD> m_fds;
	WOLFSSL_CTX* m_pCTX;
};
