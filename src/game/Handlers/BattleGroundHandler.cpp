/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "Player.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "Object.h"
#include "Chat.h"
#include "BattleGroundMgr.h"
#include "BattleGroundWS.h"
#include "BattleGround.h"
#include "Language.h"
#include "ScriptMgr.h"
#include "World.h"
#include "Anticheat.h"

void WorldSession::HandleBattlemasterHelloOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;
    recv_data >> guid;

    DEBUG_LOG("WORLD: Recvd CMSG_BATTLEMASTER_HELLO Message from %s", guid.GetString().c_str());

    Creature* pCreature = GetPlayer()->GetMap()->GetCreature(guid);

    if (!pCreature)
        return;

    if (!pCreature->IsBattleMaster())                       // it's not battlemaster
        return;

    // Stop the npc if moving
    if (!pCreature->HasExtraFlag(CREATURE_FLAG_EXTRA_NO_MOVEMENT_PAUSE))
        pCreature->PauseOutOfCombatMovement();

    BattleGroundTypeId bgTypeId = sBattleGroundMgr.GetBattleMasterBG(pCreature->GetEntry());

    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
        return;

    if (!_player->GetBGAccessByLevel(bgTypeId))
    {
        // temp, must be gossip message...
        SendNotification(LANG_YOUR_BG_LEVEL_REQ_ERROR);
        return;
    }

    SendBattleGroundList(guid, bgTypeId);
}

void WorldSession::SendBattleGroundList(ObjectGuid guid, BattleGroundTypeId bgTypeId)
{
    WorldPacket data;
    sBattleGroundMgr.BuildBattleGroundListPacket(&data, guid, _player, bgTypeId);
    SendPacket(&data);
}

void WorldSession::HandleBattlefieldJoinOpcode(WorldPacket& recv_data)
{
    uint32 mapId;
    recv_data >> mapId;
    DEBUG_LOG("WorldSession::HandleBattlefieldJoinOpcode : mapId=%u", mapId);

    WorldPacket data(CMSG_BOOTME);
    data << uint64(0);
    data << uint32(mapId);
    data << uint32(0);
    data << uint8(0);

    HandleBattlemasterJoinOpcode(data);
}

void WorldSession::HandleBattlemasterJoinOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_BATTLEMASTER_JOIN");

    ObjectGuid guid;
    uint32 instanceId;
    uint32 mapId;
    uint8 joinAsGroup;
    bool queuedAtBGPortal = false;
    bool isPremade = false;
    Group* grp;

    recv_data >> guid;                                      // battlemaster guid, or player guid if joining queue from BG portal
    recv_data >> mapId;
    recv_data >> instanceId;                                // instance id, 0 if First Available selected
    recv_data >> joinAsGroup;                               // join as group

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
    if (guid == GetPlayer()->GetObjectGuid())
        queuedAtBGPortal = true;
#endif

    BattleGroundTypeId bgTypeId = GetBattleGroundTypeIdByMapId(mapId);

    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
    {
        ProcessAnticheatAction("PassiveAnticheat", "Attempt to queue for invalid BG type", CHEAT_ACTION_LOG | CHEAT_ACTION_REPORT_GMS);
        return;
    }
    if (bgTypeId == BATTLEGROUND_AV && joinAsGroup)
    {
        ProcessAnticheatAction("PassiveAnticheat", "Attempt to queue for AV as group", CHEAT_ACTION_LOG | CHEAT_ACTION_REPORT_GMS);
        return;
    }

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
    if (queuedAtBGPortal)
    {
        auto const& bgQueuePos = _player->GetBattleGroundEntryPoint();
        if (_player->GetMapId() != bgQueuePos.mapId || !_player->IsWithinDist3d(bgQueuePos, 50.0f))
        {
            ProcessAnticheatAction("PassiveAnticheat", "Attempt to queue for BG through out of range portal", CHEAT_ACTION_LOG | CHEAT_ACTION_REPORT_GMS);
            return;
        }
    }
    else
    {
        if (!_player->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BATTLEMASTER))
        {
            ProcessAnticheatAction("PassiveAnticheat", "Attempt to queue for BG through invalid creature", CHEAT_ACTION_LOG | CHEAT_ACTION_REPORT_GMS);
            return;
        }
    }
#else
    if (!_player->FindNearestInteractableNpcWithFlag(UNIT_NPC_FLAG_BATTLEMASTER))
    {
        auto const& bgQueuePos = _player->GetBattleGroundEntryPoint();
        if (_player->GetMapId() != bgQueuePos.mapId || !_player->IsWithinDist3d(bgQueuePos, 50.0f))
        {
            ProcessAnticheatAction("PassiveAnticheat", "Attempt to queue for BG through out of range portal", CHEAT_ACTION_LOG | CHEAT_ACTION_REPORT_GMS);
            return;
        }
        else
            queuedAtBGPortal = true;
    }
    else
        queuedAtBGPortal = false;
#endif

    // can do this, since it's battleground, not arena
    BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(bgTypeId);

    // ignore if player is already in BG
    if (_player->InBattleGround())
        return;
    
    // get bg instance or bg template if instance not found
    BattleGround *bg = nullptr;
    if (instanceId)
        bg = sBattleGroundMgr.GetBattleGroundThroughClientInstance(instanceId, bgTypeId);

    if (!bg && !(bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId)))
    {
        sLog.outError("Battleground: no available bg / template found");
        return;
    }

    BattleGroundBracketId bgBracketId = _player->GetBattleGroundBracketIdFromLevel(bgTypeId);

    if (bgBracketId == BG_BRACKET_ID_NONE)
        return;

    // check queue conditions
    if (!joinAsGroup)
    {
        // check Deserter debuff
        if (!_player->CanJoinToBattleground())
        {
            WorldPacket data(SMSG_GROUP_JOINED_BATTLEGROUND, 4);
            data << uint32(0xFFFFFFFE);
            _player->GetSession()->SendPacket(&data);
            return;
        }
        // check if already in queue
        if (_player->GetBattleGroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
            //player is already in this queue
            return;
        // check if has free queue slots
        if (!_player->HasFreeBattleGroundQueueId())
            return;

        // if we're here, then the conditions to join a bg are met. We can proceed in joining.
        // _player->GetGroup() was already checked, grp is already initialized
        BattleGroundQueue& bgQueue = sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId];

        GroupQueueInfo * ginfo = bgQueue.AddGroup(_player, nullptr, bgTypeId, bgBracketId, isPremade);
        uint32 avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bgBracketId);
        // already checked if queueSlot is valid, now just get it
        uint32 queueSlot = _player->AddBattleGroundQueueId(bgQueueTypeId);
        // store entry point coords
        _player->SetBattleGroundEntryPoint(_player, queuedAtBGPortal);

        WorldPacket data;
        // send status packet (in queue)
        sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0);
        SendPacket(&data);
        DEBUG_LOG("Battleground: player joined queue for bg queue type %u bg type %u: GUID %u, NAME %s", bgQueueTypeId, bgTypeId, _player->GetGUIDLow(), _player->GetName());
    }
    else
    {
        grp = _player->GetGroup();
        // no group found, error
        if (!grp)
            return;

        std::vector<uint32> excludedMembers;
        uint32 err = grp->CanJoinBattleGroundQueue(bgTypeId, bgQueueTypeId, 0, bg->GetMaxPlayersPerTeam(), _player, &excludedMembers);
        isPremade = sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_PREMADE_GROUP_WAIT_FOR_MATCH) &&
                    (grp->GetMembersCount() >= sWorld.getConfig(CONFIG_UINT32_BATTLEGROUND_PREMADE_QUEUE_GROUP_MIN_SIZE));

        if (err == BG_JOIN_ERR_GROUP_DESERTER)
        {
            WorldPacket data;
            sBattleGroundMgr.BuildGroupJoinedBattlegroundPacket(&data, BG_GROUPJOIN_DESERTERS);
            _player->GetSession()->SendPacket(&data);
            SendBattleGroundJoinError(err);
            return;
        }
        else if (err != BG_JOIN_ERR_OK)
        {
            SendBattleGroundJoinError(err);
            return;
        }

        // if we're here, then the conditions to join a bg are met. We can proceed in joining.

        // _player->GetGroup() was already checked, grp is already initialized
        BattleGroundQueue& bgQueue = sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId];
        DEBUG_LOG("Battleground: the following players are joining as group:");
        GroupQueueInfo * ginfo = bgQueue.AddGroup(_player, grp, bgTypeId, bgBracketId, isPremade, &excludedMembers);
        uint32 avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bgBracketId);
        for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member) continue;  // this should never happen
            
            if (std::find(excludedMembers.begin(), excludedMembers.end(), member->GetGUIDLow()) != excludedMembers.end())
            {
                WorldPacket data;
                sBattleGroundMgr.BuildGroupJoinedBattlegroundPacket(&data, BG_GROUPJOIN_FAILED);
                member->GetSession()->SendPacket(&data);
                SendBattleGroundJoinError(err);
                continue;
            }

            uint32 queueSlot = member->AddBattleGroundQueueId(bgQueueTypeId);           // add to queue
            member->SetBattleGroundEntryPoint(_player, queuedAtBGPortal);               // store entry point coords

            WorldPacket data;
            // send status packet (in queue)
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0);
            member->GetSession()->SendPacket(&data);
            sBattleGroundMgr.BuildGroupJoinedBattlegroundPacket(&data, bg->GetMapId());
            member->GetSession()->SendPacket(&data);
            DEBUG_LOG("Battleground: player joined queue for bg queue type %u bg type %u: GUID %u, NAME %s", bgQueueTypeId, bgTypeId, member->GetGUIDLow(), member->GetName());
        }
        DEBUG_LOG("Battleground: group end");
    }

    sBattleGroundMgr.ScheduleQueueUpdate(bgQueueTypeId, bgTypeId, bgBracketId);
}

void WorldSession::HandleBattleGroundPlayerPositionsOpcode(WorldPacket& /*recv_data*/)
{
    // empty opcode
    //DEBUG_LOG("WORLD: Recvd MSG_BATTLEGROUND_PLAYER_POSITIONS Message");

    BattleGround *bg = _player->GetBattleGround();
    if (!bg)                                                // can't be received if player not in battleground
        return;

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_WS:
        {
            uint32 count1 = 0;                                  //always constant zero?
            uint32 count2 = 0;                                  //count of next fields

            Player* ali_plr = sObjectMgr.GetPlayer(((BattleGroundWS*)bg)->GetAllianceFlagPickerGuid());
            if (ali_plr)
                ++count2;

            Player* horde_plr = sObjectMgr.GetPlayer(((BattleGroundWS*)bg)->GetHordeFlagPickerGuid());
            if (horde_plr)
                ++count2;

            WorldPacket data(MSG_BATTLEGROUND_PLAYER_POSITIONS, (4 + 4 + 16 * count1 + 16 * count2));
            data << count1;                             // alliance flag holders count - obsolete, now always 0
            /*for(uint8 i = 0; i < count1; ++i)
            {
                data << ObjectGuid(0);                  // guid
                data << (float)0;                       // x
                data << (float)0;                       // y
            }*/
            data << count2;                             // horde flag holders count - obsolete, now count of next fields
            if (ali_plr)
            {
                data << ObjectGuid(ali_plr->GetObjectGuid());
                data << float(ali_plr->GetPositionX());
                data << float(ali_plr->GetPositionY());
            }
            if (horde_plr)
            {
                data << ObjectGuid(horde_plr->GetObjectGuid());
                data << float(horde_plr->GetPositionX());
                data << float(horde_plr->GetPositionY());
            }

            SendPacket(&data);
        }
        break;
        case BATTLEGROUND_AB:
        case BATTLEGROUND_AV:
        {
            //for other BG types - send default
            WorldPacket data(MSG_BATTLEGROUND_PLAYER_POSITIONS, (4 + 4));
            data << uint32(0);
            data << uint32(0);
            SendPacket(&data);
        }
        break;
        default:
            //maybe it is sent also in arena - do nothing
            break;
    }
}

void WorldSession::HandlePVPLogDataOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Recvd MSG_PVP_LOG_DATA Message");

    BattleGround *bg = _player->GetBattleGround();
    if (!bg)
        return;

    WorldPacket data;
    sBattleGroundMgr.BuildPvpLogDataPacket(&data, bg);
    SendPacket(&data);

    DEBUG_LOG("WORLD: Sent MSG_PVP_LOG_DATA Message");
}

void WorldSession::HandleBattlefieldListOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_BATTLEFIELD_LIST Message");

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
    uint32 mapId;
    recv_data >> mapId;

    BattleGroundTypeId bgTypeId = GetBattleGroundTypeIdByMapId(mapId);
#else
    BattleGroundTypeId bgTypeId = BattleGroundTypeId(_player->GetQueuedBattleground());
#endif

    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
    {
        sLog.outError("Battleground: invalid bgtype received.");
        return;
    }

    WorldPacket data;
    sBattleGroundMgr.BuildBattleGroundListPacket(&data, _player->GetObjectGuid(), _player, BattleGroundTypeId(bgTypeId));
    SendPacket(&data);
}

void WorldSession::HandleBattleFieldPortOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_BATTLEFIELD_PORT Message");

    uint8 action = 0; // enter battle 0x1, leave queue 0x0
    uint32 mapId = 0;

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
    recv_data >> mapId >> action;

    BattleGroundTypeId bgTypeId = GetBattleGroundTypeIdByMapId(mapId);
#else
    recv_data >> action;

    BattleGroundTypeId bgTypeId = BattleGroundTypeId(_player->GetQueuedBattleground());
#endif

    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
    {
        DEBUG_LOG("BattlegroundHandler: invalid bg map (%u) received.", mapId);
        return;
    }
    if (!_player->InBattleGroundQueue())
    {
        DEBUG_LOG("BattlegroundHandler: Invalid CMSG_BATTLEFIELD_PORT received from player (%u), he is not in bg_queue.", _player->GetGUIDLow());
        return;
    }

    //get GroupQueueInfo from BattleGroundQueue
    BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BGQueueTypeId(bgTypeId);
    BattleGroundQueue& bgQueue = sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId];
    //we must use temporary variable, because GroupQueueInfo pointer can be deleted in BattleGroundQueue::RemovePlayer() function
    GroupQueueInfo ginfo;
    if (!bgQueue.GetPlayerGroupInfoData(_player->GetObjectGuid(), &ginfo))
    {
        DEBUG_LOG("BattlegroundHandler: itrplayerstatus not found.");
        return;
    }
    // if action == 1, then instanceId is required
    if (!ginfo.IsInvitedToBGInstanceGUID && action == 1)
    {
        DEBUG_LOG("BattlegroundHandler: instance not found.");
        return;
    }

    BattleGround *bg = sBattleGroundMgr.GetBattleGround(ginfo.IsInvitedToBGInstanceGUID, bgTypeId);

    // bg template might and must be used in case of leaving queue, when instance is not created yet
    if (!bg && action == 0)
        bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
    if (!bg)
    {
        DEBUG_LOG("BattlegroundHandler: bg_template not found for type id %u.", bgTypeId);
        return;
    }

    //some checks if player isn't cheating - it is not exactly cheating, but we cannot allow it
    if (action == 1)
    {
        //if player is trying to enter battleground and he has deserter debuff, we must just remove him from queue
        if (!_player->CanJoinToBattleground())
        {
            //send bg command result to show nice message
            WorldPacket data2(SMSG_GROUP_JOINED_BATTLEGROUND, 4);
            data2 << uint32(0xFFFFFFFE);
            _player->GetSession()->SendPacket(&data2);
            action = 0;
            DEBUG_LOG("Battleground: player %s (%u) has a deserter debuff, do not port him to battleground!", _player->GetName(), _player->GetGUIDLow());
        }
        //if player don't match battleground max level, then do not allow him to enter! (this might happen when player leveled up during his waiting in queue
        if (_player->GetLevel() > bg->GetMaxLevel())
        {
            sLog.outError("Battleground: Player %s (%u) has level (%u) higher than maxlevel (%u) of battleground (%u)! Do not port him to battleground!",
                          _player->GetName(), _player->GetGUIDLow(), _player->GetLevel(), bg->GetMaxLevel(), bg->GetTypeID());
            action = 0;
        }
    }
    uint32 queueSlot = _player->GetBattleGroundQueueIndex(bgQueueTypeId);
    WorldPacket data;
    switch (action)
    {
        case 1:                                         // port to battleground
            if (!_player->IsInvitedForBattleGroundQueueType(bgQueueTypeId))
            {
                sLog.outError("Battleground: Player %s (%u) is not invited in queue type %u !", _player->GetName(), _player->GetGUIDLow(), bgQueueTypeId);
                return;                                 // cheating?
            }

            // resurrect the player
            if (!_player->IsAlive())
            {
                _player->ResurrectPlayer(1.0f);
                _player->SpawnCorpseBones();
            }
            // stop taxi flight at port
            if (_player->IsTaxiFlying())
            {
                _player->GetMotionMaster()->MovementExpired();
                _player->m_taxi.ClearTaxiDestinations();
            }

            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, queueSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime());
            _player->GetSession()->SendPacket(&data);
            // remove battleground queue status from BGmgr
            bgQueue.RemovePlayer(_player->GetObjectGuid(), false);
            // this is still needed here if battleground "jumping" shouldn't add deserter debuff
            // also this is required to prevent stuck at old battleground after SetBattleGroundId set to new
            if (BattleGround *currentBg = _player->GetBattleGround())
                currentBg->RemovePlayerAtLeave(_player->GetObjectGuid(), false, true);

            // set the destination instance id
            _player->SetBattleGroundId(bg->GetInstanceID(), bgTypeId);
            // set the destination team
            _player->SetBGTeam(ginfo.GroupTeam);
            // bg->HandleBeforeTeleportToBattleGround(_player);
            sBattleGroundMgr.SendToBattleGround(_player, ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
            // add only in HandleMoveWorldPortAck()
            // bg->AddPlayer(_player,team);
            DEBUG_LOG("Battleground: player %s (%u) joined battle for bg %u, bgtype %u, queue type %u.", _player->GetName(), _player->GetGUIDLow(), bg->GetInstanceID(), bg->GetTypeID(), bgQueueTypeId);
            sLog.out(LOG_BG, "[%u,%u]: %s:%u [%u:%s] enters",
                     bg->GetMapId(), bg->GetInstanceID(),
                     _player->GetName(),
                     _player->GetGUIDLow(),
                     GetAccountId(), GetRemoteAddress().c_str());
            break;
        case 0:                                         // leave queue
            _player->RemoveBattleGroundQueueId(bgQueueTypeId);  // must be called this way, because if you move this call to queue->removeplayer, it causes bugs
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, queueSlot, STATUS_NONE, 0, 0);
            bgQueue.RemovePlayer(_player->GetObjectGuid(), true);
            // player left queue, we should update it
            sBattleGroundMgr.ScheduleQueueUpdate(bgQueueTypeId, bgTypeId, _player->GetBattleGroundBracketIdFromLevel(bgTypeId));
            SendPacket(&data);
            DEBUG_LOG("Battleground: player %s (%u) left queue for bgtype %u, queue type %u.", _player->GetName(), _player->GetGUIDLow(), bg->GetTypeID(), bgQueueTypeId);
            sLog.out(LOG_BG, "[%u,%u] %s:%u [%u:%s] leaves queue",
                     bg->GetMapId(), bg->GetInstanceID(),
                     _player->GetName(),
                     _player->GetGUIDLow(),
                     GetAccountId(), GetRemoteAddress().c_str(),
                     bgTypeId);
            break;
        default:
            sLog.outError("Battleground port: unknown action %u", action);
            break;
    }
}

void WorldSession::HandleLeaveBattlefieldOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_LEAVE_BATTLEFIELD Message");

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
    recv_data.read_skip<uint8>();                           // unk1
    recv_data.read_skip<uint8>();                           // BattleGroundTypeId-1 ?
    recv_data.read_skip<uint16>();                          // unk2 0
#endif

    // not allow leave battleground in combat
    if (_player->IsInCombat())
        if (BattleGround* bg = _player->GetBattleGround())
            if (bg->GetStatus() != STATUS_WAIT_LEAVE)
                return;

    _player->LeaveBattleground();
}

void WorldSession::HandleBattlefieldStatusOpcode(WorldPacket& /*recv_data*/)
{
    // empty opcode
    DEBUG_LOG("WORLD: Battleground status");

    WorldPacket data;
    // we must update all queues here
    BattleGround *bg = nullptr;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattleGroundQueueTypeId bgQueueTypeId = _player->GetBattleGroundQueueTypeId(i);
        if (!bgQueueTypeId)
            continue;

        BattleGroundTypeId bgTypeId = BattleGroundMgr::BGTemplateId(bgQueueTypeId);
        if (bgTypeId == _player->GetBattleGroundTypeId())
        {
            bg = _player->GetBattleGround();
            //so i must use bg pointer to get that information
            if (bg)
            {
                // this line is checked, i only don't know if GetStartTime is changing itself after bg end!
                // send status in BattleGround
                sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, i, STATUS_IN_PROGRESS, bg->GetEndTime(), bg->GetStartTime());
                SendPacket(&data);
                continue;
            }
        }
        //we are sending update to player about queue - he can be invited there!
        //get GroupQueueInfo for queue status
        BattleGroundQueue& bgQueue = sBattleGroundMgr.m_BattleGroundQueues[bgQueueTypeId];
        GroupQueueInfo ginfo;
        if (!bgQueue.GetPlayerGroupInfoData(_player->GetObjectGuid(), &ginfo))
            continue;
        if (ginfo.IsInvitedToBGInstanceGUID)
        {
            bg = sBattleGroundMgr.GetBattleGround(ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
            if (!bg)
                continue;
            uint32 remainingTime = WorldTimer::getMSTimeDiff(WorldTimer::getMSTime(), ginfo.RemoveInviteTime);
            // send status invited to BattleGround
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, i, STATUS_WAIT_JOIN, remainingTime, 0);
            SendPacket(&data);
        }
        else
        {
            bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
            if (!bg)
                continue;
            uint32 avgTime = bgQueue.GetAverageQueueWaitTime(&ginfo, _player->GetBattleGroundBracketIdFromLevel(bgTypeId));
            // send status in BattleGround Queue
            sBattleGroundMgr.BuildBattleGroundStatusPacket(&data, bg, i, STATUS_WAIT_QUEUE, avgTime, WorldTimer::getMSTimeDiff(ginfo.JoinTime, WorldTimer::getMSTime()));
            SendPacket(&data);
        }
    }
}

void WorldSession::HandleAreaSpiritHealerQueryOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: CMSG_AREA_SPIRIT_HEALER_QUERY");

    BattleGround *bg = _player->GetBattleGround();
    if (!bg)
        return;

    ObjectGuid guid;
    recv_data >> guid;

    Creature* unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
        return;

    if (!unit->IsSpiritService())                           // it's not spirit service
        return;

    unit->SendAreaSpiritHealerQueryOpcode(GetPlayer());
}

void WorldSession::HandleAreaSpiritHealerQueueOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: CMSG_AREA_SPIRIT_HEALER_QUEUE");

    BattleGround *bg = _player->GetBattleGround();
    if (!bg)
        return;

    ObjectGuid guid;
    recv_data >> guid;

    Creature* unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
        return;

    if (!unit->IsSpiritService())                           // it's not spirit service
        return;

    sScriptMgr.OnGossipHello(GetPlayer(), unit);
}

void WorldSession::SendBattleGroundJoinError(uint8 err)
{
    WorldPacket data;
    int32 msg;
    switch (err)
    {
        case BG_JOIN_ERR_OFFLINE_MEMBER:
            msg = LANG_BG_GROUP_OFFLINE_MEMBER;
            break;
        case BG_JOIN_ERR_GROUP_TOO_MANY:
            msg = LANG_BG_GROUP_TOO_LARGE;
            break;
        case BG_JOIN_ERR_MIXED_FACTION:
            msg = LANG_BG_GROUP_MIXED_FACTION;
            break;
        case BG_JOIN_ERR_MIXED_LEVELS:
            msg = LANG_BG_GROUP_MIXED_LEVELS;
            break;
        case BG_JOIN_ERR_GROUP_MEMBER_ALREADY_IN_QUEUE:
            msg = LANG_BG_GROUP_MEMBER_ALREADY_IN_QUEUE;
            break;
        case BG_JOIN_ERR_GROUP_DESERTER:
            msg = LANG_BG_GROUP_MEMBER_DESERTER;
            break;
        case BG_JOIN_ERR_ALL_QUEUES_USED:
            msg = LANG_BG_GROUP_MEMBER_NO_FREE_QUEUE_SLOTS;
            break;
        case BG_JOIN_ERR_GROUP_NOT_ENOUGH:
        //case BG_JOIN_ERR_MIXED_ARENATEAM:
        default:
            return;
            break;
    }
    ChatHandler::BuildChatPacket(data, CHAT_MSG_BG_SYSTEM_NEUTRAL, GetMangosString(msg), LANG_UNIVERSAL);
    SendPacket(&data);
}
