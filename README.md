# mod-tofu-bot-party-exit

Tofu Worldview module for AzerothCore with mod-playerbots (Tofu fork `cdenq/wow-tofuworld-playerbots`, branch `tofu`). It decides what a playerbot does after it leaves a group that a real player led, because stock playerbots keeps the real player as the bot's master once the bot has no group, so the bot says goodbye and then keeps following.

After `TofuBotPartyExit.DelaySeconds` (default 5) the bot is handled by type:

| Bot | Result |
|---|---|
| Random bot | Drops its master, resets to random-bot strategies (new rpg, quest, grind), and teleports back to where it stood when it joined the group (skipped when dead, on a flight path, or it joined inside an instance) |
| Class bot (`.playerbots bot addclass`) | Logs out |
| Altbot summoned on demand (`.playerbots bot add`, account, guild, or linked account) | Logs out |
| Enrolled autonomous alt (mod-autonomous-altbots) | Untouched; that module resumes it on its own |

Nothing happens if, by then, the bot is in a group again (re-invited), is in a battleground or arena, is a SelfBot, or never served a real player (bot-only groups). It covers a kick, the bot leaving, and the group disbanding when the leader leaves.

## Install

1. Clone into `modules/mod-tofu-bot-party-exit` (the folder name sets the loader `Addmod_tofu_bot_party_exitScripts`).
2. Re-run CMake and build worldserver.
3. Copy `conf/mod_tofu_bot_party_exit.conf.dist` to `configs/modules/mod_tofu_bot_party_exit.conf` and set `TofuBotPartyExit.Enable = 1`.

## Config

| Key | Default | Meaning |
|---|---|---|
| `TofuBotPartyExit.Enable` | 0 | Turn the module on |
| `TofuBotPartyExit.DelaySeconds` | 5 | Wait before acting |
| `TofuBotPartyExit.RandomBots.ReleaseMaster` | 1 | Release random bots |
| `TofuBotPartyExit.RandomBots.TeleportBack` | 1 | Teleport released random bots back to where they joined |
| `TofuBotPartyExit.AddclassBots.Logout` | 1 | Log out class bots |
| `TofuBotPartyExit.AltBots.Logout` | 1 | Log out on-demand altbots |
| `TofuBotPartyExit.VerboseLogging` | 0 | Also log skipped bots and why |

Every action is logged as `mod-tofu-bot-party-exit: <name> (<type>) left the group: ...`.

## Hooks

`GroupScript::OnAddMember` (records a random bot's position), `OnRemoveMember` and `OnDisband` (queue the bot), `WorldScript::OnUpdate` (acts after the delay), `PlayerScript::OnPlayerLogout` (cleanup). No core patch, no SQL.
