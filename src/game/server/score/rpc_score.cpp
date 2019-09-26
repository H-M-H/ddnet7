//#ifdef CONF_RPC

#include <engine/shared/config.h>
#include "../gamecontroller.h"

#include "rpc_score.h"

CRPCScore::CRPCScore(CGameContext* pGameServer) :
m_pGameServer(pGameServer),
m_pServer(pGameServer->Server()),
m_pRPC(m_pServer->RPC())
{
	str_copy(m_aMap, g_Config.m_SvMap, sizeof(m_aMap));
	FormatUuid(m_pGameServer->GameUuid(), m_aGameUuid, sizeof(m_aGameUuid));

	auto MapName(std::make_shared<rpc::MapName>());
	MapName->set_name(m_aMap);
	auto Fut = RPC()->BestTime(MapName);
	m_PendingRequests.emplace_back(
	[this, Fut]()
	{
		if (!Fut.Ready())
			return false;

		try
		{
			GameServer()->m_pController->m_CurrentRecord = Fut.Get().time();
		}
		catch (RPCException& Ex)
		{
			if (Ex.Status().error_code() != grpc::StatusCode::NOT_FOUND)
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);

}

void CRPCScore::Process()
{
	for (size_t i = 0; i < m_PendingRequests.size(); ++i)
	{
		if (m_PendingRequests[i]())
			m_PendingRequests.erase(m_PendingRequests.begin() + i);
	}
}

void CRPCScore::OnShutdown()
{
}

void CRPCScore::CheckBirthday(int ClientID)
{
	auto PlayerName = std::make_shared<rpc::PlayerName>();
	std::string Name (Server()->ClientName(ClientID));
	PlayerName->set_name(Name);
	auto Fut = RPC()->CheckBirthDay(PlayerName);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	m_PendingRequests.emplace_back(
	[this, ClientID, Name, Fut, JoinTime]()
	{
		if (Server()->ClientJoinTick(ClientID) != JoinTime)
			return true;
		if (!Fut.Ready())
			return false;
		try
		{
			int YearsAgo = Fut.Get().years_ago();
			if (YearsAgo == 0)
				return true;
			char aBuf[512];

			str_format(aBuf, sizeof(aBuf), "Happy DDNet birthday to %s for finishing their first map %d year%s ago!", Name.c_str(), YearsAgo, YearsAgo > 1 ? "s" : "");
			GameServer()->SendChat(-1, CHAT_ALL, ClientID, aBuf);
			str_format(aBuf, sizeof(aBuf), "Happy DDNet birthday, %s!\nYou have finished your first map exactly %d year%s ago!", Name.c_str(), YearsAgo, YearsAgo > 1 ? "s" : "");
			GameServer()->SendBroadcast(aBuf, ClientID);

		}
		catch (RPCException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}

void CRPCScore::LoadScore(int ClientID)
{
	std::string PlayerName (Server()->ClientName(ClientID));
	std::string MapName (m_aMap);
	auto PaM(std::make_shared<rpc::PlayerAndMap>());
	PaM->set_player_name(PlayerName);
	PaM->set_map_name(MapName);
	auto Fut = RPC()->GetPlayerScore(PaM);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	m_PendingRequests.emplace_back(
	[this, Fut, ClientID, JoinTime]()
	{
		if (Server()->ClientJoinTick(ClientID) != JoinTime)
			return true;

		if (!Fut.Ready())
			return false;

		try
		{
			auto Score = Fut.Get();
			PlayerData(ClientID)->m_BestTime = Score.time();
			PlayerData(ClientID)->m_CurrentTime = Score.time();
			GameServer()->m_apPlayers[ClientID]->m_Score = -Score.time();
			GameServer()->m_apPlayers[ClientID]->m_HasFinishScore = true;

			if(g_Config.m_SvCheckpointSave)
			{
				for(int i = 0; i < min(static_cast<int>(NUM_CHECKPOINTS), Score.check_point_size()); ++i)
				{
					PlayerData(ClientID)->m_aBestCpTime[i] = Score.check_point(i);
				}
			}
		}
		catch (RPCException& Ex)
		{
			if (Ex.Status().error_code() != grpc::StatusCode::NOT_FOUND)
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}

void CRPCScore::MapInfo(int ClientID, const char* MapName)
{
	auto Map(std::make_shared<rpc::MapName>());
	std::string MapStr(MapName);
	Map->set_name(MapStr);
	auto Fut = RPC()->MapInfo(Map);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	m_PendingRequests.emplace_back(
	[this, Fut, ClientID, MapStr, JoinTime]()
	{
		if (Server()->ClientJoinTick(ClientID) != JoinTime)
			return true;
		try
		{
			if (!Fut.Ready())
				return false;
			GameServer()->SendChatTarget(ClientID, Fut.Get().text().c_str());
		}
		catch (RPCException& Ex)
		{
			if (Ex.Status().error_code() == grpc::StatusCode::NOT_FOUND)
			{
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "No map like \"%s\" found.", MapStr.c_str());
				GameServer()->SendChatTarget(ClientID, aBuf);
			}
			else
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}

void CRPCScore::MapVote(std::shared_ptr<CMapVoteResult> *ppResult, int ClientID, const char* MapName)
{

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientID];
	if (!pPlayer)
		return;

	int64 Now = Server()->Tick();
	int Timeleft = pPlayer->m_LastVoteCall + Server()->TickSpeed()*g_Config.m_SvVoteDelay - Now;

	if (Now < pPlayer->m_FirstVoteTick)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "You must wait %d seconds before making your first vote", (int)((pPlayer->m_FirstVoteTick - Now) / Server()->TickSpeed()) + 1);
		GameServer()->SendChatTarget(ClientID, aBuf);
		return;
	}
	if (pPlayer->m_LastVoteCall && Timeleft > 0)
	{
		char aChatmsg[512] = {0};
		str_format(aChatmsg, sizeof(aChatmsg), "You must wait %d seconds before making another vote", (Timeleft/Server()->TickSpeed())+1);
		GameServer()->SendChatTarget(ClientID, aChatmsg);
		return;
	}
	if(time_get() < GameServer()->m_LastMapVote + (time_freq() * g_Config.m_SvVoteMapTimeDelay))
	{
		char chatmsg[512] = {0};
		str_format(chatmsg, sizeof(chatmsg), "There's a %d second delay between map-votes, please wait %d seconds.", g_Config.m_SvVoteMapTimeDelay, (int)(((GameServer()->m_LastMapVote+(g_Config.m_SvVoteMapTimeDelay * time_freq()))/time_freq())-(time_get()/time_freq())));
		GameServer()->SendChatTarget(ClientID, chatmsg);
	}

	auto Map(std::make_shared<rpc::MapName>());
	Map->set_name(MapName);
	std::string MapStr(MapName);
	auto Fut = RPC()->FindMap(Map);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	m_PendingRequests.emplace_back(
	[this, Fut, ClientID, MapStr, ppResult, JoinTime]()
	{
		if (Server()->ClientJoinTick(ClientID) != JoinTime)
			return true;

		if (!Fut.Ready())
			return false;

		try
		{
			auto& Res = *ppResult;
			auto MapInfo = Fut.Get();
			str_copy(Res->m_aMap, MapInfo.map_name().c_str(), sizeof(Res->m_aMap));
			str_copy(Res->m_aServer, MapInfo.server_type().c_str(), sizeof(Res->m_aServer));
			for (char* p = Res->m_aServer; *p; ++p)
				*p = tolower(*p);
			Res->m_ClientID = ClientID;
			Res->m_Done = true;
		}
		catch (RPCException& Ex)
		{
			if (Ex.Status().error_code() == grpc::StatusCode::NOT_FOUND)
			{
				char aBuf[512];
				str_format(aBuf, sizeof(aBuf), "No map like \"%s\" found. Try adding a '%%' at the start if you don't know the first character. Example: /map %%castle for \"Out of Castle\"", MapStr.c_str());
				GameServer()->SendChatTarget(ClientID, aBuf);
			}
			else
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}

void CRPCScore::OnFinish(unsigned int Size, int* pClientID, float Time, const char *pTimestamp, float* apCpTime[NUM_CHECKPOINTS], bool Team, bool NotEligible)
{
	auto Finish(std::make_shared<rpc::Finish>());
	Finish->set_map_name(m_aMap);
	Finish->set_game_uuid(m_aGameUuid);
	Finish->set_team(Team);
	Finish->set_time(Time);
	std::map<int, int> JoinTimes;

	for (unsigned int i = 0; i < Size; ++i)
	{
		JoinTimes[pClientID[i]] = Server()->ClientJoinTick(pClientID[i]);
		rpc::TeeFinish* Tee = Finish->add_tee_finished();
		Tee->set_player_name(Server()->ClientName(pClientID[i]));

		for(int j = 0; j < NUM_CHECKPOINTS; j++)
			Tee->set_check_point(j, apCpTime[i][j]);
	}
	auto Fut = RPC()->OnFinish(Finish);
	m_PendingRequests.emplace_back(
	[this, Fut, Finish, JoinTimes]()
	{
		if (!Fut.Ready())
			return false;

			try
			{
				auto Chat = Fut.Get();
				GameServer()->SendChat(-1, CHAT_ALL, -1, Chat.chat_all().c_str());
				for (auto& Msg : Chat.chat_id())
				{
					if (JoinTimes.find(Msg.first) == JoinTimes.end())
						continue;
					if (Server()->ClientJoinTick(Msg.first) == JoinTimes.at(Msg.first))
						GameServer()->SendChatTarget(Msg.first, Msg.second.c_str());
				}
			}
			catch (RPCException& Ex)
			{
				dbg_msg("rpcscore", "%s", Ex.what());
			}

		return true;
	}
	);
}
void CRPCScore::ShowRank(int ClientID, const char* pName, bool Search)
{
	std::string PlayerName(pName);
	std::string RequestingPlayerName(Server()->ClientName(ClientID));
	auto PaM(std::make_shared<rpc::PlayerAndMap>());
	PaM->set_map_name(m_aMap);
	PaM->set_player_name(pName);
	auto Fut = RPC()->GetRank(PaM);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	m_PendingRequests.emplace_back(
	[this, Fut, ClientID, JoinTime, PlayerName, RequestingPlayerName]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;
		char aBuf[256];
		try
		{
			auto R = Fut.Get();
			float Time = R.time();
			int Rank = R.rank();
			if(g_Config.m_SvHideScore)
			{
				str_format(aBuf, sizeof(aBuf), "Your time: %02d:%05.2f", (int)(Time/60), Time-((int)Time/60*60));
				GameServer()->SendChatTarget(ClientID, aBuf);
			}
			else
			{
				str_format(aBuf, sizeof(aBuf), "%d. %s Time: %02d:%05.2f, requested by %s", Rank, PlayerName.c_str(), (int)(Time/60), Time-((int)Time/60*60), RequestingPlayerName.c_str());
				GameServer()->SendChat(-1, CHAT_ALL, ClientID, aBuf);
			}

		}
		catch (RPCException& Ex)
		{
			if (Ex.Status().error_code() == grpc::StatusCode::NOT_FOUND)
			{
				str_format(aBuf, sizeof(aBuf), "%s is not ranked", PlayerName.c_str());
				GameServer()->SendChatTarget(ClientID, aBuf);
			}
			else
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::ShowTeamRank(int ClientID, const char* pName, bool Search) {}
void CRPCScore::ShowTimes(int ClientID, const char* pName, int Debut) {}
void CRPCScore::ShowTimes(int ClientID, int Debut) {}
void CRPCScore::ShowTop5(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut) {}
void CRPCScore::ShowTeamTop5(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut) {}
void CRPCScore::ShowPoints(int ClientID, const char* pName, bool Search) {}
void CRPCScore::ShowTopPoints(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut) {}
void CRPCScore::RandomMap(std::shared_ptr<CRandomMapResult> *ppResult, int ClientID, int stars) {}
void CRPCScore::RandomUnfinishedMap(std::shared_ptr<CRandomMapResult> *ppResult, int ClientID, int stars) {}
void CRPCScore::SaveTeam(int Team, const char* Code, int ClientID, const char* Server) {}
void CRPCScore::LoadTeam(const char* Code, int ClientID) {}

//#endif
