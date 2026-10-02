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

## Riding skill, and the gate that is not where you would look

Riding is skill line **762**, and no mount or companion appears in it - the five
riding spells are in 762 alone - so filtering on the two collection lines cannot
share riding however the policy is set.

That is necessary and **not sufficient**, which took a bug report to notice. A
mount spell carries no requirement of its own: `MinSkillLineRank` is 1 on all
315 mounts, and nothing in `Spell::CheckCast` asks about riding. The gate in
retail is on the **item** that teaches the mount -
`item_template.RequiredSkill`, `RequiredSkillRank`, `RequiredLevel`; the Brown
Horse Bridle is `762 / 75 / 20`. Hand over the learned spell and that gate is
simply never consulted, so a level 14 warlock rides at full speed having never
bought riding.

So `Collection.RespectRequirements` reads the requirement back off the teaching
items and holds a collected spell until the character meets it. Reading it from
the item rather than hard-coding riding pays for itself: of 311 mount spells,
280 are taught by an item, and their requirements are not all riding -

| Required skill | Ranks | Mounts |
| --- | --- | --- |
| 762 Riding | 75 / 150 / 225 / 300 | 60 / 129 / 10 / 57 |
| 197 Tailoring, 202 Engineering | 300-450 | 7 |
| 148/149/150/152/533/554 legacy riding | 1 | 11 |
| none | - | 16 |

- the same rule therefore also keeps a chopper away from somebody who is not an
engineer.

Two wrinkles in that data. The six **legacy per-race riding skills** were merged
into Riding before 3.3.5, so no character has any of them and taking those
requirements literally would block those eleven mounts for ever; they are read
as Riding 75. And where **several items teach the same mount with different
requirements**, the strictest wins: 22717-22724 are each sold as both a
Journeyman and an Apprentice item, and spell 35028 has one source asking for
Riding 150 and another asking for nothing at all, so the permissive reading
would put the gate straight back on the floor. The cost is that nine war steeds
ask one rank more than their cheapest source did.

A collected mount arrives the moment it becomes legitimate: the module re-checks
the pool on level-up and whenever a spell is learned, the latter because riding
ranks and profession skills are themselves spells.

The 31 mounts taught by no item at all - class mounts, quest and achievement
rewards - carry no requirement to recover, and are shared freely.

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

The pooled spells are added with `Player::addSpell(id, SPEC_MASK_ALL, true,
true)` - temporary. That still sends the client its learn packet, so the
spellbook looks normal, but `_SaveSpells` skips temporary spells, so no row is
written. Two consequences worth having:

* `account_collection` is the single record of what has been collected, rather
  than one record plus 1,200 copies spread over the characters.
* Switching the module off gives every character back exactly the spellbook it
  owns, with nothing to clean up.

`addSpell` and not `learnSpell`, and that is not a style choice. For a temporary
learn from in world the two send the learn packet **twice**: `addSpell` has a
branch of its own for temporary spells that calls `SendLearnPacket`, and then
`learnSpell` calls `SendLearnPacket` again on success. The client reads the
second packet as a second spell, so every shared mount and companion showed up
in the spellbook twice. Calling `addSpell` directly sends exactly one. Nothing
else in `learnSpell` is wanted here anyway - its rank-chain and
requires-this-spell cascades mean nothing to a mount, and skipping it keeps this
module out of its own `OnPlayerLearnSpell` hook without needing a guard.

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
