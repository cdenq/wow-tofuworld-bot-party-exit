/*
 * mod-tofu-bot-party-exit (Tofu Worldview)
 *
 * What a playerbot does after it leaves a group that a real player led. Stock mod-playerbots keeps the real player
 * as the bot's master when the bot has no group left (ResetAiAction only clears the master when the bot is in a
 * different group), so every bot keeps following its old leader. This module, after TofuBotPartyExit.DelaySeconds:
 * - random bot: drops the master, resets to random-bot strategies, and teleports back to where it joined the group;
 * - class bot (addclass): logs out;
 * - altbot summoned on demand: logs out;
 * - enrolled autonomous alt (mod-autonomous-altbots): untouched, that module already resumes it on its own.
 * Nothing happens when the bot is in a group again by then, is in a battleground or arena, is a SelfBot, or did not
 * serve a real player (bot-only groups).
 *
 * The group hooks only record GUIDs; all actions run in WorldScript::OnUpdate on the world thread.
 */

#include "Config.h"
#include "DBCStores.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "TofuAutonomousAlt.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
constexpr char LOG_PREFIX[] = "mod-tofu-bot-party-exit:";

struct ModuleConfig
{
    bool enable = false;
    uint32 delayMs = 5000;
    bool randomRelease = true;
    bool randomTeleportBack = true;
    bool addclassLogout = true;
    bool altLogout = true;
    bool verbose = false;
};

ModuleConfig g_Config;

std::mutex g_Mutex;
// Bot GUID (raw) -> milliseconds left before it is handled.
std::unordered_map<uint64, uint32> g_Pending;
// Random bot GUID (raw) -> where it stood when it last joined a group.
std::unordered_map<uint64, WorldLocation> g_JoinPositions;

void LoadConfig()
{
    g_Config.enable = sConfigMgr->GetOption<bool>("TofuBotPartyExit.Enable", false);
    g_Config.delayMs = sConfigMgr->GetOption<uint32>("TofuBotPartyExit.DelaySeconds", 5) * 1000;
    g_Config.randomRelease = sConfigMgr->GetOption<bool>("TofuBotPartyExit.RandomBots.ReleaseMaster", true);
    g_Config.randomTeleportBack = sConfigMgr->GetOption<bool>("TofuBotPartyExit.RandomBots.TeleportBack", true);
    g_Config.addclassLogout = sConfigMgr->GetOption<bool>("TofuBotPartyExit.AddclassBots.Logout", true);
    g_Config.altLogout = sConfigMgr->GetOption<bool>("TofuBotPartyExit.AltBots.Logout", true);
    g_Config.verbose = sConfigMgr->GetOption<bool>("TofuBotPartyExit.VerboseLogging", false);

    if (!g_Config.enable)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Pending.clear();
        g_JoinPositions.clear();
    }
}

bool IsBattleGroup(Group const* group)
{
    return group && (group->isBGGroup() || group->isBFGroup());
}

void Skip(Player* bot, char const* reason)
{
    if (g_Config.verbose)
        LOG_INFO("module", "{} {} skipped: {}", LOG_PREFIX, bot->GetName(), reason);
}

// Called from the group hooks for every member that left. Queues it if it is a playerbot whose master is a real player.
void NoteLeft(ObjectGuid guid)
{
    Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
    if (!bot)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || IsSelfBot(bot))
        return;

    Player* master = botAI->GetMaster();
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (!master || master == bot || !IsRealPlayer(master))
    {
        // A bot-only group: forget its join position so a later group with a real player records a fresh one.
        g_JoinPositions.erase(guid.GetRawValue());
        return;
    }

    g_Pending[guid.GetRawValue()] = g_Config.delayMs;
}

bool LogoutBot(Player* bot, Player* master)
{
    ObjectGuid const guid = bot->GetGUID();
    if (master)
    {
        if (PlayerbotMgr* masterMgr = GET_PLAYERBOT_MGR(master))
        {
            if (masterMgr->GetPlayerBot(guid))
            {
                masterMgr->LogoutPlayerBot(guid);
                return true;
            }
        }
    }

    if (sRandomPlayerbotMgr.GetPlayerBot(guid))
    {
        sRandomPlayerbotMgr.LogoutPlayerBot(guid);
        return true;
    }

    return false;
}

void ReleaseRandomBot(Player* bot, PlayerbotAI* botAI, uint64 rawGuid)
{
    botAI->SetMaster(nullptr);
    botAI->Reset(true);
    botAI->ResetStrategies();

    bool hasPosition = false;
    WorldLocation position;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto itr = g_JoinPositions.find(rawGuid);
        if (itr != g_JoinPositions.end())
        {
            position = itr->second;
            hasPosition = true;
            g_JoinPositions.erase(itr);
        }
    }

    char const* teleport = "not teleported (no join position)";
    if (!g_Config.randomTeleportBack)
        teleport = "not teleported (TeleportBack = 0)";
    else if (hasPosition)
    {
        MapEntry const* mapEntry = sMapStore.LookupEntry(position.GetMapId());
        if (!bot->IsAlive())
            teleport = "not teleported (dead)";
        else if (bot->IsInFlight())
            teleport = "not teleported (on a flight path)";
        else if (!mapEntry || mapEntry->Instanceable())
            teleport = "not teleported (joined inside an instance)";
        else
        {
            bot->CombatStop(true);
            if (bot->TeleportTo(position.GetMapId(), position.GetPositionX(), position.GetPositionY(),
                                position.GetPositionZ(), position.GetOrientation()))
                teleport = "teleported back to where it joined";
            else
                teleport = "teleport back failed";
        }
    }

    LOG_INFO("module", "{} {} (random bot) left the group: released, {}", LOG_PREFIX, bot->GetName(), teleport);
}

void Handle(uint64 rawGuid)
{
    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(rawGuid));
    if (!bot || !bot->IsInWorld())
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_JoinPositions.erase(rawGuid);
        return;
    }

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || IsSelfBot(bot))
        return;

    if (TofuAutonomousAlt::IsActive(bot))
        return Skip(bot, "enrolled autonomous alt (mod-autonomous-altbots resumes it)");

    if (bot->GetGroup())
        return Skip(bot, "in a group again");

    if (bot->InBattleground() || bot->InArena())
        return Skip(bot, "in a battleground or arena");

    Player* master = botAI->GetMaster();
    if (master && master != bot && !IsRealPlayer(master))
        return Skip(bot, "its master is now a bot");

    if (sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        if (!g_Config.randomRelease)
            return Skip(bot, "RandomBots.ReleaseMaster = 0");

        ReleaseRandomBot(bot, botAI, rawGuid);
        return;
    }

    bool const addclass = sRandomPlayerbotMgr.IsAddclassBot(bot);
    if (addclass ? !g_Config.addclassLogout : !g_Config.altLogout)
        return Skip(bot, addclass ? "AddclassBots.Logout = 0" : "AltBots.Logout = 0");

    std::string const name = bot->GetName();
    if (LogoutBot(bot, master))
        LOG_INFO("module", "{} {} ({}) left the group: logged out", LOG_PREFIX, name, addclass ? "class bot" : "altbot");
    else
        LOG_INFO("module", "{} {} ({}) left the group: no bot manager holds it, not logged out", LOG_PREFIX, name,
                 addclass ? "class bot" : "altbot");
}

class TofuBotPartyExitGroupScript : public GroupScript
{
public:
    TofuBotPartyExitGroupScript()
        : GroupScript("TofuBotPartyExitGroupScript",
                      { GROUPHOOK_ON_ADD_MEMBER, GROUPHOOK_ON_REMOVE_MEMBER, GROUPHOOK_ON_DISBAND })
    {
    }

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (!g_Config.enable || IsBattleGroup(group))
            return;

        Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot) || !sRandomPlayerbotMgr.IsRandomBot(bot))
            return;

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_JoinPositions[guid.GetRawValue()] = WorldLocation(bot->GetMapId(), bot->GetPositionX(),
                                                            bot->GetPositionY(), bot->GetPositionZ(),
                                                            bot->GetOrientation());
        g_Pending.erase(guid.GetRawValue());
    }

    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod /*method*/, ObjectGuid /*kicker*/,
                        char const* /*reason*/) override
    {
        if (!g_Config.enable || IsBattleGroup(group))
            return;

        NoteLeft(guid);
    }

    void OnDisband(Group* group) override
    {
        if (!g_Config.enable || IsBattleGroup(group))
            return;

        // Called before the members are removed, so the slots still list everyone.
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
            NoteLeft(slot.guid);
    }
};

class TofuBotPartyExitWorldScript : public WorldScript
{
public:
    TofuBotPartyExitWorldScript()
        : WorldScript("TofuBotPartyExitWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
        LOG_INFO("server.loading", "{} {} (delay {} s)", LOG_PREFIX, g_Config.enable ? "enabled" : "disabled",
                 g_Config.delayMs / 1000);
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_Config.enable)
            return;

        std::vector<uint64> due;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            for (auto itr = g_Pending.begin(); itr != g_Pending.end();)
            {
                if (itr->second <= diff)
                {
                    due.push_back(itr->first);
                    itr = g_Pending.erase(itr);
                }
                else
                {
                    itr->second -= diff;
                    ++itr;
                }
            }
        }

        for (uint64 rawGuid : due)
            Handle(rawGuid);
    }
};

class TofuBotPartyExitPlayerScript : public PlayerScript
{
public:
    TofuBotPartyExitPlayerScript() : PlayerScript("TofuBotPartyExitPlayerScript", { PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Pending.erase(player->GetGUID().GetRawValue());
        g_JoinPositions.erase(player->GetGUID().GetRawValue());
    }
};
} // namespace

void Addmod_tofu_bot_party_exitScripts()
{
    new TofuBotPartyExitGroupScript();
    new TofuBotPartyExitWorldScript();
    new TofuBotPartyExitPlayerScript();
}
