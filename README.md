# mod-collection

Account-wide mount and companion collections. A mount or companion learned by
one character is recorded against the account, and every other character on that
account is given it at their next login.

## What there is to make account-wide

Nothing, which is the first thing to understand. 3.3.5 has no collection
system - the Mount Journal is years away - so a mount is a spell in one
character's spellbook and there is no object to share. This module gives the
account a pool and hands every character what the pool holds.

Identifying them needs no guesswork: `SkillLineAbility` puts every mount in
skill line **777** and every companion in **778**, and
`SpellInfo::IsAbilityOfSkillType` answers against those. In this client's data
that is 315 mounts and 205 companions.

## Riding skill is not shared, structurally

Riding is skill line **762**, and no mount or companion appears in it - the five
riding spells are in 762 alone. So filtering on the two collection lines cannot
share riding however the policy is set; it is not a special case that could be
got wrong later. An alt sees every mount in its spellbook and still has to buy
its own riding before any of them will work, which is also why `MinSkillLineRank`
being 1 on all 315 mounts does not matter: the requirement lives on the spell,
not on the skill line.

## The class mount trap

This is the only genuinely awkward part. A class mount sits in the mounts line
**and** in its class line, and the two disagree:

| Spell | Class line | Mounts line (777) |
| --- | --- | --- |
| 13819 / 23214 / 34767 / 34769 paladin chargers | 594, ClassMask `0x2` | ClassMask `0x2` |
| 5784 Summon Felsteed, 23161 Summon Dreadsteed | 354, ClassMask `0x100` | ClassMask **`0x00`** |
| 48778 Acherus Deathcharger | — | ClassMask `0x20` |

Reading the mounts row alone finds **four** class-locked mounts; reading every
row finds **seven**, and the three it would otherwise miss are the two warlock
steeds and the Deathcharger. A mage with Summon Felsteed looks like a working
shared collection until somebody notices.

The core's own `Player::IsSpellFitByClassAndRace` cannot be used for this: it
passes a spell as soon as *any* row fits, and the unrestricted mounts row always
fits. `CollectionRules.h` instead treats a spell as class-locked if *any* row
carries a class mask, and then requires the character to match one of them.

Class is enforced whatever the configuration says. A class mount is still
*collected* by the account that owns it - the collection reflects what the
account has - it is simply only ever handed back to a character of that class.

## Faction

Most mounts carry no race mask at all, so this decides only a handful: the
Mechano-hog (Horde races), the Mekgineer's Chopper (Alliance races) and the
racial paladin chargers. A race mask on a mount is a faction gate rather than a
mechanical requirement - nothing in the server stops the mount working on the
other side, because the only code that reads that mask is the trainer.

`Collection.CrossFaction` is **on** by default, on the grounds that a collection
belongs to an account rather than to a faction. Turn it off to keep each of those
mounts to the races its DBC row names.

## Nothing is written to `character_spell`

The pooled spells are taught with `Player::learnSpell(id, true)` - temporary.
That still sends the client its learn packet, so the spellbook looks normal, but
`_SaveSpells` skips temporary spells, so no row is written. Two consequences
worth having:

* `account_collection` is the single record of what has been collected, rather
  than one record plus 1,200 copies spread over the characters.
* Switching the module off gives every character back exactly the spellbook it
  owns, with nothing to clean up.

A character's own spells are harvested into the pool at login, which is what
makes an existing realm's collections build up rather than starting empty -
`OnPlayerLearnSpell` cannot fire retroactively for a mount somebody bought last
year.

## The table

`account_collection` in the characters database: `AccountId`, `SpellId`, `Kind`
(0 mount, 1 companion), and `FirstGuid`/`CollectedAt` for curiosity. `Kind` is
stored rather than derived so the table can be read without the DBC to hand.

It ships with the module as `data/sql/db-characters/base/` and the core's
database updater applies it at startup. It is `CREATE TABLE IF NOT EXISTS` and
never `DROP`, because a `MODULE` file is re-applied whenever its hash changes
and this table is the only place the collections exist.

## Commands

    .collection list [player]   how much that account has collected
    .collection sync [player]   hand an online character its pool now
    .collection scan            collect from every offline character too
    .collection reload          re-read the DBC and the table

**Run `.collection scan` once after installing.** It harvests `character_spell`
for every character on the realm, which is what makes existing collections
complete immediately instead of filling in one alt at a time as people log in.
It excludes bot accounts by name (`Collection.BotAccountPrefix`), because an
offline bot cannot be told from an offline player in the database -
`WorldSession::IsBot` only answers for a session that exists.

## Configuration (`mod_collection.conf`)

`Collection.Enable`, `.Mounts`, `.Companions`, `.CrossFaction`, `.SkipBots`,
`.BotAccountPrefix`, `.Announce`. Read in `OnAfterConfigLoad`, so `reload config`
applies them live - though characters already online keep what they were given
until they relog.

`Collection.SkipBots` is worth keeping on with mod-playerbots: its 500 bots share
a handful of accounts, so otherwise every bot would pool its mounts with its
account siblings. Bots are skipped in both directions.

## Tests

`src/CollectionRules.h` holds the parts that are just rules and includes nothing
but the standard library, so `tests/` builds and runs without AzerothCore. The
fixtures are real `SkillLineAbility` rows rather than invented shapes, so the
class mount trap above is pinned down as data:

    cmake -S tests -B build-tests -DCMAKE_CXX_COMPILER=g++-14
    cmake --build build-tests && ctest --test-dir build-tests

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its modules
use. See [LICENSE](LICENSE).
