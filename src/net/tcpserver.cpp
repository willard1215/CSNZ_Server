#include "net/tcpserver.h"
#include "net/extendedsocket.h"
#include "interface/net/iserverlistener.h"
#include "serverconfig.h"

#include "common/net/netdefs.h"
#include "common/utils.h"
#include "common/logger.h"

#define CERT_FILE "Data/Certs/server-cert.pem"
#define KEY_FILE  "Data/Certs/server-key.pem"

using namespace std;

/** 
 * Constructor.
 */
CTCPServer::CTCPServer() : m_ListenThread(ListenThread, this)
{
	m_Socket = INVALID_SOCKET;
	m_bIsRunning = false;
	m_pListener = NULL;
	m_pCriticalSection = NULL;
 	m_nNextClientIndex = 0;
	m_nResult = 0;
	m_pCTX = NULL;

#ifdef WIN32
	WSADATA wsaData;
	int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (result != 0)
	{
		Logger().Fatal("WSAStartup() failed with error: %d\n%s\n", m_nResult, WSAGetLastErrorString());
	}
#endif
}

/** 
 * Destructor. Stop the server on destructor
 */
CTCPServer::~CTCPServer()
{
	Stop();
}

/** 
 * Create socket and start listening.
 * @param port Server's port
 * @param tcpSendBufferSize Send buffer size for TCP
 * @param ssl Whether or not to use SSL
 * @return True on success, false on error
 */
bool CTCPServer::Start(const string& port, int tcpSendBufferSize, bool ssl)
{
	if (IsRunning())
		return false;
	
	// Initialize wolfSSL
	wolfSSL_Init();

	if (ssl)
		InitSSLContext();

	struct addrinfo* result = NULL;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	// Resolve the server address and port
	m_nResult = getaddrinfo(NULL, port.c_str(), &hints, &result);
	if (m_nResult != 0)
	{
		Logger().Fatal("getaddrinfo() failed with error: %d\n%s\n", m_nResult, WSAGetLastErrorString());
		return false;
	}

	// Create a SOCKET for connecting to server
	m_Socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (m_Socket == INVALID_SOCKET)
	{
		Logger().Fatal("socket() failed with error: %ld\n%s\n", GetNetworkError(), WSAGetLastErrorString());
		freeaddrinfo(result);
		return false;
	}
	
	// Set the mode of the socket to be nonblocking
	u_long iMode = 1;
	m_nResult = ioctlsocket(m_Socket, FIONBIO, &iMode);
	if (m_nResult == SOCKET_ERROR)
	{
		Logger().Fatal("ioctlsocket() failed with error: %d\n%s\n", GetNetworkError(), WSAGetLastErrorString());
		closesocket(m_Socket);
		return false;
	}

	// Setup the TCP listening socket
	m_nResult = ::bind(m_Socket, result->ai_addr, (int)result->ai_addrlen);
	if (m_nResult == SOCKET_ERROR)
	{
		Logger().Fatal("bind failed with error: %d\n%s\n", GetNetworkError(), WSAGetLastErrorString());
		freeaddrinfo(result);
		closesocket(m_Socket);
		return false;
	}

	freeaddrinfo(result);

	// start listening for new clients attempting to connect
	m_nResult = listen(m_Socket, SOMAXCONN);
	if (m_nResult == SOCKET_ERROR)
	{
		Logger().Fatal("listen() failed with error: %d\n%s\n", GetNetworkError(), WSAGetLastErrorString());
		closesocket(m_Socket);
		return false;
	}

	m_nResult = setsockopt(m_Socket, SOL_SOCKET, SO_SNDBUF, (char*)&tcpSendBufferSize, sizeof(tcpSendBufferSize));
	if (m_nResult == SOCKET_ERROR)
	{
		Logger().Fatal("setsockopt failed with error %d\n%s\n", GetNetworkError(), WSAGetLastErrorString());
		closesocket(m_Socket);
		return false;
	}

	WSAPOLLFD fd;
	fd.fd = m_Socket;
	fd.events = POLLRDNORM;
	fd.revents = 0;
	m_fds.push_back(fd);

	m_bIsRunning = true;
	
	m_ListenThread.Start();

	return true;
}

/** 
 * Stop the server
 */
void CTCPServer::Stop()
{
	if (IsRunning())
	{
		m_bIsRunning = false;

		m_ListenThread.Join();

		for (auto client : m_Clients)
		{
			delete client;
		}

		m_fds.clear();

		closesocket(m_Socket);

		if (m_pCTX)
		{
			// Free the wolfSSL context object
			wolfSSL_CTX_free(m_pCTX);
		}

		// Cleanup the wolfSSL environment
		wolfSSL_Cleanup();
	}
}

/**
 * Listen and wait for incoming data
 */
void CTCPServer::Listen()
{	
	int result = poll(m_fds.data(), m_fds.size(), 1000);
	if (result == SOCKET_ERROR)
	{
		Logger().Error("poll() failed with error: %d\n", GetNetworkError());

		if (m_pListener)
			m_pListener->OnTCPError(0);

		return;
	}

	// nothing happens
	if (!result)
		return;

	if (m_pCriticalSection)
		m_pCriticalSection->Enter();

	for (auto it = m_fds.begin(); it != m_fds.end(); it++)
	{
		if (it->revents & POLLRDNORM)
		{
			if (it->fd == m_Socket)
			{
				IExtendedSocket* socket = Accept(m_nNextClientIndex);
				if (!socket)
					continue;

				Logger().Info("Client (%d, %s) has been connected to the server\n", m_nNextClientIndex, socket->GetIP().c_str());

				if (m_pListener)
				{
					if (!m_pListener->OnTCPConnectionCreated(socket))
					{
						// start cycle from the beginning after disconnecting the IP banned client, probably not the best solution
						it = m_fds.begin();
						continue;
					}
				}

				m_nNextClientIndex++;

				WSAPOLLFD fd;
				fd.fd = socket->GetSocket();
				fd.events = POLLRDNORM | POLLWRNORM;
				fd.revents = 0;
				m_fds.push_back(fd);

				// start cycle from the beginning after pushing to vector, probably not the best solution
				it = m_fds.begin();
				continue;
			}
			// read from client
			else
			{
				IExtendedSocket* socket = GetExSocketBySocket(it->fd);
				if (!socket)
					continue;

				CReceivePacket* msg = socket->Read();
				int readResult = socket->GetReadResult();
				if (readResult == 0)
				{
					Logger().Info("TCP read closed by peer (%d, %s): readResult=%d, pendingMsg=%d, revents=0x%X\n",
						socket->GetID(), socket->GetIP().c_str(), readResult, socket->GetMsg() ? 1 : 0, it->revents);
					it->revents |= POLLHUP;
				}
				else if (readResult == SOCKET_ERROR)
				{
					Logger().Warn("TCP read socket error (%d, %s): readResult=%d, WSAGetLastError=%d, pendingMsg=%d, revents=0x%X\n",
						socket->GetID(), socket->GetIP().c_str(), readResult, GetNetworkError(), socket->GetMsg() ? 1 : 0, it->revents);
					it->revents |= POLLERR;

					if (m_pListener)
						m_pListener->OnTCPError(0);
				}
				else if (!msg)
				{
					// packet is not valid or wrong sequence or decryption failed
					// exclude case when message is not fully read
					if (!socket->GetMsg())
					{
						Logger().Warn("TCP read invalid packet (%d, %s): readResult=%d, revents=0x%X\n",
							socket->GetID(), socket->GetIP().c_str(), readResult, it->revents);
						it->revents |= POLLERR;

						if (m_pListener)
							m_pListener->OnTCPError(0);
					}
				}
				else
				{
					// Call this to mark that socket is ready to receive a new message
					socket->SetMsg(NULL);

					// Important: responsibility for deleting the message is assumed by the listener
					/// @todo use shared_ptr for messages?
					if (m_pListener)
						m_pListener->OnTCPMessage(socket, msg);
					else
						delete msg;
				}
			}
		}

		// write to client
		if (it->revents & POLLWRNORM && !(it->revents & (POLLERR | POLLHUP)))
		{
			IExtendedSocket* socket = GetExSocketBySocket(it->fd);
			if (!socket)
				continue;

			if (socket->GetPacketsToSend().size())
			{
				CSendPacket* msg = socket->GetPacketsToSend().at(0);
				int sendResult = socket->Send(msg, true);
				if (sendResult <= 0)
				{
					int error = GetNetworkError();
					if (error != WSAEWOULDBLOCK)
					{
						it->revents |= POLLERR;

						Logger().Warn("An error occurred while sending packet from queue: WSAGetLastError: %d, queue.size: %d\n", error, socket->GetPacketsToSend().size());

						if (m_pListener)
							m_pListener->OnTCPError(0);
					}
				}
				else
				{
					socket->GetPacketsToSend().erase(socket->GetPacketsToSend().begin());
				}
			}
		}

		// client closed the connection or had a socket error or sent an invalid packet or sent wrong sequence or decryption failed
		if (it->revents & (POLLERR | POLLHUP) && it->fd != m_Socket)
		{
			IExtendedSocket* socket = GetExSocketBySocket(it->fd);
			if (socket)
			{
				Logger().Info("TCP disconnect requested (%d, %s): revents=0x%X, bytesSent=%d, bytesReceived=%d, sendQueue=%d, pendingMsg=%d\n",
					socket->GetID(), socket->GetIP().c_str(), it->revents, socket->GetBytesSent(), socket->GetBytesReceived(),
					socket->GetPacketsToSend().size(), socket->GetMsg() ? 1 : 0);
			}
			if (socket)
				DisconnectClient(socket);

			it = m_fds.begin(); // not the best solution
			continue;
		}
	}

	if (m_pCriticalSection)
		m_pCriticalSection->Leave();
}

/**
 * Initialize SSL context
 */
void CTCPServer::InitSSLContext()
{
	if (m_pCTX)
		return;

	// Create and initialize WOLFSSL_CTX
	m_pCTX = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
	if (m_pCTX == NULL)
	{
		Logger().Error("wolfSSL_CTX_new() failed to create WOLFSSL_CTX. Continuing without SSL...\n", m_nResult, WSAGetLastErrorString());
		g_pServerConfig->ssl = false;
		return;
	}

	// Load server certificates into WOLFSSL_CTX
	if (wolfSSL_CTX_use_certificate_file(m_pCTX, CERT_FILE, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS)
	{
		Logger().Error("wolfSSL_CTX_use_certificate_file() failed to load %s, please check the file. Continuing without SSL...\n", CERT_FILE);
		m_pCTX = NULL;
		g_pServerConfig->ssl = false;
		return;
	}

	// Load server key into WOLFSSL_CTX
	if (wolfSSL_CTX_use_PrivateKey_file(m_pCTX, KEY_FILE, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS)
	{
		Logger().Error("wolfSSL_CTX_use_PrivateKey_file() failed to load %s, please check the file. Continuing without SSL...\n", KEY_FILE);
		m_pCTX = NULL;
		g_pServerConfig->ssl = false;
		return;
	}
}

/**
 * Accepts connection on a socket
 * @param id Number 
 * @return Pointer to CExtendedSocket, NULL on error
 */
IExtendedSocket* CTCPServer::Accept(unsigned int id)
{
	sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);

	SOCKET clientSocket = accept(m_Socket, (sockaddr*)&addr, &addrlen);
	if (clientSocket == INVALID_SOCKET)
	{
		Logger().Fatal("accept() failed with error: %d\n%s\n", GetNetworkError(), WSAGetLastErrorString());
		return NULL;
	}

	// set SO_KEEPALIVE for socket
	char value = 1;
	setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));

	// get IP
	char ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);

	CExtendedSocket* newSocket = new CExtendedSocket(clientSocket, id);
	newSocket->SetIP(ip);

	m_Clients.push_back(newSocket);

	// send server connected message
	static const string connectedMsg = TCP_CONNECTED_MESSAGE;
	static vector<unsigned char> msg(connectedMsg.begin(), connectedMsg.end());
	newSocket->Send(msg, true);

	if (g_pServerConfig->ssl)
	{
		// Create a WOLFSSL object
		WOLFSSL* newSSL = wolfSSL_new(m_pCTX);
		if (newSSL == NULL)
		{
			Logger().Fatal("wolfSSL_new() failed to create WOLFSSL object\n");
			return NULL;
		}

		// Attach wolfSSL to the socket
		wolfSSL_set_fd(newSSL, clientSocket);

		// Establish TLS connection
		int ret, err;
		do
		{
			ret = wolfSSL_accept(newSSL);
			err = wolfSSL_get_error(newSSL, ret);
		} while (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE);

		if (ret != WOLFSSL_SUCCESS)
			Logger().Error("wolfSSL_accept() failed for client (%s): ret=%d, err=%d\n", ip, ret, err);
		else
			Logger().Info("TLS handshake completed with client (%s)\n", ip);

		newSocket->SetSSLObject(newSSL);
	}

	return newSocket;
}

/**
 * Gets extended socket by socket descriptor value
 * @param socket
 * @return Pointer to CExtendedSocket, NULL if not exists
 */
IExtendedSocket* CTCPServer::GetExSocketBySocket(SOCKET socket)
{
	for (auto s : m_Clients)
	{
		if (s->GetSocket() == socket)
			return s;
	}

	return NULL;
}

/**
 * Disconnects client by extended socket object and deletes it
 * @param socket
 */
void CTCPServer::DisconnectClient(IExtendedSocket* socket)
{
	// connection closed
	if (m_pListener)
		m_pListener->OnTCPConnectionClosed(socket);

	Logger().Info("Client (%d, %s) has been disconnected from the server\n", socket->GetID(), socket->GetIP().c_str());

	SOCKET s = socket->GetSocket();
	m_fds.erase(remove_if(m_fds.begin(), m_fds.end(),
		[s](WSAPOLLFD fd)
		{
			if (fd.fd == s)
			{
				return true;
			}

			return false;
		}
	), m_fds.end());

	delete socket;
	m_Clients.erase(remove(m_Clients.begin(), m_Clients.end(), socket), m_Clients.end());
}

/**
 * Gets all clients connected to the server
 * @return vector of extended socket
 */
vector<IExtendedSocket*>& CTCPServer::GetClients()
{
	return m_Clients;
}

/**
 * Checks if server is running
 * @return Running status
 */
bool CTCPServer::IsRunning()
{
	return m_bIsRunning;
}

/**
 * Sets listen object
 * @param listener Listener object
 */
void CTCPServer::SetListener(IServerListenerTCP* listener)
{
	m_pListener = listener;
}

/**
 * Sets critical section object. Critical section is used for interacting with the listener object or extended socket
 * @param criticalSection Pointer to critical section object
 */
void CTCPServer::SetCriticalSection(CCriticalSection* criticalSection)
{
	m_pCriticalSection = criticalSection;
}
