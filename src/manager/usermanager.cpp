#include "usermanager.h"
#include "channelmanager.h"
#include "userdatabase.h"
#include "itemmanager.h"
#include "packetmanager.h"
#include "shopmanager.h"
#include "minigamemanager.h"
#include "luckyitemmanager.h"
#include "clanmanager.h"
#include "questmanager.h"

#include "user/userinventoryitem.h"

#include "csvtable.h"
#include "serverinstance.h"
#include "serverconfig.h"
#include "main.h"
#include "common/utils.h"

#include <cstdlib>
#include <algorithm>
#include <thread>
#include <climits>

using namespace std;

#define SUPPORTED_CLIENT_BUILD "06.07.26"
#define ENABLE_LOCAL_BOOTSTRAP 0
#define LATEST_LOGIN_BOOTSTRAP_PHASE 2

#define ZOMBIE_WAR_WEAPON_LIST_VERSION 1
#define RANDOM_WEAPON_LIST_VERSION 1

CUserManager g_UserManager;

static bool ReplayLatestLoginSample(IExtendedSocket* socket);

static void QueueDelayedLocalHandshake(IExtendedSocket* socket)
{
	std::thread([socket]()
	{
		SleepMS(2500);
		g_Events.AddEventFunction([socket]()
		{
			if (!g_pServerInstance->IsServerActive())
				return;

			Logger().Info("Delayed local version reply after client UI startup\n");
			g_PacketManager.SendVersion(socket, 0);
		});
	}).detach();
}

CUserManager::CUserManager() : CBaseManager("UserManager", true)
{
}

CUserManager::~CUserManager()
{
}

bool CUserManager::Init()
{
	for (size_t i = 0; i < g_pServerConfig->defUser.defaultItems.size(); i++)
		m_DefaultItems.push_back(CUserInventoryItem(i, g_pServerConfig->defUser.defaultItems[i], 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, {}, 0, 0, 2));

	if (!LoadZombieWarWeaponList())
		return false;

	if (!LoadRandomWeaponList())
		return false;

	return true;
}

void CUserManager::Shutdown()
{
	m_DefaultItems.clear();
	m_ZombieWarWeaponList.clear();
	m_RandomWeaponList.clear();
}

bool CUserManager::LoadZombieWarWeaponList()
{
	try
	{
		ifstream f("ZombieWarWeaponList.json");
		ordered_json cfg = ordered_json::parse(f, nullptr, false, true);

		if (cfg.is_discarded())
		{
			Logger().Fatal("CUserManager::LoadZombieWarWeaponList: couldn't load ZombieWarWeaponList.json.\n");
			return false;
		}

		int version = cfg.value("Version", 0);
		if (version != ZOMBIE_WAR_WEAPON_LIST_VERSION)
		{
			Logger().Fatal("CUserManager::LoadZombieWarWeaponList: %d != ZOMBIE_WAR_WEAPON_LIST_VERSION(%d)\n", version, ZOMBIE_WAR_WEAPON_LIST_VERSION);
			return false;
		}

		m_ZombieWarWeaponList = cfg.value("Weapons", m_ZombieWarWeaponList);
	}
	catch (exception& ex)
	{
		Logger().Fatal("CUserManager::LoadRandomWeaponList: an error occured while parsing RandomWeaponList.json: %s\n", ex.what());
		return false;
	}

	return true;
}

bool CUserManager::LoadRandomWeaponList()
{
	try
	{
		ifstream f("RandomWeaponList.json");
		ordered_json cfg = ordered_json::parse(f, nullptr, false, true);

		if (cfg.is_discarded())
		{
			Logger().Fatal("CUserManager::LoadRandomWeaponList: couldn't load RandomWeaponList.json.\n");
			return false;
		}

		int version = cfg.value("Version", 0);
		if (version != RANDOM_WEAPON_LIST_VERSION)
		{
			Logger().Fatal("CUserManager::LoadRandomWeaponList: %d != RANDOM_WEAPON_LIST_VERSION(%d)\n", version, RANDOM_WEAPON_LIST_VERSION);
			return false;
		}

		for (auto& iRandomWeapon : cfg.items())
		{
			json jRandomWeapon = iRandomWeapon.value();
			if (!jRandomWeapon.is_object())
				continue;

			RandomWeapon randomWeapon;
			randomWeapon.itemID = stoi(iRandomWeapon.key());

			for (auto& iModeFlag : jRandomWeapon.items())
			{
				json jModeFlag = iModeFlag.value();
				if (!jModeFlag.is_object())
					continue;

				RandomWeaponModeFlag randomWeaponModeFlag;
				randomWeaponModeFlag.modeFlag = stoi(iModeFlag.key());
				randomWeaponModeFlag.dropRate = jModeFlag.value("DropRate", 0);
				randomWeaponModeFlag.enhanceProbability = jModeFlag.value("EnhanceProbability", 0);

				randomWeapon.modeFlags.push_back(randomWeaponModeFlag);
			}

			m_RandomWeaponList.push_back(randomWeapon);
		}
	}
	catch (exception& ex)
	{
		Logger().Fatal("CUserManager::LoadRandomWeaponList: an error occured while parsing RandomWeaponList.json: %s\n", ex.what());
		return false;
	}

	return true;
}

bool CUserManager::OnLoginPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	Logger().Info("Client (%s) sent login packet\n", socket->GetIP().c_str());

	string steamID = msg->ReadString();
	vector<unsigned char> hwid;
	Buffer& data = msg->GetData();
	const vector<unsigned char>& raw = data.getBuffer();
	unsigned long long afterSteamID = data.getReadOffset();
	unsigned int passportSize = 0;
	if (afterSteamID + 2 <= raw.size())
		passportSize = (unsigned int)raw[afterSteamID] | ((unsigned int)raw[afterSteamID + 1] << 8);
	unsigned long long thirdStart = afterSteamID + 2 + passportSize;
	unsigned long long thirdEnd = thirdStart;
	while (thirdEnd < raw.size() && raw[thirdEnd] != '\0')
		thirdEnd++;

	if (passportSize > 0 && thirdEnd < raw.size() && thirdEnd + 1 + 16 + 8 <= raw.size())
	{
		int size = msg->ReadUInt16();
		vector<unsigned char> passport = msg->ReadArray(size);
		string locale = msg->ReadString();
		hwid = msg->ReadArray(16);
		int pcBang = msg->ReadUInt32(); // PCBang (PC Cafe) identification by running executable
		int ip = msg->ReadUInt32();

		Logger().Info("Client (%s) login packet parsed as latest layout: steamID='%s', passportSize=%d, locale='%s', pcBang=%d, ip=%s\n",
			socket->GetIP().c_str(), steamID.c_str(), size, locale.c_str(), pcBang, ip_to_string(ip).c_str());
	}
	else
	{
		int size = msg->ReadUInt16();
		vector<unsigned char> authSessionTicket = msg->ReadArray(size); // AuthSessionTicket - Reference: https://partner.steamgames.com/doc/features/auth
		hwid = msg->ReadArray(16);
		int pcBang = msg->ReadUInt32(); // PCBang (PC Cafe) identification by running executable
		int ip = msg->ReadUInt32();
		string locale = msg->ReadString();

		Logger().Info("Client (%s) login packet parsed as legacy layout: steamID='%s', ticketSize=%d, locale='%s', pcBang=%d, ip=%s\n",
			socket->GetIP().c_str(), steamID.c_str(), size, locale.c_str(), pcBang, ip_to_string(ip).c_str());
	}

	socket->SetHWID(hwid);

	if (g_UserDatabase.IsHWIDBanned(hwid))
	{
		Logger().Info("Client (%s) disconnected from the server due to banned HWID\n", socket->GetIP().c_str());

		g_pServerInstance->DisconnectClient(socket);

		return true;
	}

	IUser* user = GetUserBySocket(socket);
	if (user != NULL && user->IsCharacterExists())
	{
		CUserCharacter character = user->GetCharacter(UFLAG_LOW_ALL, UFLAG_HIGH_ALL);

		g_ItemManager.OnUserLogin(user);
		g_ClanManager.OnUserLogin(user);
		g_QuestManager.OnUserLogin(user);

		Logger().Info("Continuing local login bootstrap after client Login(3) packet\n");
		if (!ReplayLatestLoginSample(socket))
			SendLoginPacket(user, character, true, false);
		return true;
	}

	BootstrapLocalUser(socket, true);

	return true;
}

bool CUserManager::OnUdpPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case 0:
		Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received type 0\n"));
		break;
	case 1:
	{
		int unk = msg->ReadUInt32();

		Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received type 1: %d\n"), unk);
		break;
	}
	case 2:
	{
		if (msg->GetLength() == 11 && msg->CanReadBytes(9))
		{
			int portID = msg->ReadUInt8();
			uint32_t localAddr = msg->ReadUInt32(true);
			int host = msg->ReadUInt8();
			int unk = 0;
			unk |= msg->ReadUInt8();
			unk |= msg->ReadUInt8() << 8;
			unk |= msg->ReadUInt8() << 16;
			UserNetworkConfig_s network = user->GetNetworkConfig();

			Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received latest type 2: portID: %d, localAddr: %s, host: %d, unk: %d\n"),
				portID, ip_to_string(localAddr).c_str(), host, unk);

			if (portID == 0)
				g_PacketManager.SendUDPHostData(socket, true, user->GetID(), network.m_szExternalIpAddress, network.m_nExternalServerPort);
			else
				g_PacketManager.SendUDPHostData(socket, false, user->GetID(), network.m_szExternalIpAddress, network.m_nExternalClientPort);

			if (user->GetCurrentChannel() == NULL)
			{
				SleepMS(300);
				Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: skipping legacy GameMatch bootstrap for latest client\n"));
				g_ChannelManager.JoinChannel(user, g_ChannelManager.channelServers[0]->GetID(), g_ChannelManager.channelServers[0]->GetChannels()[0]->GetID(), false);
			}
			break;
		}

		int subType = msg->ReadUInt32();
		switch (subType)
		{
		case 0:
		{
			int unk = msg->ReadInt32();
			int unk2 = msg->ReadUInt16();
			int unk3 = msg->ReadUInt16();

			Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received type 2-0: unk: %d, unk2: %d, unk3: %d\n"), unk, unk2, unk3);
			break;
		}
		case 1:
		{
			int unk = msg->ReadInt32();
			int unk2 = msg->ReadUInt16();
			int unk3 = msg->ReadUInt16();

			Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received type 2-1: unk: %d, unk2: %d, unk3: %d\n"), unk, unk2, unk3);
			break;
		}
		case 2:
		{
			int unk = msg->ReadInt32();
			int unk2 = msg->ReadUInt16();

			Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received type 2-2: unk: %d, unk2: %d\n"), unk, unk2);
			break;
		}
		case 3:
		{
			int unk = msg->ReadInt32();
			int unk2 = msg->ReadUInt16();

			Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received type 2-3: unk: %d, unk2: %d\n"), unk, unk2);
			break;
		}
		case 4:
		{
			int unk = msg->ReadInt32();

			Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received type 2-4: unk: %d\n"), unk);
			break;
		}
		case 5:
		{
			int unk = msg->ReadInt32();
			int unk2 = msg->ReadUInt16();

			Logger().Info(OBFUSCATE("CUserManager::OnUdpPacket: received type 2-5: unk: %d, unk2: %d\n"), unk, unk2);
			break;
		}
		}
		break;
	}
	default:
		Logger().Info(OBFUSCATE("[User '%s'] CUserManager::OnUdpPacket: unknown request %d\n"), user->GetLogName(), type);
		break;
	}

	return true;
}

bool CUserManager::OnOptionPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case 0: // save cfg
	{
		CUserCharacterExtended character(EXT_UFLAG_CONFIG);
		character.config = msg->ReadArray(msg->ReadUInt16());

		g_UserDatabase.UpdateCharacterExtended(user->GetID(), character);
		break;
	}
	case 2: // called when joining the game
		break;
	default:
		Logger().Info(OBFUSCATE("[User '%s'] CUserManager::OnOptionPacket: unknown request %d\n"), user->GetLogName(), type);
		break;
	}

	return true;
}

bool CUserManager::OnVersionPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	if (!GetUserBySocket(socket))
	{
		int launcherVersion = msg->ReadUInt8();  // const 67
		int gameVersion = msg->ReadUInt16(); // const 26
		int clientBuildTimestamp = msg->ReadUInt32();
		int clientNARCRC = msg->ReadUInt32();

		if (g_pServerConfig->checkClientBuild && ((clientBuildTimestamp != g_pServerConfig->allowedClientTimestamp) || (launcherVersion != g_pServerConfig->allowedLauncherVersion)))
		{
			Logger().Info(OBFUSCATE("CUserManager::OnVersionPacket: user joined with outdated client build, rejecting...\n"));

			g_PacketManager.SendUMsgNoticeMsgBoxToUuid(socket, OBFUSCATE("You cannot log on due to invalid client version.\nPatch the client and try it again."));

#ifndef PUBLIC_RELEASE
			Logger().Warn("[SuspectNotice] detected suspect user '%s', reason: 1, %s\n", socket->GetIP().c_str(), launcherVersion != g_pServerConfig->allowedLauncherVersion ? "launcher version mismatch" : "client libraries timestamp mismatch");

			/// @todo get HWID by ip
			//g_UserDatabase.SuspectAddAction(socket->GetIP(), 1);
#endif
			g_pServerInstance->DisconnectClient(socket);
			return true;
		}

		GuestData_s& data = socket->GetGuestData();
		if (!data.isGuest)
		{
			if (ENABLE_LOCAL_BOOTSTRAP || (launcherVersion == 67 && gameVersion == 26 && socket->GetIP() == "127.0.0.1"))
			{
				Logger().Info("Latest local client detected after version packet, starting local login bootstrap\n");
				g_PacketManager.SendVersion(socket, 0);
				BootstrapLocalUser(socket, true);
			}
			else
			{
				g_PacketManager.SendVersion(socket, 0);
			}
		}
		data.isGuest = true;
		data.launcherVersion = launcherVersion;

		static char dateStr[30];
		time_t t = clientBuildTimestamp;
		tm* unk3_date = localtime(&t);
		strftime(dateStr, sizeof(dateStr), "%d.%m.%y", unk3_date);

		if (strcmp(dateStr, SUPPORTED_CLIENT_BUILD))
			Logger().Warn("CUserManager::OnVersionPacket: the server may not support the client build: %s (supported build: %s)\n", dateStr, SUPPORTED_CLIENT_BUILD);
	}

	return true;
}

bool CUserManager::OnFavoritePacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case FavoritePacketType::SetBuyMenu:
		return OnFavoriteSetBuyMenu(msg, user);
	case FavoritePacketType::SetFastBuy:
		return OnFavoriteSetFastBuy(msg, user); // obsolete
	case FavoritePacketType::SetLoadout:
		return OnFavoriteSetLoadout(msg, user);
	case FavoritePacketType::SetBookmark:
		return OnFavoriteSetBookmark(msg, user);
	default:
		Logger().Warn("CUserManager::OnFavoritePacket: unknown request %d\n", type);
		break;
	};

	return false;
}

bool CUserManager::OnFavoriteSetFastBuy(CReceivePacket* msg, IUser* user)
{
	vector<int> fastBuyItems;
	int fastBuySlot = msg->ReadInt8();
	string fastBuyName = msg->ReadString();

	if (fastBuySlot >= FASTBUY_COUNT)
	{
		Logger().Warn("OnFavoriteSetBookmark: invalid fastBuySlot %d\n", fastBuySlot);
		return false;
	}

	for (int i = 0; i < FASTBUY_SLOT_COUNT; i++)
		fastBuyItems.push_back(msg->ReadUInt16());

	g_UserDatabase.UpdateFastBuy(user->GetID(), fastBuySlot, fastBuyName, fastBuyItems);

	return true;
}

bool CUserManager::OnFavoriteSetBookmark(CReceivePacket* msg, IUser* user)
{
	int bookmarkSlot = msg->ReadUInt8();
	int itemID = msg->ReadUInt16();

	if (bookmarkSlot >= BOOKMARK_COUNT)
	{
		Logger().Warn("OnFavoriteSetBookmark: invalid bookmarkSlot %d\n", bookmarkSlot);
		return false;
	}

	g_UserDatabase.UpdateBookmark(user->GetID(), bookmarkSlot, itemID);

	return true;
}

void CUserManager::SendUserInventory(IUser* user)
{
	// Latest hw.dll expects a different inventory/default-item bootstrap layout.
	// Do not push the legacy snapshots during login; item packets should be
	// re-enabled only after the current 152/154 layouts are mapped.
	return;

	vector<CUserInventoryItem> items;
	g_UserDatabase.GetInventoryItems(user->GetID(), items);

	g_PacketManager.SendDefaultItems(user->GetExtendedSocket(), m_DefaultItems);
	g_PacketManager.SendInventoryAdd(user->GetExtendedSocket(), items);
}

bool CUserManager::OnFavoriteSetLoadout(CReceivePacket* msg, IUser* user)
{
	int loadoutID = 0;
	int itemID = 0;

	int loadoutType = msg->ReadUInt8(); // нада как-то узнать что там
	if (loadoutType == 1) // switch current loadout
		loadoutID = msg->ReadUInt8();
	else
		itemID = msg->ReadUInt16();

	CUserCharacterExtended character = user->GetCharacterExtended(EXT_UFLAG_CURLOADOUT);

	if (loadoutType == 1)
	{
		if (loadoutID >= LOADOUT_COUNT)
		{
			Logger().Info(OBFUSCATE("CUserManager::OnFavoriteSetLoadout: invalid loadout %d\n"), loadoutID);
			return false;
		}

		// change current loadout
		character.flag = EXT_UFLAG_CURLOADOUT;
		character.curLoadout = loadoutID;
		g_UserDatabase.UpdateCharacterExtended(user->GetID(), character);
		return true;
	}
	else if (loadoutType == (character.curLoadout + 1) * 10 ||
		loadoutType == (character.curLoadout + 1) * 10 + 1 ||
		loadoutType == (character.curLoadout + 1) * 10 + 2 ||
		loadoutType == (character.curLoadout + 1) * 10 + 3)
	{
		int slot = loadoutType - (character.curLoadout + 1) * 10;
		
		if (character.curLoadout >= LOADOUT_COUNT)
		{
			Logger().Info(OBFUSCATE("CUserManager::OnFavoriteSetLoadout: invalid loadout %d\n"), character.curLoadout);
			return false;
		}

		if (slot >= LOADOUT_SLOT_COUNT)
		{
			Logger().Info(OBFUSCATE("CUserManager::OnFavoriteSetLoadout: invalid slot %d\n"), slot);
			return false;
		}

		vector<CUserInventoryItem> items;
		if (!g_UserDatabase.GetInventoryItemsByID(user->GetID(), itemID, items))
			return false;

		int category = g_pItemTable->GetCell<int>("Category", to_string(itemID));

		if (category != 11 && (category < 1 || category > 6))
			return false;

		g_UserDatabase.UpdateLoadout(user->GetID(), character.curLoadout, slot, itemID);
		return true;
	}
	else if (loadoutType == 0)
	{
		vector<CUserInventoryItem> items;
		if (!g_UserDatabase.GetInventoryItemsByID(user->GetID(), itemID, items))
			return false;

		int category = g_pItemTable->GetCell<int>("Category", to_string(itemID));

		if (category != 7)
			return false;

		// change bg character...
		character.flag = EXT_UFLAG_CHARACTERID;
		character.characterID = itemID;
		g_UserDatabase.UpdateCharacterExtended(user->GetID(), character);
	}
	else
	{
		Logger().Warn("CUserManager::OnFavoriteSetLoadout: unknown loadout type: %d\n", loadoutType);
	}

	return true;
}

bool CUserManager::OnFavoriteSetBuyMenu(CReceivePacket* msg, IUser* user)
{
	int subMenuID = msg->ReadUInt8();
	int subMenuSlot = msg->ReadUInt8();
	int itemID = msg->ReadUInt16();

	if (subMenuID >= BUYMENU_COUNT)
	{
		Logger().Info(OBFUSCATE("CUserManager::OnFavoriteSetBuyMenu: invalid subMenuId %d\n"), subMenuID);
		return false;
	}

	if (subMenuSlot >= BUYMENU_SLOT_COUNT)
	{
		Logger().Info(OBFUSCATE("CUserManager::OnFavoriteSetBuyMenu: invalid subMenuSlot %d\n"), subMenuSlot);
		return false;
	}

	g_UserDatabase.UpdateBuyMenu(user->GetID(), subMenuID, subMenuSlot, itemID);

	Logger().Info(OBFUSCATE("User '%d' updated buy menu, %d, %d, %d\n"), user->GetID(), subMenuID, subMenuSlot, itemID);

	return true;
}

int CUserManager::ChangeUserNickname(IUser* user, const string& newNickname, bool createCharacter)
{
	if (newNickname.size() < 4)
		return -1;
	else if (newNickname.size() > 16)
		return -2;
	else if (g_UserDatabase.IsUserExists(newNickname, false))
		return -3;
	else if (newNickname.find_first_not_of((const char*)OBFUSCATE("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890")) != string::npos)
		return -4;
	else if (findCaseInsensitive(newNickname, g_pServerConfig->nameBlacklist))
		return -4;

	if (createCharacter)
	{
		if (!user->CreateCharacter(newNickname))
			return 0;

		// give	a user pseudo default items
		vector<RewardItem> rewardItems;
		for (auto itemID : g_pServerConfig->defUser.pseudoDefaultItems)
		{
			RewardItem rewardItem;
			rewardItem.itemID = itemID;
			rewardItem.count = 1;
			rewardItem.duration = 0;
			rewardItem.lockStatus = 2;
			rewardItems.push_back(rewardItem);
		}

		// give a user lvl box 1
		RewardItem rewardItem;
		rewardItem.itemID = 1001;
		rewardItem.count = 1;
		rewardItem.duration = 0;
		rewardItem.lockStatus = 0;
		rewardItems.push_back(rewardItem);

		g_ItemManager.AddItems(user->GetID(), user, rewardItems);
	}
	else
	{
		// set new nickname
		user->UpdateGameName(newNickname);
	}

	return 1;
}

vector<CUserInventoryItem>& CUserManager::GetDefaultInventoryItems()
{
	return m_DefaultItems;
}

void CUserManager::SendGuestUserPacket(IExtendedSocket* socket)
{
	g_PacketManager.SendUMsgNoticeMessageInChat(socket, OBFUSCATE("Welcome to the CSN:S server. Enter /login <username> <password> to login to your account."));
	g_PacketManager.SendUMsgNoticeMessageInChat(socket, OBFUSCATE("If you don't have an account enter /register <username> <password>"));
	g_PacketManager.SendUMsgNoticeMessageInChat(socket, OBFUSCATE("Server developers: Jusic, Hardee, NekoMeow, Smilex_Gamer, xRiseless. Our Discord: https://discord.gg/EvUAY6D"));
}

bool CUserManager::BootstrapLocalUser(IExtendedSocket* socket, bool sendLoginReply)
{
	if (GetUserBySocket(socket))
		return true;

	const string localUserName = "localuser";
	const string localPassword = "localpass1";
	const string localCharacterName = "LocalPlayer";

	int loginResult = LoginUser(socket, localUserName, localPassword, sendLoginReply, false);
	if (loginResult == LOGIN_NO_SUCH_USER)
	{
		int registerResult = RegisterUser(socket, localUserName, localPassword);
		Logger().Info("Local auto-register result: %d\n", registerResult);
		loginResult = LoginUser(socket, localUserName, localPassword, sendLoginReply, false);
	}

	if (loginResult != LOGIN_OK)
	{
		Logger().Warn("Local bootstrap login failed (code: %d)\n", loginResult);
		return false;
	}

	IUser* user = GetUserBySocket(socket);
	if (user != NULL && !user->IsCharacterExists())
	{
		int replyCode = ChangeUserNickname(user, localCharacterName, true);
		if (replyCode != 1)
		{
			Logger().Warn("Local bootstrap character create failed (code: %d)\n", replyCode);
			return false;
		}

		if (sendLoginReply)
		{
			Logger().Info("Local bootstrap character created; deferring login packet until crypt acknowledgement\n");
			return true;
		}

		CUserCharacter character = user->GetCharacter(UFLAG_LOW_ALL, UFLAG_HIGH_ALL);

		g_ItemManager.OnUserLogin(user);
		g_ClanManager.OnUserLogin(user);
		g_QuestManager.OnUserLogin(user);

		SendLoginPacket(user, character);
	}

	return true;
}

bool CUserManager::BootstrapLocalUserAfterMetadata(IExtendedSocket* socket)
{
	if (GetUserBySocket(socket))
		return true;

	const string localUserName = "localuser";
	const string localPassword = "localpass1";
	const string localCharacterName = "LocalPlayer";

	UserBan ban = {};
	int userID = g_UserDatabase.Login(localUserName, localPassword, socket, ban, NULL);
	if (userID == LOGIN_NO_SUCH_USER)
	{
		int registerResult = RegisterUser(socket, localUserName, localPassword);
		Logger().Info("Local host bootstrap auto-register result: %d\n", registerResult);
		userID = g_UserDatabase.Login(localUserName, localPassword, socket, ban, NULL);
	}

	if (userID <= 0)
	{
		Logger().Warn("Local host bootstrap login failed (code: %d)\n", userID);
		return false;
	}

	if (GetUserById(userID))
	{
		Logger().Warn("Local host bootstrap user is already logged in (uid: %d)\n", userID);
		return false;
	}

	IUser* user = AddUser(socket, userID, localUserName);
	if (user == NULL)
	{
		Logger().Warn("Local host bootstrap failed: server is full\n");
		return false;
	}

	Logger().Info("User logged in on host socket (IP: %s, UID: %d, Username: %s)\n",
		user->GetNetworkConfig().m_szExternalIpAddress.c_str(), userID, localUserName.c_str());

	if (!user->IsCharacterExists())
	{
		int replyCode = ChangeUserNickname(user, localCharacterName, true);
		if (replyCode != 1)
		{
			Logger().Warn("Local host bootstrap character create failed (code: %d)\n", replyCode);
			return false;
		}
	}

	CUserCharacter character = user->GetCharacter(UFLAG_LOW_ALL, UFLAG_HIGH_ALL);

	g_ItemManager.OnUserLogin(user);
	g_ClanManager.OnUserLogin(user);
	g_QuestManager.OnUserLogin(user);

	SendLoginPacket(user, character, false);
	return true;
}

void CUserManager::SendLoginPacket(IUser* user, const CUserCharacter& character, bool includeMetadata, bool sendStartPackets)
{
	IExtendedSocket* socket = user->GetExtendedSocket();
	(void)character;

	const int loginLowFlags =
		UFLAG_LOW_GAMENAME |
		UFLAG_LOW_UNK27;
	const int loginHighFlags = 0;
	CUserCharacter loginCharacter = user->GetCharacter(loginLowFlags, loginHighFlags);

	if (sendStartPackets)
	{
		g_PacketManager.SendUserStartStep(socket, 0);
		g_PacketManager.SendUserStart(socket, user->GetID(), user->GetUsername(), loginCharacter.gameName, includeMetadata);
	}

	if (includeMetadata)
		SendMetadata(socket);
	else
	{
		Logger().Info("Latest host bootstrap: sending minimal UserUpdateInfo(157) for latest full-user flags\n");
		g_PacketManager.SendUserUpdateInfoMinimal(socket, user);
	}

	CUserCharacterExtended characterExtended = user->GetCharacterExtended(EXT_UFLAG_CONFIG | EXT_UFLAG_BANSETTINGS);
	if (characterExtended.config.size())
		g_PacketManager.SendOption(socket, characterExtended.config);

	vector<string> banList;
	g_UserDatabase.GetBanList(user->GetID(), banList);
	if (!banList.empty())
		g_PacketManager.SendBanList(socket, banList);

	// Latest hw.dll does not need the legacy Ban(74) settings during login.
	// Sending it keeps the client in the same timeout path as the old bootstrap.

	if (LATEST_LOGIN_BOOTSTRAP_PHASE <= 1)
	{
		Logger().Info("Latest login bootstrap phase 1: minimal user/lobby packets only\n");
		g_ChannelManager.JoinChannel(user, g_ChannelManager.channelServers[0]->GetID(), g_ChannelManager.channelServers[0]->GetChannels()[0]->GetID(), false);
		return;
	}

	if (includeMetadata)
	{
		Logger().Info("Skipping legacy full UserUpdateInfo(157) for latest client\n");
	}

	if (!includeMetadata)
	{
		Logger().Info("Latest host bootstrap phase: user start/update only\n");
		return;
	}

	Logger().Info("Skipping legacy GameMatch(99) bootstrap packets for latest client\n");

	if (g_pServerConfig->mainMenuSkinEvent > 0)
		g_PacketManager.SendEventMainMenuSkin(socket, g_pServerConfig->mainMenuSkinEvent);

	g_PacketManager.SendEventUnk(socket);

	g_PacketManager.SendEventAdd(socket, g_pServerConfig->activeMiniGamesFlag);

	if (g_pServerConfig->activeMiniGamesFlag & kEventFlag_WeaponRelease)
		g_MiniGameManager.SendWeaponReleaseUpdate(user);

	SendUserInventory(user);
	SendUserLoadout(user);

	// Latest hw.dll requests shop panels explicitly. Avoid pushing the legacy
	// full shop payload during login; it is parsed by the latest client with a
	// different layout.

	g_ChannelManager.JoinChannel(user, g_ChannelManager.channelServers[0]->GetID(), g_ChannelManager.channelServers[0]->GetChannels()[0]->GetID(), false);

	// Optional post-login packets (notices, surveys, league, update-info and
	// voxel URLs) use older layouts and can terminate the latest client during
	// startup. The latest client also closes after the legacy final start-step,
	// so leave startup in the post-login loading state until that packet is mapped.
}

void CUserManager::SendMetadata(IExtendedSocket* socket)
{
	uint64_t flag = g_pServerConfig->metadataToSend;

	// These legacy flags either collide with latest metadata IDs or do not have
	// a verified latest hw.dll layout yet. Keep the live ZIP metadata enabled by
	// default; only suppress layouts known to be unsafe for the current client.
	flag &= ~kMetadataFlag_WeaponPaints;
	flag &= ~kMetadataFlag_Unk3;
	flag &= ~kMetadataFlag_ZombieWarWeaponList;
	flag &= ~kMetadataFlag_Encyclopedia;
	flag &= ~kMetadataFlag_ReinforceItemsExp;
	flag &= ~kMetadataFlag_Unk31;
	flag &= ~kMetadataFlag_Unk43;
	flag &= ~kMetadataFlag_Unk49;
	flag &= ~kMetadataFlag_RandomWeaponList;
	flag &= ~kMetadataFlag_Unk54;
	flag &= ~kMetadataFlag_Unk55;

	const bool legacyMetadataIDs = g_PacketManager.UsesLegacyMetadataIDs();
	const char* minMetadataEnv = getenv("CSNZ_ASSET_MIN_METADATA");
	if (minMetadataEnv && minMetadataEnv[0] == '1')
	{
		flag = kMetadataFlag_MapList | kMetadataFlag_MatchOption | kMetadataFlag_GameModeList;
		Logger().Info("Using asset-safe minimal metadata set\n");
	}

	const char* metadataFilterEnv = getenv("CSNZ_METADATA_FILTER");
	if (metadataFilterEnv && metadataFilterEnv[0])
	{
		string filter = ",";
		filter += metadataFilterEnv;
		filter += ",";
		auto hasMetadataID = [&filter](int id)
		{
			return filter.find("," + to_string(id) + ",") != string::npos;
		};

		uint64_t filteredFlag = 0;
		if (hasMetadataID(kPacket_Metadata_MapList)) filteredFlag |= kMetadataFlag_MapList;
		if (hasMetadataID(kPacket_Metadata_ModeList)) filteredFlag |= kMetadataFlag_ModeList;
		if (hasMetadataID(kPacket_Metadata_MatchOption)) filteredFlag |= kMetadataFlag_MatchOption;
		if (hasMetadataID(kPacket_Metadata_WeaponAuction)) filteredFlag |= kMetadataFlag_WeaponAuction;
		if (hasMetadataID(kPacket_Metadata_WeaponParts)) filteredFlag |= kMetadataFlag_WeaponParts;
		if (hasMetadataID(kPacket_Metadata_MileageShop)) filteredFlag |= kMetadataFlag_MileageShop;
		if (hasMetadataID(kPacket_Metadata_GameModeList)) filteredFlag |= kMetadataFlag_GameModeList;
		if (hasMetadataID(kPacket_Metadata_ProgressUnlock)) filteredFlag |= kMetadataFlag_ProgressUnlock;
		if (hasMetadataID(kPacket_Metadata_ReinforceMaxLvl)) filteredFlag |= kMetadataFlag_ReinforceMaxLvl;
		if (hasMetadataID(kPacket_Metadata_ReinforceMaxEXP)) filteredFlag |= kMetadataFlag_ReinforceMaxEXP;
		if (hasMetadataID(kPacket_Metadata_Item)) filteredFlag |= kMetadataFlag_Item;
		if (hasMetadataID(kPacket_Metadata_HonorMoneyShop)) filteredFlag |= kMetadataFlag_HonorMoneyShop;
		if (hasMetadataID(kPacket_Metadata_ItemExpireTime)) filteredFlag |= kMetadataFlag_ItemExpireTime;
		if (hasMetadataID(kPacket_Metadata_ScenarioTX_Common)) filteredFlag |= kMetadataFlag_ScenarioTX_Common;
		if (hasMetadataID(kPacket_Metadata_ScenarioTX_Dedi)) filteredFlag |= kMetadataFlag_ScenarioTX_Dedi;
		if (hasMetadataID(kPacket_Metadata_ShopItemList_Dedi)) filteredFlag |= kMetadataFlag_ShopItemList_Dedi;
		if (hasMetadataID(kPacket_Metadata_PPSystem)) filteredFlag |= kMetadataFlag_PPSystem;
		if (hasMetadataID(kPacket_Metadata_ZBCompetitive)) filteredFlag |= kMetadataFlag_ZBCompetitive;
		if (hasMetadataID(kPacket_Metadata_ModeEvent)) filteredFlag |= kMetadataFlag_ModeEvent;
		if (hasMetadataID(kPacket_Metadata_EventShop)) filteredFlag |= kMetadataFlag_EventShop;
		if (hasMetadataID(kPacket_Metadata_FamilyTotalWarMap)) filteredFlag |= kMetadataFlag_FamilyTotalWarMap;
		if (hasMetadataID(kPacket_Metadata_FamilyTotalWar)) filteredFlag |= kMetadataFlag_FamilyTotalWar;
		flag = filteredFlag;
		Logger().Info("CSNZ_METADATA_FILTER active: %s\n", metadataFilterEnv);
	}

	auto logMetadata = [](int id, const char* name)
	{
		Logger().Info("TX metadata: id=%d, name=%s\n", g_PacketManager.GetMetadataWireID(id), name);
	};

	// Keep the host-server metadata stream in the latest hw.dll table order.
	if (flag & kMetadataFlag_MapList)
	{
		logMetadata(kPacket_Metadata_MapList, "resource/MapList.csv");
		g_PacketManager.SendMetadataMaplist(socket);
	}
	if (flag & kMetadataFlag_ModeList)
	{
		logMetadata(kPacket_Metadata_ModeList, "resource/MapModeV2/ModeList.csv");
		g_PacketManager.SendMetadataModelist(socket);
	}
	if (flag & kMetadataFlag_MatchOption)
	{
		logMetadata(kPacket_Metadata_MatchOption, "Matching.csv");
		g_PacketManager.SendMetadataMatchOption(socket);
	}
	if (flag & kMetadataFlag_WeaponParts)
	{
		logMetadata(kPacket_Metadata_WeaponParts, "weaponparts.csv");
		g_PacketManager.SendMetadataWeaponParts(socket);
	}
	if (flag & kMetadataFlag_WeaponAuction)
	{
		logMetadata(kPacket_Metadata_WeaponAuction, "WeaponAuction");
		g_PacketManager.SendMetadataWeaponAuction(socket);
	}
	if (flag & kMetadataFlag_MileageShop)
	{
		logMetadata(kPacket_Metadata_MileageShop, "MileageShop.csv");
		g_PacketManager.SendMetadataMileageShop(socket);
	}
	if (flag & kMetadataFlag_GameModeList)
	{
		logMetadata(kPacket_Metadata_GameModeList, "resource/GameModeList.csv");
		g_PacketManager.SendMetadataGameModeList(socket);
	}
	if (flag & kMetadataFlag_ProgressUnlock)
	{
		logMetadata(kPacket_Metadata_ProgressUnlock, "resource/zombiez/progress_unlock.csv");
		g_PacketManager.SendMetadataProgressUnlock(socket);
	}
	if (flag & kMetadataFlag_ReinforceMaxLvl)
	{
		logMetadata(kPacket_Metadata_ReinforceMaxLvl, "ReinforceMaxLv.csv");
		g_PacketManager.SendMetadataReinforceMaxLvl(socket);
	}
	if (flag & kMetadataFlag_ReinforceMaxEXP)
	{
		logMetadata(kPacket_Metadata_ReinforceMaxEXP, "ReinforceMaxEXP.csv");
		g_PacketManager.SendMetadataReinforceMaxEXP(socket);
	}
	g_PacketManager.SendMetadataHash(socket);
	if (flag & kMetadataFlag_ReinforceItemsExp)
		g_PacketManager.SendMetadataReinforceItemsExp(socket);
	if (flag & kMetadataFlag_Item)
	{
		logMetadata(kPacket_Metadata_Item, "resource/item.csv");
		g_PacketManager.SendMetadataItem(socket);
		if (legacyMetadataIDs)
		{
			logMetadata(kPacket_Metadata_VoxelList, "voxel/voxel_list.csv");
			g_PacketManager.SendMetadataVoxelList(socket);
		}
	}
	if (flag & kMetadataFlag_CodisData)
	{
		logMetadata(kPacket_Metadata_CodisData, "resource/codis/codisdata.cso");
		g_PacketManager.SendMetadataCodisData(socket);
	}
	if (flag & kMetadataFlag_HonorMoneyShop)
	{
		logMetadata(kPacket_Metadata_HonorMoneyShop, "HonorShop.csv");
		g_PacketManager.SendMetadataHonorMoneyShop(socket);
	}
	if (flag & kMetadataFlag_ItemExpireTime)
	{
		logMetadata(kPacket_Metadata_ItemExpireTime, "resource/ItemExpireTime.csv");
		g_PacketManager.SendMetadataItemExpireTime(socket);
	}
	if (flag & kMetadataFlag_ScenarioTX_Common)
	{
		logMetadata(kPacket_Metadata_ScenarioTX_Common, "resource/scenariotx/scenariotx_common.json");
		g_PacketManager.SendMetadataScenarioTX_Common(socket);
	}
	if (flag & kMetadataFlag_ScenarioTX_Dedi)
	{
		logMetadata(kPacket_Metadata_ScenarioTX_Dedi, "resource/scenariotx/scenariotx_dedi.json");
		g_PacketManager.SendMetadataScenarioTX_Dedi(socket);
	}
	if (flag & kMetadataFlag_ShopItemList_Dedi)
	{
		logMetadata(kPacket_Metadata_ShopItemList_Dedi, "resource/scenariotx/shopitemlist_dedi.json");
		g_PacketManager.SendMetadataShopItemList_Dedi(socket);
	}
	if (!legacyMetadataIDs)
	{
		logMetadata(kPacket_Metadata_EpicPieceShop, "EpicPieceShop.csv");
		g_PacketManager.SendMetadataEpicPieceShop(socket);
	}

	if (flag & kMetadataFlag_WeaponProp)
	{
		logMetadata(kPacket_Metadata_WeaponProp, "models/SkinWeaponInfo_server.json");
		g_PacketManager.SendMetadataWeaponProp(socket);
	}
	if (flag & kMetadataFlag_PPSystem)
	{
		logMetadata(kPacket_Metadata_PPSystem, "ppsystem/config.json");
		g_PacketManager.SendMetadataPPSystem(socket);
	}
	if (flag & kMetadataFlag_ZBCompetitive)
	{
		logMetadata(kPacket_Metadata_ZBCompetitive, "resource/zombiecompetitive/ZBCompetitive.json");
		g_PacketManager.SendMetadataZBCompetitive(socket);
	}
	if (flag & kMetadataFlag_ModeEvent)
	{
		logMetadata(kPacket_Metadata_ModeEvent, "resource/ModeEvent/ModeEvent.csv");
		g_PacketManager.SendMetadataModeEvent(socket);
	}
	if (flag & kMetadataFlag_EventShop)
	{
		logMetadata(kPacket_Metadata_EventShop, "resource/CPShop/EventShop.csv");
		g_PacketManager.SendMetadataEventShop(socket);
	}
	if (flag & kMetadataFlag_FamilyTotalWarMap)
	{
		logMetadata(kPacket_Metadata_FamilyTotalWarMap, "resource/ClanWar/FamilyTotalWarMap.csv");
		g_PacketManager.SendMetadataFamilyTotalWarMap(socket);
	}
	if (flag & kMetadataFlag_FamilyTotalWar)
	{
		logMetadata(kPacket_Metadata_FamilyTotalWar, "resource/ClanWar/FamilyTotalWar.json");
		g_PacketManager.SendMetadataFamilyTotalWar(socket);
	}
	if (!legacyMetadataIDs)
	{
		logMetadata(kPacket_Metadata_WeaponAscend, "resource/WeaponAscend.csv");
		g_PacketManager.SendMetadataWeaponAscend(socket);
		logMetadata(kPacket_Metadata_PerkParam, "resource/zombi/PerkParam.csv");
		g_PacketManager.SendMetadataPerkParam(socket);
		logMetadata(kPacket_Metadata_Synthesis, "resource/Synthesis/SynthesisItem.csv");
		g_PacketManager.SendMetadataSynthesis(socket);
	}
	if (!legacyMetadataIDs)
	{
		logMetadata(kPacket_Metadata_ZCoinShop, "ZCoinShop.csv");
		g_PacketManager.SendMetadataZCoinShop(socket);
	}

	if (flag & kMetadataFlag_Unk54)
		g_PacketManager.SendMetadataUnk54(socket);
	if (flag & kMetadataFlag_Unk55)
		g_PacketManager.SendMetadataUnk55(socket);
}

void CUserManager::SendCrypt(IExtendedSocket* socket)
{
	if (!socket->GetSSLObject() && g_pServerConfig->crypt)
	{
		if (!socket->SetupCrypt())
		{
			Logger().Info("Client (%s) disconnected from the server (Crypt failed)\n", socket->GetIP().c_str());

			g_pServerInstance->DisconnectClient(socket);
		}
		else
		{
			unsigned char* key = socket->GetCryptKey();
			unsigned char* iv = socket->GetCryptIV();

			g_PacketManager.SendCrypt(socket, 0, key, iv);
			g_PacketManager.SendCrypt(socket, 1, key, iv);
		}
	}
}

static bool SendLatestLoginCrypt(IExtendedSocket* socket)
{
	if (!socket->SetupCrypt())
	{
		Logger().Info("Client (%s) latest login crypt setup failed\n", socket->GetIP().c_str());
		return false;
	}

	unsigned char* key = socket->GetCryptKey();
	unsigned char* iv = socket->GetCryptIV();

	g_PacketManager.SendCrypt(socket, 0, key, iv);
	int cryptDelayMs = 100;
	if (const char* envDelay = getenv("CSNZ_CRYPT1_DELAY_MS"))
		cryptDelayMs = max(0, atoi(envDelay));
	Sleep(cryptDelayMs);
	socket->SetCryptOutput(true);
	g_PacketManager.SendCrypt(socket, 1, key, iv);
	Logger().Info("Latest login crypt bootstrap sent; delayMs=%d, encrypted output enabled while waiting for client acknowledgement\n", cryptDelayMs);
	return true;
}

static bool ReplayLatestLoginSample(IExtendedSocket* socket)
{
	if (getenv("CSNZ_DISABLE_LOGIN_SAMPLE_REPLAY"))
		return false;

	int minIndex = 6;
	// Packets 128..131 are the large live metadata refresh set.  The server now
	// generates those records directly from LiveMetadata/MetadataArtifacts, so
	// keep replay to the pre-refresh bootstrap range by default.  Packet 134 is
	// a legacy GuideQuest(120) payload that the current client rejects.
	// Diagnostics can still opt into later captures through
	// CSNZ_LOGIN_SAMPLE_MAX_INDEX.
	int maxIndex = 127;
	if (const char* envMin = getenv("CSNZ_LOGIN_SAMPLE_MIN_INDEX"))
		minIndex = max(0, atoi(envMin));
	if (const char* envMax = getenv("CSNZ_LOGIN_SAMPLE_MAX_INDEX"))
		maxIndex = max(0, atoi(envMax));

	const char* sampleDirEnv = getenv("CSNZ_LOGIN_SAMPLE_DIR");
	vector<string> roots;
	if (sampleDirEnv && *sampleDirEnv)
		roots.push_back(sampleDirEnv);
	roots.push_back("..\\Packets_sampel\\2");
	roots.push_back("Packets_sampel\\2");
	roots.push_back("CSNZ_Server\\Packets_sampel\\2");

	struct SamplePacket
	{
		int index;
		string path;
	};

	vector<SamplePacket> packets;
	string selectedRoot;

	for (const auto& root : roots)
	{
		WIN32_FIND_DATAA data = {};
		string pattern = root + "\\Packet_*_ID_*_*.bin";
		HANDLE find = FindFirstFileA(pattern.c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			continue;

		// Sample 79 is a 471-byte legacy full UserUpdateInfo(157) payload whose
		// first subtype byte is 173.  The latest client repeatedly overruns that
		// layout and raises its runtime error path, while samples 117/118 use the
		// short current-compatible 157 forms.
		do
		{
			int index = -1;
			if (sscanf(data.cFileName, "Packet_%d_ID_%*d_%*d.bin", &index) == 1 &&
				index >= minIndex && index <= maxIndex && index != 79)
				packets.push_back({ index, root + "\\" + data.cFileName });
		} while (FindNextFileA(find, &data));

		FindClose(find);

		if (!packets.empty())
		{
			selectedRoot = root;
			break;
		}
	}

	if (packets.empty())
	{
		Logger().Info("Latest login sample replay skipped: sample packets not found\n");
		return false;
	}

	sort(packets.begin(), packets.end(), [](const SamplePacket& a, const SamplePacket& b)
	{
		return a.index < b.index;
	});

	Logger().Info("Latest login sample replay: root=%s, packets=%u, range=%d..%d, first=%d, last=%d\n",
		selectedRoot.c_str(), static_cast<unsigned int>(packets.size()),
		minIndex, maxIndex == INT_MAX ? -1 : maxIndex,
		packets.front().index, packets.back().index);

	int delayMs = 10;
	if (const char* envDelay = getenv("CSNZ_LOGIN_SAMPLE_DELAY_MS"))
		delayMs = max(0, atoi(envDelay));

	for (size_t i = 0; i < packets.size(); ++i)
	{
		const auto& packet = packets[i];
		if ((i % 16) == 0 || i + 1 == packets.size())
			Logger().Info("Latest login sample replay progress: packetIndex=%d (%u/%u)\n",
				packet.index, static_cast<unsigned int>(i + 1), static_cast<unsigned int>(packets.size()));
		g_PacketManager.SendPacketFromFile(socket, packet.path);
		if (delayMs > 0)
			SleepMS(delayMs);
	}

	return true;
}

void CUserManager::SendUserLoadout(IUser* user)
{
	// Latest hw.dll rejects the legacy Favorite(77) loadout/buy-menu payloads.
	// Skip them during startup until the latest favorite/loadout layout is mapped.
	return;

	vector<CUserLoadout> loadouts;
	g_UserDatabase.GetLoadouts(user->GetID(), loadouts);

	// unknown size error
	//vector<CUserFastBuy> fastBuy;
	//g_UserDatabase.GetFastBuy(user->GetID(), fastBuy);

	vector<CUserBuyMenu> buyMenu;
	g_UserDatabase.GetBuyMenu(user->GetID(), buyMenu);

	CUserCharacterExtended character(EXT_UFLAG_CURLOADOUT | EXT_UFLAG_CHARACTERID);
	g_UserDatabase.GetCharacterExtended(user->GetID(), character);

	vector<int> bookmark;
	g_UserDatabase.GetBookmark(user->GetID(), bookmark);

	g_PacketManager.SendFavoriteLoadout(user->GetExtendedSocket(), character.characterID, character.curLoadout, loadouts);
	//g_PacketManager.SendFavoriteFastBuy(user->GetExtendedSocket(), fastBuy);
	g_PacketManager.SendFavoriteBuyMenu(user->GetExtendedSocket(), buyMenu);
	g_PacketManager.SendFavoriteBookmark(user->GetExtendedSocket(), bookmark);
}

void CUserManager::SendUserNotices(IUser* user)
{
	for (auto& notice : g_pServerConfig->notices)
	{
		g_PacketManager.SendUMsgNotice(user->GetExtendedSocket(), notice);
	}
}

bool CUserManager::OnCharacterPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	string name = msg->ReadString();

	IUser* user = GetUserBySocket(socket);

	int replyCode = ChangeUserNickname(user, name, true);
	switch (replyCode)
	{
	case 0:
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(socket, OBFUSCATE("DB_QUERY_FAILED"));
		DisconnectUser(user);
		return false;
	case 1:
		g_PacketManager.SendReply(user->GetExtendedSocket(), ServerReply::S_REPLY_CREATEOK);
		break;
	case -1:
		g_PacketManager.SendReply(user->GetExtendedSocket(), ServerReply::S_REPLY_CREATE_ID_TOO_SHORT);
		return false;
	case -2:
		g_PacketManager.SendReply(user->GetExtendedSocket(), ServerReply::S_REPLY_CREATE_ID_TOO_LONG);
		return false;
	case -3:
		g_PacketManager.SendReply(user->GetExtendedSocket(), ServerReply::S_REPLY_CREATE_ID_ALREADY_EXIST);
		return false;
	case -4:
		g_PacketManager.SendReply(user->GetExtendedSocket(), 26); // 26? name blacklist
		return false;
	}

	CUserCharacter character = user->GetCharacter(UFLAG_LOW_ALL, UFLAG_HIGH_ALL);

	g_ItemManager.OnUserLogin(user);
	g_ClanManager.OnUserLogin(user);
	g_QuestManager.OnUserLogin(user);

	SendLoginPacket(user, character);

	return true;
}

bool CUserManager::OnUserMessage(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);

	int type = msg->ReadUInt8();
	switch (type)
	{
	case UMsgReceiveType::WhisperChat:
		g_ChannelManager.OnWhisperMessage(msg, user);
		break;
	case UMsgReceiveType::LobbyChat:
		g_ChannelManager.OnLobbyMessage(msg, socket, user);
		break;
	case UMsgReceiveType::RoomChat:
		g_ChannelManager.OnRoomUserMessage(msg, user);
		break;
	case UMsgReceiveType::ClanChat:
		Logger().Warn("CUserManager::OnUserMessage: ClanChat!\n");
		break;
	case UMsgReceiveType::RoomTeamChat:
		g_ChannelManager.OnRoomTeamUserMessage(msg, user);
		break;
	case UMsgReceiveType::PartyChat:
		// Party system is not implemented, so let's just say the party doesn't exist
		g_PacketManager.SendGameMatchFailMessage(socket, 6);
		break;
	case UMsgReceiveType::ServerYellChat:
		g_ChannelManager.OnServerYellMessage(msg, user);
		break;
	case UMsgReceiveType::RewardSelect:
		g_ItemManager.OnRewardSelect(msg, user);
		break;
	default:
		Logger().Warn("CUserManager::OnUserMessage: unknown request %d\n", type);
		break;
	}

	return true;
}

bool CUserManager::OnUpdateInfoPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int msgType = msg->ReadUInt8();
	switch (msgType)
	{
	case UpdateInfoPacketType::RequestUpdateNickname:
		g_ItemManager.OnNicknameChangeUse(user, msg->ReadString());
		break;
	case UpdateInfoPacketType::RequestUpdateLocation:
	{
		int nation = msg->ReadUInt16();
		int city = msg->ReadUInt16();
		int town = msg->ReadUInt16();

		user->UpdateLocation(nation, city, town);
		break;
	}
	case UpdateInfoPacketType::RequestUpdateTutorial:
	{
		int tutorial = msg->ReadUInt8();
		Logger().Info("RequestTutorial: %d\n", tutorial);

		break;
	}
	case UpdateInfoPacketType::RequestUpdateChatColor:
	{
		int chatColorID = msg->ReadUInt16();

		if (chatColorID)
		{
			string resourceName = g_pItemTable->GetCell<string>("recourcename", to_string(chatColorID));
			if (resourceName.find("chatcolor_") == std::string::npos)
				return true;

			CUserInventoryItem item;
			g_UserDatabase.GetFirstActiveItemByItemID(user->GetID(), chatColorID, item);

			if (!item.m_nItemID)
				return true;
		}

		user->UpdateChatColor(chatColorID);
		break;
	}
	case 12: // called when click on inventory button
		break;
	default:
		Logger().Warn("CUserManager::OnUpdateInfoPacket: unknown request %d\n", msgType);
		break;
	}

	return true;
}

void CUserManager::OnSecondTick(time_t curTime)
{
	for (auto u : m_Users)
		u->OnTick();
}

void CUserManager::SendNoticeMessageToAll(const string& msg)
{
	for (auto u : m_Users)
		g_PacketManager.SendUMsgNoticeMessageInChat(u->GetExtendedSocket(), msg);
}

void CUserManager::SendNoticeMsgBoxToAll(const string& msg)
{
	for (auto u : m_Users)
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(u->GetExtendedSocket(), msg);
}

int CUserManager::LoginUser(IExtendedSocket* socket, const string& userName, const string& password)
{
	return LoginUser(socket, userName, password, true, true);
}

int CUserManager::LoginUser(IExtendedSocket* socket, const string& userName, const string& password, bool sendLoginReply, bool sendCharacterPrompt)
{
	UserBan ban = {};
	int userID = g_UserDatabase.Login(userName, password, socket, ban, NULL);
	if (userID <= 0)
	{
		switch (userID)
		{
		case LOGIN_USER_BANNED:
			if (ban.banType == 1) // ban with msg
				g_PacketManager.SendUMsgSystemReply(socket, UMsgPacketType::SystemReply_MsgBox, "GM_CUT", vector<string>{ban.reason});
			break;
		}
		Logger().Info(OBFUSCATE("Login failed (code: %d)\n"), userID);
		return userID;
	}

	if (GetUserById(userID)) // if user with this uid is already on server
	{
		Logger().Info("Login failed (code: %d)\n", LOGIN_USER_ALREADY_LOGGED_IN_UID);

		g_PacketManager.SendReply(socket, ServerReply::S_REPLY_PLAYING);

		return LOGIN_USER_ALREADY_LOGGED_IN_UID;
	}

	if (GetUserBySocket(socket)) // if user with the same socket object is already on server
	{
		Logger().Info("Login failed (code: %d)\n", LOGIN_USER_ALREADY_LOGGED_IN_UUID);

		g_PacketManager.SendReply(socket, ServerReply::S_REPLY_PLAYING);

		return LOGIN_USER_ALREADY_LOGGED_IN_UUID;
	}

	IUser* newUser = AddUser(socket, userID, userName);
	if (!newUser)
	{
		Logger().Info("Login failed (code: %d)\n", LOGIN_SERVER_IS_FULL); // -5 (user limit)

		g_PacketManager.SendReply(socket, ServerReply::S_REPLY_EXCEED_MAX_CONNECTION);

		return LOGIN_SERVER_IS_FULL;
	}

	Logger().Info(OBFUSCATE("User logged in (IP: %s, UID: %d, Username: %s)\n"), newUser->GetNetworkConfig().m_szExternalIpAddress.c_str(), userID, userName.c_str());

	// Update last login time, last IP and last HWID
	CUserData data = newUser->GetUser(UDATA_FLAG_FIRSTLOGONTIME);
	data.flag = 0;
	if (data.firstLogonTime == 0)
	{
		data.firstLogonTime = g_pServerInstance->GetCurrentTime();
		data.flag |= UDATA_FLAG_FIRSTLOGONTIME;
	}

	data.flag |= UDATA_FLAG_LASTLOGONTIME | UDATA_FLAG_LASTIP | UDATA_FLAG_LASTHWID;
	data.lastLogonTime = g_pServerInstance->GetCurrentTime();
	data.lastIP = socket->GetIP();
	data.lastHWID = socket->GetHWID();

	g_UserDatabase.UpdateUserData(userID, data);
		
	if (sendLoginReply)
	{
		g_PacketManager.SendReply(socket, ServerReply::S_REPLY_YES);
		g_PacketManager.SendSessionID(socket, 167);

		string gameName = newUser->GetUsername();
		if (newUser->IsCharacterExists())
		{
			CUserCharacter loginCharacter = newUser->GetCharacter(UFLAG_LOW_GAMENAME | UFLAG_LOW_UNK27, 0);
			if (!loginCharacter.gameName.empty())
				gameName = loginCharacter.gameName;
		}
		g_PacketManager.SendUserStart(socket, newUser->GetID(), newUser->GetUsername(), gameName, true);

		if (SendLatestLoginCrypt(socket))
		{
			Logger().Info("Waiting for latest client crypt acknowledgement before login bootstrap\n");
			return true;
		}
	}

	if (!newUser->IsCharacterExists())
	{
		if (sendCharacterPrompt)
			g_PacketManager.SendCharacter(socket);
	}
	else
	{
		// continue login proccess
		CUserCharacter character = newUser->GetCharacter(UFLAG_LOW_ALL, UFLAG_HIGH_ALL);

		g_ItemManager.OnUserLogin(newUser);
		g_ClanManager.OnUserLogin(newUser);
		g_QuestManager.OnUserLogin(newUser);

		SendLoginPacket(newUser, character);
	}

	return LOGIN_OK;
}

int CUserManager::RegisterUser(IExtendedSocket* socket, const string& userName, const string& password)
{
	if (password.size() < 5 || password.size() > 15 || password.find_first_not_of("0123456789") == string::npos)
		return REGISTER_PASSWORD_WRONG;
	if (userName.size() < 5 || userName.size() > 15 || userName.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890") != string::npos)
		return REGISTER_USERNAME_WRONG;

	int regResult = g_UserDatabase.Register(userName, password, socket->GetIP());
	if (regResult < 0)
	{
		Logger().Info("Register failed (code: %d)\n", regResult);
		return regResult;
	}

	Logger().Info("Register ok (code: %d)\n", regResult);

	return regResult;
}

void CUserManager::DisconnectUser(IUser* user)
{
	g_pServerInstance->DisconnectClient(user->GetExtendedSocket());
}

void CUserManager::DisconnectAllFromServer()
{
	for (auto u : m_Users)
	{
		DisconnectUser(u);
	}
}

IUser* CUserManager::AddUser(IExtendedSocket* socket, int userID, const string& userName)
{
	if ((int)m_Users.size() >= g_pServerConfig->maxPlayers)
		return NULL;

	m_Users.push_back(new CUser(socket, userID, userName));
	return m_Users.back();
}

IUser* CUserManager::GetUserById(int userId)
{
	for (auto u : m_Users)
	{
		if (u->GetID() == userId)
			return u;
	}

	return NULL;
}

IUser* CUserManager::GetUserBySocket(IExtendedSocket* socket)
{
	for (auto u : m_Users)
	{
		if (u->GetExtendedSocket() == socket)
			return u;
	}

	return NULL;
}

IUser* CUserManager::GetUserByUsername(const string& username)
{
	for (auto u : m_Users)
	{
		if (u->GetUsername() == username)
			return u;
	}

	return NULL;
}

IUser* CUserManager::GetUserByNickname(const string& nickname)
{
	int userID = g_UserDatabase.IsUserExists(nickname, false);

	return GetUserById(userID);
}

void CUserManager::RemoveUser(IUser* user)
{
	for (auto u : m_Users)
	{
		if (u == user && RemoveUserInternal(u))
			break;
	}
}

void CUserManager::RemoveUserById(int userId)
{
	for (auto u : m_Users)
	{
		if (u->GetID() == userId && RemoveUserInternal(u))
			break;
	}
}

void CUserManager::RemoveUserBySocket(IExtendedSocket* socket)
{
	for (auto u : m_Users)
	{
		if (u->GetExtendedSocket() == socket && RemoveUserInternal(u))
			break;
	}
}

bool CUserManager::RemoveUserInternal(IUser* user)
{
	m_Users.erase(remove(begin(m_Users), end(m_Users), user), end(m_Users));

	CleanUpUser(user);
	delete user;

	return true;
}

void CUserManager::CleanUpUser(IUser* user)
{
	IRoom* room = user->GetCurrentRoom();
	if (room)
		room->RemoveUser(user);

	CChannel* channel = user->GetCurrentChannel();
	if (channel)
		channel->UserLeft(user);

	user->SetCurrentChannel(NULL);
}

std::vector<IUser*> CUserManager::GetUsers()
{
	return m_Users;
}

bool CUserManager::OnReportPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;
#if 0
	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int reason = msg->ReadUInt8();
	int userID = msg->ReadUInt32();
	string classificator = msg->ReadString();
	// cfg shit...., (5)ClsNotOpen, (6)BoxItem, (7)Spectator, (8)BoxItem2, (50)SPEED, Wall, W2lal, (1)Memory, (2)Class, (3)WpnCmd, (4)ClsCmd, (7)Table, spdcl, ChaHack, MOVTYP, OBS, OBSGUEST, SVDEX, QBARREL, SOCC(last char? a1 + 8), SOLID
	string object = msg->ReadString();

	Logger().Info("[SuspectNotice] detected suspect user '%s', userID dest: %d, reason: %d, %s, %s\n", user->GetLogName(), userID, reason, classificator.empty() ? "NULL" : classificator.c_str(), object.empty() ? "NULL" : object.c_str());

	// E0000002 - section(ce detect)
	// E0000005 - ogl hook?
	// E0000006 - ogl hook?
	// E0000008 - ogl hook?
	// UI001, UI002, UI003 - oid/uid manipulations
	if (classificator == "spdcl" || classificator == "E0000002" || classificator == "E0000005" || classificator == "E0000006" || classificator == "UI002" || classificator == "UI003" || classificator == "UI001" || classificator == "UI000"
		|| strstr(object.c_str(), "NG.dll") /*|| (classificator == "E0000008" && !strstr(object.c_str(), "Data = 0000000000.0000000000.0000000000") && !strstr(object.c_str(), "File = "))*/)
	{
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(user->GetExtendedSocket(), "The game has detected suspicious activity on client side. Try to close apps and join connect the game again.");

		DisconnectUser(user);
	}

	/// @todo:
	g_UserDatabase.SuspectAddAction(user->GetExtendedSocket()->GetHWID(), 0);
#endif
	return true;
}

bool CUserManager::OnAlarmPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
	{
		return false;
	}

	//vector<Notice_s> notices;
	//g_PacketManager.SendAlarm(socket, g_pServerConfig->notices);

	//for (auto& notice : g_pServerConfig->notices)
	//{
	//	g_PacketManager.SendUMsgNotice(socket, notice, 0);
	//}

	return true;
}

bool CUserManager::OnUserSurveyPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case 11:
		OnUserSurveyAnswerRequest(msg, user);
		break;
	case 12:
	{
		int unk = msg->ReadUInt8();
		string unk2 = msg->ReadString();
		break;
	}
	default:
		Logger().Warn(OBFUSCATE("[User '%s'] CUserManager::OnUserSurveyPacket: unknown request %d (len: %d)\n"), user->GetLogName(), type, msg->GetLength());
		break;
	}

	return true;
}

bool CUserManager::OnBanPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case BanPacketType::RequestBanAddNickname:
		OnBanAddNicknameRequest(msg, user);
		break;
	case BanPacketType::RequestBanRemoveNickname:
		OnBanRemoveNicknameRequest(msg, user);
		break;
	case BanPacketType::RequestBanSettings:
		OnBanSettingsRequest(msg, user);
		break;
	case BanPacketType::RequestBanListMaxSize:
		g_PacketManager.SendBanMaxSize(socket, g_pServerConfig->banListMaxSize);
		break;
	default:
		Logger().Warn(OBFUSCATE("[User '%s'] Unknown Packet_Ban type %d (len: %d)\n"), user->GetLogName(), type, msg->GetLength());
		break;
	}

	return true;
}

bool CUserManager::OnMessengerPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case 1: // send user info
	{
		/// @todo handle errors
		string gameName = msg->ReadString();

		int userID = g_UserDatabase.IsUserExists(gameName, false);

		CUserCharacter character = user->GetCharacter(UFLAG_LOW_ALL, UFLAG_HIGH_ALL);

		g_PacketManager.SendMessengerUserInfo(socket, userID, character);
		break;
	}
	default:
		Logger().Warn(OBFUSCATE("[User '%s'] CUserManager::OnMessengerPacket: unknown request %d\n"), user->GetLogName(), type);
	}

	return true;
}

bool CUserManager::OnAddonPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	vector<int> addons;
	g_UserDatabase.GetAddons(user->GetID(), addons);

	// update addon list on client side
	if (!addons.empty())
		g_PacketManager.SendAddonPacket(socket, addons);

	return true;
}

bool CUserManager::OnLeaguePacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case 0:
		g_PacketManager.SendLeaguePacket(socket);
		break;
	default:
		Logger().Warn(OBFUSCATE("[User '%s'] Unknown Packet_League type %d (len: %d)\n"), user->GetLogName(), type, msg->GetLength());
		break;
	}

	return true;
}

bool CUserManager::OnCryptPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	Logger().Info("Client (%s) crypt acknowledgement received\n", socket->GetIP().c_str());
	socket->SetCryptInput(true);

	IUser* user = GetUserBySocket(socket);
	if (user != NULL && user->IsCharacterExists())
	{
		const char* transferEnv = getenv("CSNZ_TRANSFER_AFTER_CRYPT");
		if (!transferEnv || transferEnv[0] != '1')
		{
			CUserCharacter character = user->GetCharacter(UFLAG_LOW_ALL, UFLAG_HIGH_ALL);

			g_ItemManager.OnUserLogin(user);
			g_ClanManager.OnUserLogin(user);
			g_QuestManager.OnUserLogin(user);

			Logger().Info("Continuing latest login bootstrap on same user socket after crypt acknowledgement\n");
			SendLoginPacket(user, character, true, false);
			return true;
		}

		int transferPort = 30002;
		try
		{
			transferPort = stoi(g_pServerConfig->tcpPort);
		}
		catch (...)
		{
			Logger().Warn("Invalid configured TCP port '%s', falling back to %d for HostServer transfer\n",
				g_pServerConfig->tcpPort.c_str(), transferPort);
		}

		const char* transferPacketEnv = getenv("CSNZ_TRANSFER_PACKET");
		if (transferPacketEnv && transferPacketEnv[0] == '1')
		{
			Logger().Info("Sending diagnostic Transfer(2) packet to 127.0.0.1:%d\n", transferPort);
			g_PacketManager.SendTransfer(socket, "127.0.0.1", transferPort, user->GetUsername());
		}
		else
		{
			Logger().Info("Sending asset HostServer transfer to 127.0.0.1:%d\n", transferPort);
			g_PacketManager.SendHostServerTransfer(socket, "127.0.0.1", transferPort);
		}
	}

	return true;
}

bool CUserManager::OnKickPacket(CReceivePacket* msg, IExtendedSocket* socket)
{
	LOG_PACKET;

	IUser* user = GetUserBySocket(socket);
	if (user == NULL)
		return false;

	CChannel* currentChannel = user->GetCurrentChannel();
	if (currentChannel == NULL)
		return false;

	IRoom* currentRoom = user->GetCurrentRoom();
	if (currentRoom == NULL)
		return false;

	int type = msg->ReadUInt8();
	switch (type)
	{
	case 0:
		Logger().Warn(OBFUSCATE("[User '%s'] Packet_Kick type 0\n"), user->GetLogName());
		break;
	case 1:
	{
		if (user != currentRoom->GetHostUser())
			return false;

		CRoomSettings* roomSettings = currentRoom->GetSettings();
		if (!roomSettings->superRoom)
			return false;

		int userID = msg->ReadUInt32();
		int unk2 = msg->ReadUInt8();
		string unk3 = msg->ReadString();

		IUser* destUser = GetUserById(userID);
		if (destUser == NULL)
			return false;

		if (destUser == user)
			return false;

		if (destUser->GetCurrentRoom() != currentRoom)
			return false;

		for (auto u : currentRoom->GetUsers())
			g_PacketManager.SendKickPacket(u->GetExtendedSocket(), userID);

		currentRoom->KickUser(destUser);

		currentChannel->SendFullUpdateRoomList(destUser);

		// add user to channel user list back
		currentChannel->UserJoin(destUser, true);
		g_PacketManager.SendLobbyJoin(destUser->GetExtendedSocket(), currentChannel);

		Logger().Warn(OBFUSCATE("[User '%s'] Packet_Kick type 1: unk: %d, unk2: %d, unk3: %s\n"), user->GetLogName(), userID, unk2, unk3.c_str());
		break;
	}
	default:
		Logger().Warn(OBFUSCATE("[User '%s'] Unknown Packet_Kick type %d (len: %d)\n"), user->GetLogName(), type, msg->GetLength());
		break;
	}

	return true;
}

void CUserManager::OnUserSurveyAnswerRequest(CReceivePacket* msg, IUser* user)
{
	UserSurveyAnswer answer;
	int surveyID = msg->ReadUInt32();
	answer.surveyID = surveyID;

	auto surveyIt = find_if(g_pServerConfig->surveys.begin(), g_pServerConfig->surveys.end(),
		[surveyID](const Survey& survey) { return survey.id == surveyID; });
	if (surveyIt == g_pServerConfig->surveys.end())
	{
		g_PacketManager.SendUserSurveyReply(user->GetExtendedSocket(), ANSWER_INVALID);
		return;
	}

	int questionsCount = msg->ReadUInt8();
	for (int i = 0; i < questionsCount; i++)
	{
		UserSurveyQuestionAnswer questionAnswer = {};
		int questionID = msg->ReadUInt8();
		questionAnswer.questionID = questionID;

		auto surveyQuestion = find_if(surveyIt->questions.begin(), surveyIt->questions.end(),
			[questionID](const SurveyQuestion& question) { return question.id == questionID; });
		if (surveyQuestion == surveyIt->questions.end())
		{
			g_PacketManager.SendUserSurveyReply(user->GetExtendedSocket(), ANSWER_INVALID);
			return;
		}

		int answerType = msg->ReadUInt8();
		if (answerType == 1 || answerType == 2)
		{
			questionAnswer.answers.push_back(msg->ReadString());
		}
		else
		{
			questionAnswer.checkBox = true;
			int checkBoxAnswersCount = msg->ReadUInt8();
			if (!checkBoxAnswersCount)
			{
				g_PacketManager.SendUserSurveyReply(user->GetExtendedSocket(), ANSWER_INVALID);
				return;
			}

			for (int k = 0; k < checkBoxAnswersCount; k++)
			{
				int answerID = msg->ReadUInt8();

				auto checkBoxAnswer = find_if(surveyQuestion->answersCheckBox.begin(), surveyQuestion->answersCheckBox.end(),
					[answerID](const SurveyQuestionAnswerCheckBox& answer) { return answer.id == answerID; });
				if (checkBoxAnswer == surveyQuestion->answersCheckBox.end())
				{
					g_PacketManager.SendUserSurveyReply(user->GetExtendedSocket(), ANSWER_INVALID);
					return;
				}

				questionAnswer.answers.push_back(checkBoxAnswer->answer);
			}
		}

		answer.questionsAnswers.push_back(questionAnswer);
	}

	if (g_UserDatabase.SurveyAnswer(user->GetID(), answer) <= 0)
	{
		g_PacketManager.SendUserSurveyReply(user->GetExtendedSocket(), ANSWER_DB_ERROR);
	}

	g_PacketManager.SendUserSurveyReply(user->GetExtendedSocket(), ANSWER_OK);
}

void CUserManager::OnBanAddNicknameRequest(CReceivePacket* msg, IUser* user)
{
	string gameName = msg->ReadString();

	int result = user->UpdateBanList(gameName);
	switch (result)
	{
	case 0:
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(user->GetExtendedSocket(), OBFUSCATE("DB_QUERY_FAILED")); // db error
		break;
	case 1:
		g_PacketManager.SendBanUpdateList(user->GetExtendedSocket(), gameName);
		break;
	case -1:
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(user->GetExtendedSocket(), OBFUSCATE("BAN_ADD_FAIL_NICKNAME_NOT_EXIST"));
		break;
	case -2:
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(user->GetExtendedSocket(), OBFUSCATE("BAN_ADD_FAIL_ID_EXIST"));
		break;
	case -3:
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(user->GetExtendedSocket(), OBFUSCATE("BAN_ADD_FAIL_MAX_NUMBER_EXCESS"));
		break;
	}
}

void CUserManager::OnBanRemoveNicknameRequest(CReceivePacket* msg, IUser* user)
{
	string gameName = msg->ReadString();

	int result = user->UpdateBanList(gameName, true);
	switch (result)
	{
	case 0:
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(user->GetExtendedSocket(), OBFUSCATE("DB_QUERY_FAILED")); // db error
		break;
	case 1:
		g_PacketManager.SendBanUpdateList(user->GetExtendedSocket(), gameName, true);
		break;
	case -1:
		g_PacketManager.SendUMsgNoticeMsgBoxToUuid(user->GetExtendedSocket(), OBFUSCATE("BAN_REMOVE_FAIL_NICKNAME_NOT_EXIST"));
		break;
	}
}

void CUserManager::OnBanSettingsRequest(CReceivePacket* msg, IUser* user)
{
	int settings = msg->ReadUInt8();

	user->UpdateBanSettings(settings);

	g_PacketManager.SendBanSettings(user->GetExtendedSocket(), settings);
}
