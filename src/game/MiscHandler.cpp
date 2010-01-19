/*
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseImpl.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "Auth/BigNumber.h"
#include "Auth/Sha1.h"
#include "UpdateData.h"
#include "LootMgr.h"
#include "Chat.h"
#include "ScriptCalls.h"
#include <zlib/zlib.h>
#include "ObjectAccessor.h"
#include "Object.h"
#include "BattleGround.h"
#include "Pet.h"
#include "SocialMgr.h"

void WorldSession::HandleRepopRequestOpcode( WorldPacket & recv_data )
{
    sLog.outDebug( "WORLD: Recvd CMSG_REPOP_REQUEST Message" );

    // recv_data.read_skip<uint8>(); client crash

    if(GetPlayer()->isAlive()||GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;

    // the world update order is sessions, players, creatures
    // the netcode runs in parallel with all of these
    // creatures can kill players
    // so if the server is lagging enough the player can
    // release spirit after he's killed but before he is updated
    if(GetPlayer()->getDeathState() == JUST_DIED)
    {
        sLog.outDebug("HandleRepopRequestOpcode: got request after player %s(%d) was killed and before he was updated", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow());
        GetPlayer()->KillPlayer();
    }

    //this is spirit release confirm?
    GetPlayer()->RemovePet(NULL,PET_SAVE_NOT_IN_SLOT, true);
    GetPlayer()->BuildPlayerRepop();
    GetPlayer()->RepopAtGraveyard();
}

void WorldSession::HandleWhoOpcode( WorldPacket & recv_data )
{
    sLog.outDebug( "WORLD: Recvd CMSG_WHO Message" );
    //recv_data.hexlike();

    uint32 clientcount = 0;

    uint32 level_min, level_max, racemask, classmask, zones_count, str_count;
    uint32 zoneids[10];                                     // 10 is client limit
    std::string player_name, guild_name;

    recv_data >> level_min;                                 // maximal player level, default 0
    recv_data >> level_max;                                 // minimal player level, default 100 (MAX_LEVEL)
    recv_data >> player_name;                               // player name, case sensitive...

    recv_data >> guild_name;                                // guild name, case sensitive...

    recv_data >> racemask;                                  // race mask
    recv_data >> classmask;                                 // class mask
    recv_data >> zones_count;                               // zones count, client limit=10 (2.0.10)

    if(zones_count > 10)
        return;                                             // can't be received from real client or broken packet

    for(uint32 i = 0; i < zones_count; ++i)
    {
        uint32 temp;
        recv_data >> temp;                                  // zone id, 0 if zone is unknown...
        zoneids[i] = temp;
        sLog.outDebug("Zone %u: %u", i, zoneids[i]);
    }

    recv_data >> str_count;                                 // user entered strings count, client limit=4 (checked on 2.0.10)

    if(str_count > 4)
        return;                                             // can't be received from real client or broken packet

    sLog.outDebug("Minlvl %u, maxlvl %u, name %s, guild %s, racemask %u, classmask %u, zones %u, strings %u", level_min, level_max, player_name.c_str(), guild_name.c_str(), racemask, classmask, zones_count, str_count);

    std::wstring str[4];                                    // 4 is client limit
    for(uint32 i = 0; i < str_count; ++i)
    {
        std::string temp;
        recv_data >> temp;                                  // user entered string, it used as universal search pattern(guild+player name)?

        if(!Utf8toWStr(temp,str[i]))
            continue;

        wstrToLower(str[i]);

        sLog.outDebug("String %u: %s", i, temp.c_str());
    }

    std::wstring wplayer_name;
    std::wstring wguild_name;
    if(!(Utf8toWStr(player_name, wplayer_name) && Utf8toWStr(guild_name, wguild_name)))
        return;
    wstrToLower(wplayer_name);
    wstrToLower(wguild_name);

    // client send in case not set max level value 100 but mangos support 255 max level,
    // update it to show GMs with characters after 100 level
    if(level_max >= MAX_LEVEL)
        level_max = STRONG_MAX_LEVEL;

    uint32 team = _player->GetTeam();
    uint32 security = GetSecurity();
    bool allowTwoSideWhoList = sWorld.getConfig(CONFIG_ALLOW_TWO_SIDE_WHO_LIST);
    uint32 gmLevelInWhoList  = sWorld.getConfig(CONFIG_GM_LEVEL_IN_WHO_LIST);

    WorldPacket data( SMSG_WHO, 50 );                       // guess size
    data << clientcount;                                    // clientcount place holder
    data << clientcount;                                    // clientcount place holder

    //TODO: Guard Player map
    HashMapHolder<Player>::MapType& m = ObjectAccessor::Instance().GetPlayers();
    for(HashMapHolder<Player>::MapType::const_iterator itr = m.begin(); itr != m.end(); ++itr)
    {
        if (security == SEC_PLAYER)
        {
            // player can see member of other team only if CONFIG_ALLOW_TWO_SIDE_WHO_LIST
            if (itr->second->GetTeam() != team && !allowTwoSideWhoList )
                continue;

            // player can see MODERATOR, GAME MASTER, ADMINISTRATOR only if CONFIG_GM_IN_WHO_LIST
            if ((itr->second->GetSession()->GetSecurity() > gmLevelInWhoList))
                continue;
        }

        // check if target is globally visible for player
        if (!(itr->second->IsVisibleGloballyFor(_player)))
            continue;

        // check if target's level is in level range
        uint32 lvl = itr->second->getLevel();
        if (lvl < level_min || lvl > level_max)
            continue;

        // check if class matches classmask
        uint32 class_ = itr->second->getClass();
        if (!(classmask & (1 << class_)))
            continue;

        // check if race matches racemask
        uint32 race = itr->second->getRace();
        if (!(racemask & (1 << race)))
            continue;

        uint32 pzoneid = itr->second->GetZoneId();

        bool z_show = true;
        for(uint32 i = 0; i < zones_count; ++i)
        {
            if(zoneids[i] == pzoneid)
            {
                z_show = true;
                break;
            }

            z_show = false;
        }
        if (!z_show)
            continue;

        std::string pname = itr->second->GetName();
        std::wstring wpname;
        if(!Utf8toWStr(pname,wpname))
            continue;
        wstrToLower(wpname);

        if (!(wplayer_name.empty() || wpname.find(wplayer_name) != std::wstring::npos))
            continue;

        std::string gname = sObjectMgr.GetGuildNameById(itr->second->GetGuildId());
        std::wstring wgname;
        if(!Utf8toWStr(gname,wgname))
            continue;
        wstrToLower(wgname);

        if (!(wguild_name.empty() || wgname.find(wguild_name) != std::wstring::npos))
            continue;

        std::string aname;
        if(AreaTableEntry const* areaEntry = GetAreaEntryByAreaID(itr->second->GetZoneId()))
            aname = areaEntry->area_name[GetSessionDbcLocale()];

        bool s_show = true;
        for(uint32 i = 0; i < str_count; ++i)
        {
            if (!str[i].empty())
            {
                if (wgname.find(str[i]) != std::wstring::npos ||
                    wpname.find(str[i]) != std::wstring::npos ||
                    Utf8FitTo(aname, str[i]) )
                {
                    s_show = true;
                    break;
                }
                s_show = false;
            }
        }
        if (!s_show)
            continue;

        data << pname;                                      // player name
        data << gname;                                      // guild name
        data << uint32( lvl );                              // player level
        data << uint32( class_ );                           // player class
        data << uint32( race );                             // player race
        data << uint32( pzoneid );                          // player zone id

        // 49 is maximum player count sent to client
        if ((++clientcount) == 49)
            break;
    }

    data.put( 0,              clientcount );                //insert right count
    data.put( sizeof(uint32), clientcount );                //insert right count

    SendPacket(&data);
    sLog.outDebug( "WORLD: Send SMSG_WHO Message" );
}

void WorldSession::HandleLogoutRequestOpcode( WorldPacket & /*recv_data*/ )
{
    sLog.outDebug( "WORLD: Recvd CMSG_LOGOUT_REQUEST Message, security - %u", GetSecurity() );

    if (uint64 lguid = GetPlayer()->GetLootGUID())
        DoLootRelease(lguid);

    //Can not logout if...
    if( GetPlayer()->isInCombat() ||                        //...is in combat
        GetPlayer()->duel         ||                        //...is in Duel
                                                            //...is jumping ...is falling
        GetPlayer()->m_movementInfo.HasMovementFlag(MovementFlags(MOVEMENTFLAG_JUMPING | MOVEMENTFLAG_FALLING)))
    {
        WorldPacket data( SMSG_LOGOUT_RESPONSE, (2+4) ) ;
        data << (uint8)0xC;
        data << uint32(0);
        data << uint8(0);
        SendPacket( &data );
        LogoutRequest(0);
        return;
    }

    //instant logout in taverns/cities or on taxi or for admins, gm's, mod's if its enabled in mangosd.conf
    if (GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || GetPlayer()->isInFlight() ||
        GetSecurity() >= sWorld.getConfig(CONFIG_INSTANT_LOGOUT))
    {
        LogoutPlayer(true);
        return;
    }

    // not set flags if player can't free move to prevent lost state at logout cancel
    if(GetPlayer()->CanFreeMove())
    {
        GetPlayer()->SetStandState(UNIT_STAND_STATE_SIT);

        WorldPacket data( SMSG_FORCE_MOVE_ROOT, (8+4) );    // guess size
        data.append(GetPlayer()->GetPackGUID());
        data << (uint32)2;
        SendPacket( &data );
        GetPlayer()->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    WorldPacket data( SMSG_LOGOUT_RESPONSE, 5 );
    data << uint32(0);
    data << uint8(0);
    SendPacket( &data );
    LogoutRequest(time(NULL));
}

void WorldSession::HandlePlayerLogoutOpcode( WorldPacket & /*recv_data*/ )
{
    sLog.outDebug( "WORLD: Recvd CMSG_PLAYER_LOGOUT Message" );
}

void WorldSession::HandleLogoutCancelOpcode( WorldPacket & /*recv_data*/ )
{
    sLog.outDebug( "WORLD: Recvd CMSG_LOGOUT_CANCEL Message" );

    LogoutRequest(0);

    WorldPacket data( SMSG_LOGOUT_CANCEL_ACK, 0 );
    SendPacket( &data );

    // not remove flags if can't free move - its not set in Logout request code.
    if(GetPlayer()->CanFreeMove())
    {
        //!we can move again
        data.Initialize( SMSG_FORCE_MOVE_UNROOT, 8 );       // guess size
        data.append(GetPlayer()->GetPackGUID());
        data << uint32(0);
        SendPacket( &data );

        //! Stand Up
        GetPlayer()->SetStandState(UNIT_STAND_STATE_STAND);

        //! DISABLE_ROTATE
        GetPlayer()->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    sLog.outDebug( "WORLD: sent SMSG_LOGOUT_CANCEL_ACK Message" );
}

void WorldSession::HandleTogglePvP( WorldPacket & recv_data )
{
    // this opcode can be used in two ways: Either set explicit new status or toggle old status
    if(recv_data.size() == 1)
    {
        bool newPvPStatus;
        recv_data >> newPvPStatus;
        GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP, newPvPStatus);
    }
    else
    {
        GetPlayer()->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP);
    }

    if(GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
    {
        if(!GetPlayer()->IsPvP() || GetPlayer()->pvpInfo.endTimer != 0)
            GetPlayer()->UpdatePvP(true, true);
    }
    else
    {
        if(!GetPlayer()->pvpInfo.inHostileArea && GetPlayer()->IsPvP())
            GetPlayer()->pvpInfo.endTimer = time(NULL);     // start toggle-off
    }
}

void WorldSession::HandleZoneUpdateOpcode( WorldPacket & recv_data )
{
    uint32 newZone;
    recv_data >> newZone;

    sLog.outDetail("WORLD: Recvd ZONE_UPDATE: %u", newZone);

    // use server size data
    uint32 newzone, newarea;
    GetPlayer()->GetZoneAndAreaId(newzone,newarea);
    GetPlayer()->UpdateZone(newzone,newarea);
}

void WorldSession::HandleSetTargetOpcode( WorldPacket & recv_data )
{
    // When this packet send?
    uint64 guid ;
    recv_data >> guid;

    _player->SetTargetGUID(guid);

    // update reputation list if need
    Unit* unit = ObjectAccessor::GetUnit(*_player, guid );
    if(!unit)
        return;

    if(FactionTemplateEntry const* factionTemplateEntry = sFactionTemplateStore.LookupEntry(unit->getFaction()))
        _player->GetReputationMgr().SetVisible(factionTemplateEntry);
}

void WorldSession::HandleSetSelectionOpcode( WorldPacket & recv_data )
{
    uint64 guid;
    recv_data >> guid;

    _player->SetSelection(guid);

    // update reputation list if need
    Unit* unit = ObjectAccessor::GetUnit(*_player, guid );
    if(!unit)
        return;

    if(FactionTemplateEntry const* factionTemplateEntry = sFactionTemplateStore.LookupEntry(unit->getFaction()))
        _player->GetReputationMgr().SetVisible(factionTemplateEntry);
}

void WorldSession::HandleStandStateChangeOpcode( WorldPacket & recv_data )
{
    // sLog.outDebug( "WORLD: Received CMSG_STANDSTATECHANGE"  ); -- too many spam in log at lags/debug stop
    uint32 animstate;
    recv_data >> animstate;

    _player->SetStandState(animstate);
}

void WorldSession::HandleFriendListOpcode( WorldPacket & recv_data )
{
    sLog.outDebug( "WORLD: Received CMSG_FRIEND_LIST" );
    _player->GetSocial()->SendFriendList();
}

void WorldSession::HandleAddFriendOpcode( WorldPacket & recv_data )
{
    sLog.outDebug( "WORLD: Received CMSG_ADD_FRIEND" );

    std::string friendName = GetMangosString(LANG_FRIEND_IGNORE_UNKNOWN);

    recv_data >> friendName;

    if(!normalizePlayerName(friendName))
        return;

    CharacterDatabase.escape_string(friendName);            // prevent SQL injection - normal name don't must changed by this call

    sLog.outDebug( "WORLD: %s asked to add friend : '%s'",
        GetPlayer()->GetName(), friendName.c_str() );

    CharacterDatabase.AsyncPQuery(&WorldSession::HandleAddFriendOpcodeCallBack, GetAccountId(), "SELECT guid, race FROM characters WHERE name = '%s'", friendName.c_str());
}

void WorldSession::HandleAddFriendOpcodeCallBack(QueryResult *result, uint32 accountId)
{
    if(!result)
        return;

    uint64 friendGuid = MAKE_NEW_GUID((*result)[0].GetUInt32(), 0, HIGHGUID_PLAYER);
    uint32 team = Player::TeamForRace((*result)[1].GetUInt8());

    delete result;

    WorldSession * session = sWorld.FindSession(accountId);
    if(!session || !session->GetPlayer())
        return;

    FriendsResult friendResult = FRIEND_NOT_FOUND;
    if(friendGuid)
    {
        if(friendGuid==session->GetPlayer()->GetGUID())
            friendResult = FRIEND_SELF;
        else if(session->GetPlayer()->GetTeam() != team && !sWorld.getConfig(CONFIG_ALLOW_TWO_SIDE_ADD_FRIEND) && session->GetSecurity() < SEC_MODERATOR)
            friendResult = FRIEND_ENEMY;
        else if(session->GetPlayer()->GetSocial()->HasFriend(GUID_LOPART(friendGuid)))
            friendResult = FRIEND_ALREADY;
        else
        {
            Player* pFriend = ObjectAccessor::FindPlayer(friendGuid);
            if( pFriend && pFriend->IsInWorld() && pFriend->IsVisibleGloballyFor(session->GetPlayer()))
                friendResult = FRIEND_ADDED_ONLINE;
            else
                friendResult = FRIEND_ADDED_OFFLINE;

            if(!session->GetPlayer()->GetSocial()->AddToSocialList(GUID_LOPART(friendGuid), false))
            {
                friendResult = FRIEND_LIST_FULL;
                sLog.outDebug( "WORLD: %s's friend list is full.", session->GetPlayer()->GetName());
            }
        }
    }

    sSocialMgr.SendFriendStatus(session->GetPlayer(), friendResult, GUID_LOPART(friendGuid), false);

    sLog.outDebug( "WORLD: Sent (SMSG_FRIEND_STATUS)" );
}

void WorldSession::HandleDelFriendOpcode( WorldPacket & recv_data )
{
    uint64 FriendGUID;

    sLog.outDebug( "WORLD: Received CMSG_DEL_FRIEND" );

    recv_data >> FriendGUID;

    _player->GetSocial()->RemoveFromSocialList(GUID_LOPART(FriendGUID), false);

    sSocialMgr.SendFriendStatus(GetPlayer(), FRIEND_REMOVED, GUID_LOPART(FriendGUID), false);

    sLog.outDebug( "WORLD: Sent motd (SMSG_FRIEND_STATUS)" );
}

void WorldSession::HandleAddIgnoreOpcode( WorldPacket & recv_data )
{
    sLog.outDebug( "WORLD: Received CMSG_ADD_IGNORE" );

    std::string IgnoreName = GetMangosString(LANG_FRIEND_IGNORE_UNKNOWN);

    recv_data >> IgnoreName;

    if(!normalizePlayerName(IgnoreName))
        return;

    CharacterDatabase.escape_string(IgnoreName);            // prevent SQL injection - normal name don't must changed by this call

    sLog.outDebug( "WORLD: %s asked to Ignore: '%s'",
        GetPlayer()->GetName(), IgnoreName.c_str() );

    CharacterDatabase.AsyncPQuery(&WorldSession::HandleAddIgnoreOpcodeCallBack, GetAccountId(), "SELECT guid FROM characters WHERE name = '%s'", IgnoreName.c_str());
}

void WorldSession::HandleAddIgnoreOpcodeCallBack(QueryResult *result, uint32 accountId)
{
    if(!result)
        return;

    uint64 IgnoreGuid = MAKE_NEW_GUID((*result)[0].GetUInt32(), 0, HIGHGUID_PLAYER);

    delete result;

    WorldSession * session = sWorld.FindSession(accountId);
    if(!session || !session->GetPlayer())
        return;

    FriendsResult ignoreResult = FRIEND_IGNORE_NOT_FOUND;
    if(IgnoreGuid)
    {
        if(IgnoreGuid==session->GetPlayer()->GetGUID())              //not add yourself
            ignoreResult = FRIEND_IGNORE_SELF;
        else if( session->GetPlayer()->GetSocial()->HasIgnore(GUID_LOPART(IgnoreGuid)) )
            ignoreResult = FRIEND_IGNORE_ALREADY;
        else
        {
            ignoreResult = FRIEND_IGNORE_ADDED;

            // ignore list full
            if(!session->GetPlayer()->GetSocial()->AddToSocialList(GUID_LOPART(IgnoreGuid), true))
                ignoreResult = FRIEND_IGNORE_FULL;
        }
    }

    sSocialMgr.SendFriendStatus(session->GetPlayer(), ignoreResult, GUID_LOPART(IgnoreGuid), false);

    sLog.outDebug( "WORLD: Sent (SMSG_FRIEND_STATUS)" );
}

void WorldSession::HandleDelIgnoreOpcode( WorldPacket & recv_data )
{
    uint64 IgnoreGUID;

    sLog.outDebug( "WORLD: Received CMSG_DEL_IGNORE" );

    recv_data >> IgnoreGUID;

    _player->GetSocial()->RemoveFromSocialList(GUID_LOPART(IgnoreGUID), true);

    sSocialMgr.SendFriendStatus(GetPlayer(), FRIEND_IGNORE_REMOVED, GUID_LOPART(IgnoreGUID), false);

    sLog.outDebug( "WORLD: Sent motd (SMSG_FRIEND_STATUS)" );
}

void WorldSession::HandleBugOpcode( WorldPacket & recv_data )
{
    uint32 suggestion, contentlen;
    std::string content;
    uint32 typelen;
    std::string type;

    recv_data >> suggestion >> contentlen >> content;

    recv_data >> typelen >> type;

    if( suggestion == 0 )
        sLog.outDebug( "WORLD: Received CMSG_BUG [Bug Report]" );
    else
        sLog.outDebug( "WORLD: Received CMSG_BUG [Suggestion]" );

    sLog.outDebug("%s", type.c_str( ) );
    sLog.outDebug("%s", content.c_str( ) );

    CharacterDatabase.escape_string(type);
    CharacterDatabase.escape_string(content);
    CharacterDatabase.PExecute ("INSERT INTO bugreport (type,content) VALUES('%s', '%s')", type.c_str( ), content.c_str( ));
}

void WorldSession::HandleReclaimCorpseOpcode(WorldPacket &recv_data)
{
    sLog.outDetail("WORLD: Received CMSG_RECLAIM_CORPSE");
    if (GetPlayer()->isAlive())
        return;

    // body not released yet
    if(!GetPlayer()->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;

    Corpse *corpse = GetPlayer()->GetCorpse();

    if (!corpse )
        return;

    // prevent resurrect before 30-sec delay after body release not finished
    if(corpse->GetGhostTime() + GetPlayer()->GetCorpseReclaimDelay(corpse->GetType()==CORPSE_RESURRECTABLE_PVP) > time(NULL))
        return;

    if (!corpse->IsWithinDist(GetPlayer(), CORPSE_RECLAIM_RADIUS, true))
        return;

    uint64 guid;
    recv_data >> guid;

    // resurrect
    GetPlayer()->ResurrectPlayer(GetPlayer()->InBattleGround() ? 1.0f : 0.5f);

    // spawn bones
    GetPlayer()->SpawnCorpseBones();

    GetPlayer()->SaveToDB();
}

void WorldSession::HandleResurrectResponseOpcode(WorldPacket & recv_data)
{
    sLog.outDetail("WORLD: Received CMSG_RESURRECT_RESPONSE");

    if(GetPlayer()->isAlive())
        return;

    uint64 guid;
    uint8 status;
    recv_data >> guid;
    recv_data >> status;

    if(status == 0)
    {
        GetPlayer()->clearResurrectRequestData();           // reject
        return;
    }

    if(!GetPlayer()->isRessurectRequestedBy(guid))
        return;

    GetPlayer()->ResurectUsingRequestData();
    GetPlayer()->SaveToDB();
}

void WorldSession::HandleAreaTriggerOpcode(WorldPacket & recv_data)
{
    sLog.outDebug("WORLD: Received CMSG_AREATRIGGER");

    uint32 Trigger_ID;

    recv_data >> Trigger_ID;
    sLog.outDebug("Trigger ID:%u",Trigger_ID);

    if(GetPlayer()->isInFlight())
    {
        sLog.outDebug("Player '%s' (GUID: %u) in flight, ignore Area Trigger ID:%u",GetPlayer()->GetName(),GetPlayer()->GetGUIDLow(), Trigger_ID);
        return;
    }

    AreaTriggerEntry const* atEntry = sAreaTriggerStore.LookupEntry(Trigger_ID);
    if(!atEntry)
    {
        sLog.outDebug("Player '%s' (GUID: %u) send unknown (by DBC) Area Trigger ID:%u",GetPlayer()->GetName(),GetPlayer()->GetGUIDLow(), Trigger_ID);
        return;
    }

    if (GetPlayer()->GetMapId()!=atEntry->mapid)
    {
        sLog.outDebug("Player '%s' (GUID: %u) too far (trigger map: %u player map: %u), ignore Area Trigger ID: %u", GetPlayer()->GetName(), atEntry->mapid, GetPlayer()->GetMapId(), GetPlayer()->GetGUIDLow(), Trigger_ID);
        return;
    }

    // delta is safe radius
    const float delta = 5.0f;
    // check if player in the range of areatrigger
    Player* pl = GetPlayer();

    if (atEntry->radius > 0)
    {
        // if we have radius check it
        float dist = pl->GetDistance(atEntry->x,atEntry->y,atEntry->z);
        if(dist > atEntry->radius + delta)
        {
            sLog.outDebug("Player '%s' (GUID: %u) too far (radius: %f distance: %f), ignore Area Trigger ID: %u",
                pl->GetName(), pl->GetGUIDLow(), atEntry->radius, dist, Trigger_ID);
            return;
        }
    }
    else
    {
        // we have only extent

        // rotate the players position instead of rotating the whole cube, that way we can make a simplified
        // is-in-cube check and we have to calculate only one point instead of 4

        // 2PI = 360°, keep in mind that ingame orientation is counter-clockwise
        double rotation = 2*M_PI-atEntry->box_orientation;
        double sinVal = sin(rotation);
        double cosVal = cos(rotation);

        float playerBoxDistX = pl->GetPositionX() - atEntry->x;
        float playerBoxDistY = pl->GetPositionY() - atEntry->y;

        float rotPlayerX = atEntry->x + playerBoxDistX * cosVal - playerBoxDistY*sinVal;
        float rotPlayerY = atEntry->y + playerBoxDistY * cosVal + playerBoxDistX*sinVal;

        // box edges are parallel to coordiante axis, so we can treat every dimension independently :D
        float dz = pl->GetPositionZ() - atEntry->z;
        float dx = rotPlayerX - atEntry->x;
        float dy = rotPlayerY - atEntry->y;
        if( (fabs(dx) > atEntry->box_x/2 + delta) ||
            (fabs(dy) > atEntry->box_y/2 + delta) ||
            (fabs(dz) > atEntry->box_z/2 + delta) )
        {
            sLog.outDebug("Player '%s' (GUID: %u) too far (1/2 box X: %f 1/2 box Y: %f 1/2 box Z: %f rotatedPlayerX: %f rotatedPlayerY: %f dZ:%f), ignore Area Trigger ID: %u",
                pl->GetName(), pl->GetGUIDLow(), atEntry->box_x/2, atEntry->box_y/2, atEntry->box_z/2, rotPlayerX, rotPlayerY, dz, Trigger_ID);
            return;
        }
    }

    if(Script->scriptAreaTrigger(GetPlayer(), atEntry))
        return;

    uint32 quest_id = sObjectMgr.GetQuestForAreaTrigger( Trigger_ID );
    if( quest_id && GetPlayer()->isAlive() && GetPlayer()->IsActiveQuest(quest_id) )
    {
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(quest_id);
        if( pQuest )
        {
            if(GetPlayer()->GetQuestStatus(quest_id) == QUEST_STATUS_INCOMPLETE)
                GetPlayer()->AreaExploredOrEventHappens( quest_id );
        }
    }

    if(sObjectMgr.IsTavernAreaTrigger(Trigger_ID))
    {
        // set resting flag we are in the inn
        GetPlayer()->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING);
        GetPlayer()->InnEnter(time(NULL), atEntry->mapid, atEntry->x, atEntry->y, atEntry->z);
        GetPlayer()->SetRestType(REST_TYPE_IN_TAVERN);

        if(sWorld.IsFFAPvPRealm())
            GetPlayer()->SetFFAPvP(false);

        return;
    }

    if(GetPlayer()->InBattleGround())
    {
        BattleGround* bg = GetPlayer()->GetBattleGround();
        if(bg)
            bg->HandleAreaTrigger(GetPlayer(), Trigger_ID);
        return;
    }

    // NULL if all values default (non teleport trigger)
    AreaTrigger const* at = sObjectMgr.GetAreaTrigger(Trigger_ID);
    if(!at)
        return;

    if(!GetPlayer()->isGameMaster())
    {
        uint32 missingLevel = 0;
        if(GetPlayer()->getLevel() < at->requiredLevel && !sWorld.getConfig(CONFIG_INSTANCE_IGNORE_LEVEL))
            missingLevel = at->requiredLevel;

        // must have one or the other, report the first one that's missing
        uint32 missingItem = 0;
        if(at->requiredItem)
        {
            if(!GetPlayer()->HasItemCount(at->requiredItem, 1) &&
                (!at->requiredItem2 || !GetPlayer()->HasItemCount(at->requiredItem2, 1)))
                missingItem = at->requiredItem;
        }
        else if(at->requiredItem2 && !GetPlayer()->HasItemCount(at->requiredItem2, 1))
            missingItem = at->requiredItem2;

        uint32 missingQuest = 0;
        if(at->requiredQuest && !GetPlayer()->GetQuestRewardStatus(at->requiredQuest))
            missingQuest = at->requiredQuest;

        if(missingLevel || missingItem || missingQuest)
        {
            // TODO: all this is probably wrong
            if(missingItem)
                SendAreaTriggerMessage(GetMangosString(LANG_LEVEL_MINREQUIRED_AND_ITEM), at->requiredLevel, ObjectMgr::GetItemPrototype(missingItem)->Name1);
            /*[-ZERO] else if(missingKey)
                GetPlayer()->SendTransferAborted(at->target_mapId,0); */
            else if(missingQuest)
                SendAreaTriggerMessage("%s", at->requiredFailedText.c_str());
            else if(missingLevel)
                SendAreaTriggerMessage(GetMangosString(LANG_LEVEL_MINREQUIRED), missingLevel);
            return;
        }
    }

    GetPlayer()->TeleportTo(at->target_mapId,at->target_X,at->target_Y,at->target_Z,at->target_Orientation,TELE_TO_NOT_LEAVE_TRANSPORT);
}

void WorldSession::HandleUpdateAccountData(WorldPacket & recv_data)
{
    sLog.outDetail("WORLD: Received CMSG_UPDATE_ACCOUNT_DATA");
    recv_data.rpos(recv_data.wpos());                       // prevent spam at unimplemented packet
    //recv_data.hexlike();
}

void WorldSession::HandleRequestAccountData(WorldPacket& /*recv_data*/)
{
    sLog.outDetail("WORLD: Received CMSG_REQUEST_ACCOUNT_DATA");
    //recv_data.hexlike();
}

void WorldSession::HandleSetActionButtonOpcode(WorldPacket& recv_data)
{
    sLog.outDebug(  "WORLD: Received CMSG_SET_ACTION_BUTTON" );
    uint8 button;
    uint32 packetData;
    recv_data >> button >> packetData;

    uint32 action = ACTION_BUTTON_ACTION(packetData);
    uint8  type   = ACTION_BUTTON_TYPE(packetData);

    sLog.outDetail( "BUTTON: %u ACTION: %u TYPE: %u", button, action, type );
    if (!packetData)
    {
        sLog.outDetail( "MISC: Remove action from button %u", button );
        GetPlayer()->removeActionButton(button);
    }
    else
    {
        switch(type)
        {
            case ACTION_BUTTON_MACRO:
            case ACTION_BUTTON_CMACRO:
                sLog.outDetail( "MISC: Added Macro %u into button %u", action, button );
                break;
            case ACTION_BUTTON_SPELL:
                sLog.outDetail( "MISC: Added Spell %u into button %u", action, button );
                break;
            case ACTION_BUTTON_ITEM:
                sLog.outDetail( "MISC: Added Item %u into button %u", action, button );
                break;
            default:
                sLog.outError( "MISC: Unknown action button type %u for action %u into button %u", type, action, button );
                return;
        }
        GetPlayer()->addActionButton(button,action,type);
    }
}

void WorldSession::HandleCompleteCinematic( WorldPacket & /*recv_data*/ )
{
    DEBUG_LOG( "WORLD: Player is watching cinema" );
}

void WorldSession::HandleNextCinematicCamera( WorldPacket & /*recv_data*/ )
{
    DEBUG_LOG( "WORLD: Which movie to play" );
}

void WorldSession::HandleMoveTimeSkippedOpcode( WorldPacket & recv_data )
{
    /*  WorldSession::Update( getMSTime() );*/
    DEBUG_LOG( "WORLD: Time Lag/Synchronization Resent/Update" );

    recv_data.read_skip<uint64>();
    recv_data.read_skip<uint32>();
    /*
        uint64 guid;
        uint32 time_skipped;
        recv_data >> guid;
        recv_data >> time_skipped;
        sLog.outDebug( "WORLD: CMSG_MOVE_TIME_SKIPPED" );

        /// TODO
        must be need use in mangos
        We substract server Lags to move time ( AntiLags )
        for exmaple
        GetPlayer()->ModifyLastMoveTime( -int32(time_skipped) );
    */
}

void WorldSession::HandleFeatherFallAck(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: CMSG_MOVE_FEATHER_FALL_ACK");

    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
}

void WorldSession::HandleMoveUnRootAck(WorldPacket& recv_data)
{
    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
/*
    uint64 guid;
    recv_data >> guid;

    // now can skip not our packet
    if(_player->GetGUID() != guid)
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    sLog.outDebug( "WORLD: CMSG_FORCE_MOVE_UNROOT_ACK" );

    recv_data.read_skip<uint32>();                          // unk

    MovementInfo movementInfo;
    ReadMovementInfo(recv_data, &movementInfo);
*/
}

void WorldSession::HandleMoveRootAck(WorldPacket& recv_data)
{
    // no used
    recv_data.rpos(recv_data.wpos());                       // prevent warnings spam
/*
    uint64 guid;
    recv_data >> guid;

    // now can skip not our packet
    if(_player->GetGUID() != guid)
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    sLog.outDebug( "WORLD: CMSG_FORCE_MOVE_ROOT_ACK" );

    recv_data.read_skip<uint32>();                          // unk

    MovementInfo movementInfo;
    ReadMovementInfo(recv_data, &movementInfo);
*/
}

void WorldSession::HandleSetActionBarToggles(WorldPacket& recv_data)
{
    uint8 ActionBar;

    recv_data >> ActionBar;

    if(!GetPlayer())                                        // ignore until not logged (check needed because STATUS_AUTHED)
    {
        if(ActionBar!=0)
            sLog.outError("WorldSession::HandleSetActionBarToggles in not logged state with value: %u, ignored",uint32(ActionBar));
        return;
    }

    GetPlayer()->SetByteValue(PLAYER_FIELD_BYTES, 2, ActionBar);
}

void WorldSession::HandleWardenDataOpcode(WorldPacket& recv_data)
{
    recv_data.read_skip<uint8>();
    /*
        uint8 tmp;
        recv_data >> tmp;
        sLog.outDebug("Received opcode CMSG_WARDEN_DATA, not resolve.uint8 = %u",tmp);
    */
}

void WorldSession::HandlePlayedTime(WorldPacket& /*recv_data*/)
{
    WorldPacket data(SMSG_PLAYED_TIME, 4 + 4);
    data << uint32(_player->GetTotalPlayedTime());
    data << uint32(_player->GetLevelPlayedTime());
    SendPacket(&data);
}

void WorldSession::HandleInspectOpcode(WorldPacket& recv_data)
{
    uint64 guid;
    recv_data >> guid;
    DEBUG_LOG("Inspected guid is (GUID: %u TypeId: %u)", GUID_LOPART(guid), GuidHigh2TypeId(GUID_HIPART(guid)));

    _player->SetSelection(guid);

    Player *plr = sObjectMgr.GetPlayer(guid);
    if(!plr)                                                // wrong player
        return;

    WorldPacket data( SMSG_INSPECT, 8 );
    data << guid;
    SendPacket(&data);
}

void WorldSession::HandleInspectHonorStatsOpcode(WorldPacket& recv_data)
{
    Player *pl;
    uint64 guid;
    recv_data >> guid;
    // DEBUG_LOG("Party Stats guid is " I64FMTD,guid);

    pl = sObjectMgr.GetPlayer(guid);
    if(pl)
    {
        WorldPacket data( MSG_INSPECT_HONOR_STATS, (8+1+4+4+4+4+4+4+4+4+4+4+1) );
        data << guid;                                       // player guid
                                                            // Rank, filling bar, PLAYER_BYTES_3, ??
        data << (uint8)pl->GetUInt32Value(PLAYER_FIELD_BYTES2);
                                                            // Today Honorable and Dishonorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_SESSION_KILLS);
                                                            // Yesterday Honorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_YESTERDAY_KILLS);
                                                            // Last Week Honorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_KILLS);
                                                            // This Week Honorable kills
        data << pl->GetUInt32Value(PLAYER_FIELD_THIS_WEEK_KILLS);
                                                            // Lifetime Honorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);
                                                            // Lifetime Dishonorable Kills
        data << pl->GetUInt32Value(PLAYER_FIELD_LIFETIME_DISHONORABLE_KILLS);
                                                            // Yesterday Honor
        data << pl->GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION);
                                                            // Last Week Honor
        data << pl->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_CONTRIBUTION);
                                                            // This Week Honor
        data << pl->GetUInt32Value(PLAYER_FIELD_THIS_WEEK_CONTRIBUTION);
                                                            // Last Week Standing
        data << pl->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_RANK);
        data << (uint8)pl->GetHonorHighestRank();           // Highest Rank, ??
        SendPacket(&data);
    }
    else
        sLog.outDebug("Player %u not found!", guid);
}

void WorldSession::HandleWorldTeleportOpcode(WorldPacket& recv_data)
{
    // write in client console: worldport 469 452 6454 2536 180 or /console worldport 469 452 6454 2536 180
    // Received opcode CMSG_WORLD_TELEPORT
    // Time is ***, map=469, x=452.000000, y=6454.000000, z=2536.000000, orient=3.141593

    uint32 time;
    uint32 mapid;
    float PositionX;
    float PositionY;
    float PositionZ;
    float Orientation;

    recv_data >> time;                                      // time in m.sec.
    recv_data >> mapid;
    recv_data >> PositionX;
    recv_data >> PositionY;
    recv_data >> PositionZ;
    recv_data >> Orientation;                               // o (3.141593 = 180 degrees)

    //sLog.outDebug("Received opcode CMSG_WORLD_TELEPORT");

    if(GetPlayer()->isInFlight())
    {
        sLog.outDebug("Player '%s' (GUID: %u) in flight, ignore worldport command.",GetPlayer()->GetName(),GetPlayer()->GetGUIDLow());
        return;
    }

    DEBUG_LOG("Time %u sec, map=%u, x=%f, y=%f, z=%f, orient=%f", time/1000, mapid, PositionX, PositionY, PositionZ, Orientation);

    if (GetSecurity() >= SEC_ADMINISTRATOR)
        GetPlayer()->TeleportTo(mapid,PositionX,PositionY,PositionZ,Orientation);
    else
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
    sLog.outDebug("Received worldport command from player %s", GetPlayer()->GetName());
}

void WorldSession::HandleWhoisOpcode(WorldPacket& recv_data)
{
    sLog.outDebug("Received opcode CMSG_WHOIS");
    std::string charname;
    recv_data >> charname;

    if (GetSecurity() < SEC_ADMINISTRATOR)
    {
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
        return;
    }

    if(charname.empty() || !normalizePlayerName (charname))
    {
        SendNotification(LANG_NEED_CHARACTER_NAME);
        return;
    }

    Player *plr = sObjectMgr.GetPlayer(charname.c_str());

    if(!plr)
    {
        SendNotification(LANG_PLAYER_NOT_EXIST_OR_OFFLINE, charname.c_str());
        return;
    }

    uint32 accid = plr->GetSession()->GetAccountId();

    QueryResult *result = loginDatabase.PQuery("SELECT username,email,last_ip FROM account WHERE id=%u", accid);
    if(!result)
    {
        SendNotification(LANG_ACCOUNT_FOR_PLAYER_NOT_FOUND, charname.c_str());
        return;
    }

    Field *fields = result->Fetch();
    std::string acc = fields[0].GetCppString();
    if(acc.empty())
        acc = "Unknown";
    std::string email = fields[1].GetCppString();
    if(email.empty())
        email = "Unknown";
    std::string lastip = fields[2].GetCppString();
    if(lastip.empty())
        lastip = "Unknown";

    std::string msg = charname + "'s " + "account is " + acc + ", e-mail: " + email + ", last ip: " + lastip;

    WorldPacket data(SMSG_WHOIS, msg.size()+1);
    data << msg;
    _player->GetSession()->SendPacket(&data);

    delete result;

    sLog.outDebug("Received whois command from player %s for character %s", GetPlayer()->GetName(), charname.c_str());
}

void WorldSession::HandleFarSightOpcode( WorldPacket & recv_data )
{
    sLog.outDebug("WORLD: CMSG_FAR_SIGHT");
    //recv_data.hexlike();

    uint8 unk;
    recv_data >> unk;

    switch(unk)
    {
        case 0:
            //WorldPacket data(SMSG_CLEAR_FAR_SIGHT_IMMEDIATE, 0)
            //SendPacket(&data);
            //_player->SetUInt64Value(PLAYER_FARSIGHT, 0);
            sLog.outDebug("Removed FarSight from player %u", _player->GetGUIDLow());
            break;
        case 1:
            sLog.outDebug("Added FarSight (GUID:%u TypeId:%u) to player %u", GUID_LOPART(_player->GetFarSight()), GuidHigh2TypeId(GUID_HIPART(_player->GetFarSight())), _player->GetGUIDLow());
            break;
    }
}

void WorldSession::HandleResetInstancesOpcode( WorldPacket & /*recv_data*/ )
{
    sLog.outDebug("WORLD: CMSG_RESET_INSTANCES");
    Group *pGroup = _player->GetGroup();
    if(pGroup)
    {
        if(pGroup->IsLeader(_player->GetGUID()))
            pGroup->ResetInstances(INSTANCE_RESET_ALL, _player);
    }
    else
        _player->ResetInstances(INSTANCE_RESET_ALL);
}

void WorldSession::HandleTimeSyncResp( WorldPacket & recv_data )
{
    sLog.outDebug("CMSG_TIME_SYNC_RESP");

    uint32 counter, time_;
    recv_data >> counter >> time_;

    // time_ seems always more than getMSTime()
    uint32 diff = getMSTimeDiff(getMSTime(),time_);

    sLog.outDebug("response sent: counter %u, time %u (HEX: %X), ms. time %u, diff %u", counter, time_, time_, getMSTime(), diff);
}

void WorldSession::HandleCancelMountAuraOpcode( WorldPacket & /*recv_data*/ )
{
    sLog.outDebug("WORLD: CMSG_CANCEL_MOUNT_AURA");
    //recv_data.hexlike();

    //If player is not mounted, so go out :)
    if (!_player->IsMounted())                              // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_CHAR_NON_MOUNTED);
        return;
    }

    if(_player->isInFlight())                               // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_YOU_IN_FLIGHT);
        return;
    }

    _player->Unmount();
    _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
}

void WorldSession::HandleRequestPetInfoOpcode( WorldPacket & /*recv_data */)
{
    /*
        sLog.outDebug("WORLD: CMSG_REQUEST_PET_INFO");
        recv_data.hexlike();
    */
}

void WorldSession::HandleSetTaxiBenchmarkOpcode( WorldPacket & recv_data )
{
    uint8 mode;
    recv_data >> mode;

    sLog.outDebug("Client used \"/timetest %d\" command", mode);
}
