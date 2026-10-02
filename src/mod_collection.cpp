/*
 * mod-collection - account-wide mount and companion collections
 *
 * 3.3.5 has no collection system to make account-wide: a mount or a companion
 * is a spell in one character's spellbook and nothing else. So the account is
 * given a pool, and every character on it is handed what the pool holds.
 *
 * Identifying them needs no guesswork. SkillLineAbility puts every mount in
 * skill line 777 and every companion in 778, and SpellInfo::IsAbilityOfSkillType
 * answers against those. Riding skill is line 762 and no mount or companion
 * appears in it, so sharing a collection cannot leak riding - an alt sees the
 * mounts and still has to buy its own riding before any of them will work.
 *
 * The one real subtlety is class mounts, and it is a trap. A class mount sits
 * in the mounts line AND in its class line, and the two disagree: the paladin
 * chargers carry ClassMask 0x2 in both, but Summon Felsteed and Summon
 * Dreadsteed carry 0x100 only in the Demonology line and 0x00 in the mounts
 * line. Reading the mounts row alone finds four class-locked mounts out of 315;
 * reading every row finds seven, the extra three being the two warlock steeds
 * and Acherus Deathcharger. The core's Player::IsSpellFitByClassAndRace is no
 * help, because it passes a spell the moment ANY row fits and the unrestricted
 * mounts row always fits. CollectionRules.h does it properly.
 *
 * Nothing is written to character_spell. The pooled spells are added with
 * Player::addSpell(id, SPEC_MASK_ALL, true, true) - temporary - which sends the
 * client its learn packet but is skipped by _SaveSpells, so account_collection
 * stays the single record of what has been collected and switching the module
 * off gives every character back exactly the spellbook it owns.
 *
 * addSpell and not learnSpell, and that is not a style choice. For a temporary
 * learn from in world the two send the learn packet twice over: addSpell has a
 * branch of its own for temporary spells that calls SendLearnPacket, and then
 * learnSpell calls SendLearnPacket again on success. The client takes the second
 * packet as a second spell, so every shared mount and companion appeared in the
 * spellbook twice. Calling addSpell directly sends exactly one packet. Nothing
 * else in learnSpell is wanted here either: the rank-chain and
 * requires-this-spell cascades at the end of it mean nothing to a mount, and
 * skipping it also keeps this module out of its own OnPlayerLearnSpell hook.
 */

#include "CollectionRules.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Language.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    struct Config
    {
        bool Enable     = true;
        bool SkipBots   = true;
        bool Announce   = true;
        std::string BotAccountPrefix = "RNDBOT";
        Collection::Policy Policy;
    };

    Config cfg;

    // Every spell that belongs to a collection, with all of its skill line rows.
    // Built once from the DBC at startup, so the hooks are a hash lookup.
    struct Collectable
    {
        Collection::Kind  kind  = Collection::KIND_MOUNT;
        Collection::SkillLines lines;
    };

    std::unordered_map<uint32 /*spellId*/, Collectable> collectables;

    // accountId -> the spells it has collected.
    std::unordered_map<uint32, std::unordered_set<uint32>> pool;

    void LoadConfig()
    {
        cfg.Enable               = sConfigMgr->GetOption<bool>("Collection.Enable", true);
        cfg.SkipBots             = sConfigMgr->GetOption<bool>("Collection.SkipBots", true);
        cfg.Announce             = sConfigMgr->GetOption<bool>("Collection.Announce", true);
        cfg.BotAccountPrefix     = sConfigMgr->GetOption<std::string>("Collection.BotAccountPrefix", "RNDBOT");
        cfg.Policy.mounts        = sConfigMgr->GetOption<bool>("Collection.Mounts", true);
        cfg.Policy.companions    = sConfigMgr->GetOption<bool>("Collection.Companions", true);
        cfg.Policy.crossFaction  = sConfigMgr->GetOption<bool>("Collection.CrossFaction", true);
    }

    // One pass over SkillLineAbility.dbc gathering every row per spell, then a
    // second keeping the spells that appear in a collection line. Both lines of
    // a class mount are needed, which is why the rows cannot simply be filtered
    // on skill line as they are read.
    uint32 BuildCollectables()
    {
        collectables.clear();

        std::unordered_map<uint32, Collection::SkillLines> allLines;

        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* entry = sSkillLineAbilityStore.LookupEntry(i);
            if (!entry || !entry->Spell)
                continue;

            allLines[entry->Spell].push_back({ entry->SkillLine, entry->RaceMask, entry->ClassMask });
        }

        uint32 mounts = 0;
        uint32 companions = 0;

        for (auto& [spellId, lines] : allLines)
        {
            std::optional<Collection::Kind> const kind = Collection::KindOf(lines);
            if (!kind)
                continue;

            // A spell the server does not know about cannot be taught, and a
            // DBC that mentions one is not worth a log line per row.
            if (!sSpellMgr->GetSpellInfo(spellId))
                continue;

            collectables[spellId] = { *kind, std::move(lines) };
            *kind == Collection::KIND_MOUNT ? ++mounts : ++companions;
        }

        LOG_INFO("module", "mod-collection: {} mount(s) and {} companion(s) are collectable.", mounts, companions);
        return uint32(collectables.size());
    }

    uint32 LoadPool()
    {
        pool.clear();

        QueryResult result = CharacterDatabase.Query("SELECT `AccountId`, `SpellId` FROM `account_collection`");
        if (!result)
        {
            LOG_INFO("module", "mod-collection: no collections are recorded yet.");
            return 0;
        }

        uint32 rows = 0;
        do
        {
            Field* fields = result->Fetch();
            pool[fields[0].Get<uint32>()].insert(fields[1].Get<uint32>());
            ++rows;
        } while (result->NextRow());

        LOG_INFO("module", "mod-collection: loaded {} collected spell(s) across {} account(s).", rows, pool.size());
        return rows;
    }

    bool IsBot(Player const* player)
    {
        return player && player->GetSession() && player->GetSession()->IsBot();
    }

    Collectable const* Find(uint32 spellId)
    {
        auto const it = collectables.find(spellId);
        return it == collectables.end() ? nullptr : &it->second;
    }

    // Returns true when this is new to the account.
    bool Record(uint32 accountId, uint32 spellId, ObjectGuid::LowType guid)
    {
        Collectable const* what = Find(spellId);
        if (!what || !Collection::MayCollect(what->lines, cfg.Policy))
            return false;

        if (!pool[accountId].insert(spellId).second)
            return false;

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO `account_collection` (`AccountId`, `SpellId`, `Kind`, `FirstGuid`) VALUES ({}, {}, {}, {})",
            accountId, spellId, uint32(what->kind), guid);

        return true;
    }

    // Everything this character already knows goes into the account's pool.
    // Without this, a realm that has been running for months would only start
    // collecting from the next mount bought, because OnPlayerLearnSpell cannot
    // fire retroactively.
    uint32 Harvest(Player* player)
    {
        uint32 added = 0;

        for (auto const& [spellId, state] : player->GetSpellMap())
        {
            if (!state || state->State == PLAYERSPELL_REMOVED)
                continue;

            // Spells this module handed over are temporary and belong to the
            // pool already; collecting them back would be harmless but would
            // credit the wrong character with finding them.
            if (state->State == PLAYERSPELL_TEMPORARY)
                continue;

            if (Record(player->GetSession()->GetAccountId(), spellId, player->GetGUID().GetCounter()))
                ++added;
        }

        return added;
    }

    // Hand over everything in the pool this character may have and lacks.
    uint32 Teach(Player* player)
    {
        auto const it = pool.find(player->GetSession()->GetAccountId());
        if (it == pool.end())
            return 0;

        uint32 const classMask = player->getClassMask();
        uint32 const raceMask  = player->getRaceMask();

        uint32 taught = 0;

        for (uint32 const spellId : it->second)
        {
            if (player->HasSpell(spellId))
                continue;

            Collectable const* what = Find(spellId);
            if (!what || !Collection::MayTeach(what->lines, classMask, raceMask, cfg.Policy))
                continue;

            // temporary: sends the client its learn packet, and _SaveSpells
            // skips it, so character_spell is never touched. SPEC_MASK_ALL is
            // what GetLearnSpellSpecMask would return anyway - it only narrows
            // the mask for talent-based spells, and no mount or companion is
            // one - and that helper is private to Player.
            player->addSpell(spellId, SPEC_MASK_ALL, true, true);
            ++taught;
        }

        return taught;
    }
}

class Collection_WorldScript : public WorldScript
{
public:
    Collection_WorldScript() : WorldScript("Collection_WorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();

        // The DBC stores and the characters database are both absent at startup
        // config load, so the real work happens in OnStartup.
        if (reload)
        {
            BuildCollectables();
            LoadPool();
        }
    }

    void OnStartup() override
    {
        BuildCollectables();
        LoadPool();
    }
};

class Collection_PlayerScript : public PlayerScript
{
public:
    Collection_PlayerScript() : PlayerScript("Collection_PlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LEARN_SPELL
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!cfg.Enable || !player)
            return;

        // 500 playerbots share a handful of accounts, so without this every bot
        // would pool its mounts with its account siblings. They are skipped in
        // both directions: nothing is collected from them and nothing is given
        // to them.
        if (cfg.SkipBots && IsBot(player))
            return;

        uint32 const collected = Harvest(player);
        uint32 const taught    = Teach(player);

        if (collected || taught)
            LOG_DEBUG("module", "mod-collection: {} contributed {} and received {}.",
                player->GetName(), collected, taught);

        if (cfg.Announce && taught)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Your account's collection has added {} mount(s) and companion(s) to your spellbook.", taught);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        // No re-entrancy guard is needed: addSpell does not fire this hook,
        // only learnSpell does, and Harvest skips temporary spells so what this
        // module handed over is never collected back.
        if (!cfg.Enable || !player)
            return;

        if (cfg.SkipBots && IsBot(player))
            return;

        if (!Record(player->GetSession()->GetAccountId(), spellId, player->GetGUID().GetCounter()))
            return;

        if (cfg.Announce)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Added to your account's collection - your other characters will have it at their next login.");
    }
};

class Collection_CommandScript : public CommandScript
{
public:
    Collection_CommandScript() : CommandScript("Collection_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable collectionCommandTable =
        {
            { "list",   HandleCollectionListCommand,   SEC_GAMEMASTER,    Console::Yes },
            { "scan",   HandleCollectionScanCommand,   SEC_ADMINISTRATOR, Console::Yes },
            { "sync",   HandleCollectionSyncCommand,   SEC_GAMEMASTER,    Console::Yes },
            { "reload", HandleCollectionReloadCommand, SEC_ADMINISTRATOR, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "collection", collectionCommandTable }
        };

        return commandTable;
    }

    static bool HandleCollectionReloadCommand(ChatHandler* handler)
    {
        uint32 const known = BuildCollectables();
        uint32 const rows  = LoadPool();

        handler->PSendSysMessage("mod-collection: {} collectable spell(s), {} collected row(s) across {} account(s).",
            known, rows, pool.size());
        handler->PSendSysMessage("Characters already online keep what they were given until they relog.");
        return true;
    }

    static bool HandleCollectionListCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!target)
        {
            handler->SendErrorMessage(LANG_PLAYER_NOT_FOUND);
            return false;
        }

        uint32 const accountId = sCharacterCache->GetCharacterAccountIdByGuid(target->GetGUID());
        auto const it = pool.find(accountId);

        if (it == pool.end() || it->second.empty())
        {
            handler->PSendSysMessage("Account {} ({}) has collected nothing.", accountId, target->GetName());
            return true;
        }

        uint32 mounts = 0;
        uint32 companions = 0;
        for (uint32 const spellId : it->second)
            if (Collectable const* what = Find(spellId))
                what->kind == Collection::KIND_MOUNT ? ++mounts : ++companions;

        handler->PSendSysMessage("Account {} ({}): {} mount(s), {} companion(s).",
            accountId, target->GetName(), mounts, companions);
        return true;
    }

    // Teach a character its account's pool now, without waiting for a relog.
    static bool HandleCollectionSyncCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        Player* player = target ? target->GetConnectedPlayer() : nullptr;
        if (!player)
        {
            handler->SendErrorMessage("That character has to be online to be synced.");
            return false;
        }

        uint32 const collected = Harvest(player);
        uint32 const taught    = Teach(player);

        handler->PSendSysMessage("{} contributed {} spell(s) and received {}.", player->GetName(), collected, taught);
        return true;
    }

    // Harvest from character_spell for characters that are not online, which is
    // what makes an existing realm's collections complete rather than filling
    // in one alt at a time as people log in.
    static bool HandleCollectionScanCommand(ChatHandler* handler)
    {
        if (collectables.empty())
        {
            handler->SendErrorMessage("No collectable spells are loaded.");
            return false;
        }

        // Bot accounts are excluded by name, because an offline bot cannot be
        // told from an offline player in the database - WorldSession::IsBot only
        // answers for a session that exists.
        std::string where;
        if (cfg.SkipBots && !cfg.BotAccountPrefix.empty())
            where = Acore::StringFormat(" AND a.`username` NOT LIKE '{}%'", cfg.BotAccountPrefix);

        QueryResult result = CharacterDatabase.Query(
            "SELECT c.`account`, s.`spell`, MIN(s.`guid`) FROM `character_spell` s "
            "JOIN `characters` c ON c.`guid` = s.`guid` GROUP BY c.`account`, s.`spell`");

        if (!result)
        {
            handler->PSendSysMessage("No character spells to scan.");
            return true;
        }

        // The account name filter cannot be joined across databases, so bot
        // accounts are collected from the login database first.
        std::unordered_set<uint32> skip;
        if (!where.empty())
        {
            if (QueryResult bots = LoginDatabase.Query(
                Acore::StringFormat("SELECT `id` FROM `account` WHERE `username` LIKE '{}%'", cfg.BotAccountPrefix)))
            {
                do
                {
                    skip.insert(bots->Fetch()[0].Get<uint32>());
                } while (bots->NextRow());
            }
        }

        uint32 added = 0;
        uint32 scanned = 0;

        do
        {
            Field* fields = result->Fetch();

            uint32 const accountId = fields[0].Get<uint32>();
            uint32 const spellId   = fields[1].Get<uint32>();
            uint32 const guid      = fields[2].Get<uint32>();

            ++scanned;

            if (skip.count(accountId))
                continue;

            if (Record(accountId, spellId, guid))
                ++added;
        } while (result->NextRow());

        handler->PSendSysMessage("Scanned {} character spell row(s), skipped {} bot account(s), added {} to collections.",
            scanned, uint32(skip.size()), added);
        handler->PSendSysMessage("Characters receive them at their next login, or with \".collection sync\" now.");
        return true;
    }
};

void AddCollectionScripts()
{
    new Collection_WorldScript();
    new Collection_PlayerScript();
    new Collection_CommandScript();
}
