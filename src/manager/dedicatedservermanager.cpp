#include "dedicatedservermanager.h"
#include "usermanager.h"
#include "packetmanager.h"
#include "serverconfig.h"
#include "main.h"
#include "common/utils.h"

#include <cstdlib>
#include <thread>

static bool IsEnvEnabled(const char* name)
{
	const char* value = getenv(name);
	return value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' || value[0] == 't' || value[0] == 'T');
}

CDedicatedServer::CDedicatedServer(IExtendedSocket* socket, int ip, int port)
{
	m_pSocket = socket;
	m_iIP = ip;
	m_iPort = port; // dedi client/server port
	m_pRoom = NULL;
	m_iLastMemory = 0;

	g_UserManager.SendCrypt(socket);
}

void CDedicatedServer::SetRoom(IRoom* room)
{
	m_pRoom = room;
}

void CDedicatedServer::SetMemoryUsage(int memShift)
{
	m_iLastMemory = memShift << 20;
}

IExtendedSocket* CDedicatedServer::GetSocket()
{
	return m_pSocket;
}

IRoom* CDedicatedServer::GetRoom()
{
	return m_pRoom;
}

int CDedicatedServer::GetMemoryUsage()
{
	return m_iLastMemory;
}

int CDedicatedServer::GetIP()
{
	return m_iIP;
}

int CDedicatedServer::GetPort()
{
	return m_iPort;
}

CDedicatedServerManager g_DedicatedServerManager;

CDedicatedServerManager::CDedicatedServerManager() : CBaseManager("DedicatedServerManager")
{
	SetCanReload(false);
}

CDedicatedServerManager::~CDedicatedServerManager()
{
}

/**
 * Sends a stop message to all dedi
 */
void CDedicatedServerManager::Shutdown()
{
	for (auto server : m_vServerPools)
		g_PacketManager.SendHostServerStop(server->GetSocket());
}

bool CDedicatedServerManager::OnPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case HostServerPacketType::AddServer:
	{
		// If dedicated server is not in whitelist, don't add it to the pool
		if (std::find(g_pServerConfig->dedicatedServerWhitelist.begin(), g_pServerConfig->dedicatedServerWhitelist.end(), socket->GetIP()) == g_pServerConfig->dedicatedServerWhitelist.end())
		{
			Logger().Warn("CDedicatedServerManager::OnPacket(AddServer): IP %s is not in the dedicated server whitelist, ignoring\n", socket->GetIP().c_str());
			return true;
		}

		int port = msg->ReadUInt16(); // -port, default is 27015
		int ip = msg->ReadUInt32(true); // ip from -hostip dedi argument
		Logger().Info("CDedicatedServerManager::OnPacket(AddServer): socket=%s advertised=%s:%d\n",
			socket->GetIP().c_str(), ip_to_string(ip).c_str(), port);

		// if IP is not specified by dedi server, use IP from socket
		if (ip == 0)
		{
			ip = ip_string_to_int(socket->GetIP());
			Logger().Warn("CDedicatedServerManager::OnPacket(AddServer): no IP specified, using IP: %s\n", socket->GetIP().c_str());
		}

		AddServer(socket, ip, port);

		break;
	}
	case HostServerPacketType::MemoryUsage:
	{
		CDedicatedServer* server = GetServerBySocket(socket);
		if (server)
			server->SetMemoryUsage(msg->ReadUInt16());

		break;
	}
	case HostServerPacketType::Unk2:
	{
		// Integer in "rev.txt" file
		int unk = msg->ReadUInt32();
		Logger().Warn("CDedicatedServerManager::OnPacket(2): %d\n", unk);

		break;
	}
	case HostServerPacketType::Unk3:
	{
		// I think it's a string
		std::string unk = msg->ReadString();
		Logger().Warn("CDedicatedServerManager::OnPacket(3): %s\n", unk.c_str());
		CDedicatedServer* server = GetServerBySocket(socket);
		if (server)
		{
			Logger().Info("Dedicated host server ready, scheduling metadata\n");
			std::thread([socket]()
			{
				SleepMS(2000);
				g_Events.AddEventFunction([socket]()
				{
					if (!g_pServerInstance->IsServerActive())
						return;

					if (g_DedicatedServerManager.GetServerBySocket(socket) == NULL)
						return;

					Logger().Info("Sending delayed dedicated host metadata\n");
					g_UserManager.SendMetadata(socket);

					if (!IsEnvEnabled("CSNZ_DEDI_USER_BOOTSTRAP"))
					{
						Logger().Info("Skipping dedicated host user bootstrap; set CSNZ_DEDI_USER_BOOTSTRAP=1 only for diagnostics\n");
						return;
					}

					std::thread([socket]()
					{
						SleepMS(500);
						g_Events.AddEventFunction([socket]()
						{
							if (!g_pServerInstance->IsServerActive())
								return;

							if (g_DedicatedServerManager.GetServerBySocket(socket) == NULL)
								return;

							Logger().Info("Sending dedicated host user bootstrap after metadata grace period\n");
							g_UserManager.BootstrapLocalUserAfterMetadata(socket);
						});

						SleepMS(500);
						g_Events.AddEventFunction([socket]()
						{
							if (!g_pServerInstance->IsServerActive())
								return;

							if (g_DedicatedServerManager.GetServerBySocket(socket) == NULL)
								return;

							Logger().Info("Sending delayed live metadata payloads after lobby bootstrap: item/voxel/codis\n");
							g_PacketManager.SendMetadataItem(socket);
							g_PacketManager.SendMetadataVoxelList(socket);
							g_PacketManager.SendMetadataCodisData(socket);
						});
					}).detach();
				});
			}).detach();
		}

		break;
	}
	default:
		Logger().Warn("Unknown host server request %d\n", type);
		break;
	}

	return true;
}

/**
 * Get free dedi server and link it with room
 * @return NULL if no such dedi server
 */
CDedicatedServer* CDedicatedServerManager::GetAvailableServerFromPools(IRoom* room)
{
	for (auto server : m_vServerPools)
	{
		if (!server->GetRoom())
		{
			server->SetRoom(room);
			Logger().Info("CDedicatedServerManager::GetAvailableServerFromPools: assigned %s:%d to room\n",
				ip_to_string(server->GetIP()).c_str(), server->GetPort());
			return server;
		}
	}

	Logger().Warn("CDedicatedServerManager::GetAvailableServerFromPools: no free dedicated server available, pool=%d\n",
		m_vServerPools.size());
	return NULL;
}

/**
 * Checks if there is a free dedi server to play
 */
bool CDedicatedServerManager::IsPoolAvailable()
{
	for (auto server : m_vServerPools)
		if (!server->GetRoom())
			return true;

	return false;
}

/**
 * Create and add dedi server object to the pool
 */
void CDedicatedServerManager::AddServer(IExtendedSocket* socket, int ip, int port)
{
	if (GetServerBySocket(socket))
	{
		Logger().Error("CDedicatedServerManager::AddServer: %s:%d duplicate\n", socket->GetIP(), ntohs(port));
		return;
	}

	CDedicatedServer* server = new CDedicatedServer(socket, ip, port);
	m_vServerPools.push_back(server);
	Logger().Info("CDedicatedServerManager::AddServer: added %s:%d socket=%s pool=%d\n",
		ip_to_string(ip).c_str(), port, socket->GetIP().c_str(), m_vServerPools.size());
}

CDedicatedServer* CDedicatedServerManager::GetServerBySocket(IExtendedSocket* socket)
{
	for (auto server : m_vServerPools)
		if (server->GetSocket() == socket)
			return server;

	return NULL;
}

/**
 * Removes dedicated server from pool if it crashes or just turns off
 */
void CDedicatedServerManager::RemoveServer(IExtendedSocket* socket)
{
	CDedicatedServer* server = GetServerBySocket(socket);
	if (!server)
		return;

	Logger().Warn("CDedicatedServerManager::RemoveServer: removing %s:%d socket=%s poolBefore=%d\n",
		ip_to_string(server->GetIP()).c_str(), server->GetPort(), socket->GetIP().c_str(), m_vServerPools.size());

	IRoom* room = server->GetRoom();
	if (room)
	{
		room->SetServer(NULL);

		if (room->GetGameMatch())
			room->EndGame(true);
	}

	delete server;
	m_vServerPools.erase(remove(m_vServerPools.begin(), m_vServerPools.end(), server), m_vServerPools.end());
}

/**
 * Connects dedi server to another master server
 */
void CDedicatedServerManager::TransferServer(IExtendedSocket* socket, const std::string& ipAddress, int port)
{
	CDedicatedServer* server = GetServerBySocket(socket);
	if (!server)
		return;

	g_PacketManager.SendHostServerTransfer(socket, ipAddress, port);
}

std::vector<CDedicatedServer*>& CDedicatedServerManager::GetServers()
{
	return m_vServerPools;
}
