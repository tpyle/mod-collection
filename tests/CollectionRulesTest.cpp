/*
 * Tests for mod-collection's rules (src/CollectionRules.h).
 *
 * The cases worth pinning down are the ones the DBC actually contains, so the
 * fixtures below are real SkillLineAbility rows read out of
 * SkillLineAbility.dbc rather than invented shapes. The failure they guard
 * against is silent: a mage that can summon a felsteed looks like a working
 * shared collection until somebody notices.
 */

#include "CollectionRules.h"

#include <gtest/gtest.h>

using namespace Collection;

namespace
{
    // Class masks from the core's ClassMask enum.
    constexpr std::uint32_t MAGE     = 1u << 7;   // 0x080
    constexpr std::uint32_t WARLOCK  = 1u << 8;   // 0x100
    constexpr std::uint32_t PALADIN  = 1u << 1;   // 0x002
    constexpr std::uint32_t DK       = 1u << 5;   // 0x020

    // Race masks.
    constexpr std::uint32_t HUMAN    = 1u << 0;   // 0x001
    constexpr std::uint32_t ORC      = 1u << 1;   // 0x002
    constexpr std::uint32_t BLOODELF = 1u << 9;   // 0x200

    constexpr std::uint32_t ALLIANCE_RACES = 0xc4d;  // as 60424 carries it
    constexpr std::uint32_t HORDE_RACES    = 0x3b2;  // as 55531 carries it

    // 458 Brown Horse - an ordinary mount, one unrestricted row.
    SkillLines PlainMount() { return { { SKILL_MOUNTS, 0, 0 } }; }

    // 34092 Kodo - an ordinary companion.
    SkillLines PlainCompanion() { return { { SKILL_COMPANIONS, 0, 0 } }; }

    // 5784 Summon Felsteed. THE awkward one: the class mask lives in the
    // Demonology line and the mounts line is wide open.
    SkillLines Felsteed() { return { { 354, 0, WARLOCK }, { SKILL_MOUNTS, 0, 0 } }; }

    // 23214 Summon Charger - the paladin line and the mounts line agree.
    SkillLines Charger()
    {
        return { { 594, 0x405, PALADIN }, { SKILL_MOUNTS, 0x405, PALADIN } };
    }

    // 48778 Acherus Deathcharger.
    SkillLines Deathcharger() { return { { SKILL_MOUNTS, 0, DK } }; }

    // 60424 Mekgineer's Chopper - race-gated to the Alliance, no class mask.
    SkillLines AllianceChopper() { return { { SKILL_MOUNTS, ALLIANCE_RACES, 0 } }; }

    // 55531 Mechano-hog - the Horde counterpart.
    SkillLines HordeChopper() { return { { SKILL_MOUNTS, HORDE_RACES, 0 } }; }

    // A spell in neither collection line - Journeyman Riding (33391, skill 762).
    SkillLines Riding() { return { { SKILL_RIDING, 0, 0x5df } }; }

    Policy Default() { return Policy{}; }
}

TEST(CollectionKind, RecognisesBothLinesAndNothingElse)
{
    EXPECT_EQ(KindOf(PlainMount()), KIND_MOUNT);
    EXPECT_EQ(KindOf(PlainCompanion()), KIND_COMPANION);
    EXPECT_EQ(KindOf(Felsteed()), KIND_MOUNT);
    EXPECT_EQ(KindOf(Charger()), KIND_MOUNT);

    EXPECT_FALSE(KindOf(Riding()).has_value());
    EXPECT_FALSE(KindOf({}).has_value());
    EXPECT_FALSE(KindOf({ { 171, 0, 0 } }).has_value());   // alchemy
}

TEST(CollectionKind, RidingIsNeverACollectable)
{
    // The whole "riding stays separate" promise rests on this: riding lives in
    // skill 762 and no mount or companion appears there, so the filter cannot
    // share it however the policy is set.
    EXPECT_FALSE(MayCollect(Riding(), Default()));
    EXPECT_FALSE(MayTeach(Riding(), MAGE, HUMAN, Default()));

    Policy everything;
    everything.crossFaction = true;
    everything.mounts = true;
    everything.companions = true;
    EXPECT_FALSE(MayTeach(Riding(), PALADIN, BLOODELF, everything));
}

TEST(CollectionClassLock, ReadsEveryRowAndNotJustTheMountsRow)
{
    // Reading only the mounts row would call Felsteed unrestricted, because
    // that row carries ClassMask 0x00. All seven real class-locked mounts have
    // to come back locked.
    EXPECT_TRUE(IsClassLocked(Felsteed()));
    EXPECT_TRUE(IsClassLocked(Charger()));
    EXPECT_TRUE(IsClassLocked(Deathcharger()));

    EXPECT_FALSE(IsClassLocked(PlainMount()));
    EXPECT_FALSE(IsClassLocked(PlainCompanion()));
    EXPECT_FALSE(IsClassLocked(AllianceChopper()));   // race gated, not class gated
}

TEST(CollectionClassLock, AMageNeverGetsAFelsteed)
{
    EXPECT_FALSE(MayTeach(Felsteed(), MAGE, HUMAN, Default()));
    EXPECT_TRUE(MayTeach(Felsteed(), WARLOCK, HUMAN, Default()));
}

TEST(CollectionClassLock, ClassMountsGoOnlyToTheirClass)
{
    EXPECT_TRUE(MayTeach(Charger(), PALADIN, HUMAN, Default()));
    EXPECT_FALSE(MayTeach(Charger(), MAGE, HUMAN, Default()));
    EXPECT_FALSE(MayTeach(Charger(), WARLOCK, HUMAN, Default()));

    EXPECT_TRUE(MayTeach(Deathcharger(), DK, HUMAN, Default()));
    EXPECT_FALSE(MayTeach(Deathcharger(), PALADIN, HUMAN, Default()));
}

TEST(CollectionClassLock, ClassIsEnforcedEvenWithEveryPolicyOpen)
{
    // crossFaction loosens race and must not loosen class with it.
    Policy open;
    open.crossFaction = true;

    EXPECT_FALSE(MayTeach(Felsteed(), MAGE, HUMAN, open));
    EXPECT_FALSE(MayTeach(Charger(), MAGE, ORC, open));
}

TEST(CollectionClassLock, AClassMountIsStillCollectedByItsOwner)
{
    // The account records it even though only its paladins will be given it
    // back, so the collection reflects what the account owns.
    EXPECT_TRUE(MayCollect(Charger(), Default()));
    EXPECT_TRUE(MayCollect(Felsteed(), Default()));
}

TEST(CollectionFaction, CrossFactionSharesTheRaceGatedMounts)
{
    Policy const crossing = Default();   // crossFaction defaults to true
    ASSERT_TRUE(crossing.crossFaction);

    // A human receives the Horde chopper and an orc the Alliance one. Nothing
    // in the server stops either working: the race mask on a mount is a faction
    // gate, and only the trainer code consults it.
    EXPECT_TRUE(MayTeach(HordeChopper(), MAGE, HUMAN, crossing));
    EXPECT_TRUE(MayTeach(AllianceChopper(), MAGE, ORC, crossing));
}

TEST(CollectionFaction, WithoutCrossFactionTheRaceGateHolds)
{
    Policy strict;
    strict.crossFaction = false;

    EXPECT_FALSE(MayTeach(HordeChopper(), MAGE, HUMAN, strict));
    EXPECT_TRUE(MayTeach(HordeChopper(), MAGE, ORC, strict));

    EXPECT_TRUE(MayTeach(AllianceChopper(), MAGE, HUMAN, strict));
    EXPECT_FALSE(MayTeach(AllianceChopper(), MAGE, ORC, strict));

    // An unrestricted mount is unaffected either way.
    EXPECT_TRUE(MayTeach(PlainMount(), MAGE, HUMAN, strict));
}

TEST(CollectionFaction, StrictModeStillLetsAClassMountThroughToItsOwnRace)
{
    // The charger carries both masks; a blood elf paladin matches 0x405? No -
    // 0x405 is human, dwarf and draenei, so this is the pair of racial charger
    // spells behaving as the DBC says rather than as lore might suggest.
    Policy strict;
    strict.crossFaction = false;

    EXPECT_TRUE(MayTeach(Charger(), PALADIN, HUMAN, strict));
    EXPECT_FALSE(MayTeach(Charger(), PALADIN, BLOODELF, strict));

    // Crossing faction, the same blood elf paladin may have it.
    EXPECT_TRUE(MayTeach(Charger(), PALADIN, BLOODELF, Default()));
}

TEST(CollectionPolicy, EitherKindCanBeSwitchedOffIndependently)
{
    Policy mountsOnly;
    mountsOnly.companions = false;

    EXPECT_TRUE(MayTeach(PlainMount(), MAGE, HUMAN, mountsOnly));
    EXPECT_FALSE(MayTeach(PlainCompanion(), MAGE, HUMAN, mountsOnly));
    EXPECT_TRUE(MayCollect(PlainMount(), mountsOnly));
    EXPECT_FALSE(MayCollect(PlainCompanion(), mountsOnly));

    Policy companionsOnly;
    companionsOnly.mounts = false;

    EXPECT_FALSE(MayTeach(PlainMount(), MAGE, HUMAN, companionsOnly));
    EXPECT_TRUE(MayTeach(PlainCompanion(), MAGE, HUMAN, companionsOnly));
}

TEST(CollectionPolicy, CompanionsCarryNoRestrictionsAtAll)
{
    // All 205 of them: not one row in the DBC has a race or class mask, so a
    // companion fits every character there is.
    EXPECT_TRUE(MayTeach(PlainCompanion(), MAGE, HUMAN, Default()));
    EXPECT_TRUE(MayTeach(PlainCompanion(), DK, BLOODELF, Default()));

    Policy strict;
    strict.crossFaction = false;
    EXPECT_TRUE(MayTeach(PlainCompanion(), PALADIN, ORC, strict));
}

TEST(CollectionFits, UnrestrictedFitsEverybody)
{
    EXPECT_TRUE(FitsClass(PlainMount(), MAGE));
    EXPECT_TRUE(FitsClass(PlainMount(), 0));
    EXPECT_TRUE(FitsRace(PlainMount(), HUMAN));
    EXPECT_TRUE(FitsRace(PlainMount(), 0));
}

// ---------------------------------------------------------------------------
// Requirements.
//
// These exist because a mount spell carries no requirement at all: the gate is
// on the item that teaches it. Sharing a learned spell bypassed it completely
// and a level 14 character rode without ever having bought riding.
// ---------------------------------------------------------------------------

TEST(CollectionRequirement, NoRequirementIsNoGate)
{
    Requirement none;
    EXPECT_FALSE(none.Any());
    EXPECT_TRUE(Meets(none, 1, 0));
}

TEST(CollectionRequirement, RidingRankAndLevelAreBothEnforced)
{
    // 5656 Brown Horse Bridle: RequiredSkill 762, rank 75, level 20.
    Requirement const bridle{ SKILL_RIDING, 75, 20 };

    EXPECT_TRUE(bridle.Any());

    EXPECT_FALSE(Meets(bridle, 14, 0));    // the reported bug: level 14, no riding
    EXPECT_FALSE(Meets(bridle, 20, 0));    // right level, no riding
    EXPECT_FALSE(Meets(bridle, 14, 75));   // riding, too low a level
    EXPECT_TRUE(Meets(bridle, 20, 75));    // both
    EXPECT_TRUE(Meets(bridle, 80, 300));   // comfortably both
}

TEST(CollectionRequirement, AProfessionGateWorksTheSameWay)
{
    // The engineering choppers ask for Engineering rather than Riding, and the
    // same rule keeps them away from anybody who is not an engineer.
    Requirement const chopper{ 202 /*engineering*/, 375, 70 };

    EXPECT_FALSE(Meets(chopper, 80, 0));
    EXPECT_FALSE(Meets(chopper, 80, 300));
    EXPECT_TRUE(Meets(chopper, 80, 375));
    EXPECT_FALSE(Meets(chopper, 69, 450));
}

TEST(CollectionRequirement, LegacyRacialRidingSkillsAreReadAsRiding)
{
    // 148, 149, 150, 152, 533 and 554 were merged into Riding before 3.3.5, so
    // no character has any of them. Taken literally they would block the eleven
    // mounts whose items still ask for one, for ever.
    for (std::uint32_t skill : { 148u, 149u, 150u, 152u, 533u, 554u })
    {
        EXPECT_TRUE(IsLegacyRidingSkill(skill)) << skill;

        Requirement const legacy = Normalise({ skill, 1, 40 });
        EXPECT_EQ(legacy.skillId, SKILL_RIDING);
        EXPECT_EQ(legacy.skillRank, RIDING_APPRENTICE);
        EXPECT_EQ(legacy.level, 40u);

        // Which then behaves as an ordinary riding gate.
        EXPECT_FALSE(Meets(legacy, 40, 0));
        EXPECT_TRUE(Meets(legacy, 40, 75));
    }

    EXPECT_FALSE(IsLegacyRidingSkill(SKILL_RIDING));
    EXPECT_FALSE(IsLegacyRidingSkill(202));
    EXPECT_FALSE(IsLegacyRidingSkill(0));
}

TEST(CollectionRequirement, NormaliseLeavesEverythingElseAlone)
{
    Requirement const riding = Normalise({ SKILL_RIDING, 150, 40 });
    EXPECT_EQ(riding.skillId, SKILL_RIDING);
    EXPECT_EQ(riding.skillRank, 150u);

    Requirement const tailoring = Normalise({ 197, 300, 60 });
    EXPECT_EQ(tailoring.skillId, 197u);
    EXPECT_EQ(tailoring.skillRank, 300u);
}

TEST(CollectionRequirement, TheStrictestSourceWins)
{
    // 22717 through 22724 are each sold as both a Journeyman and an Apprentice
    // item. Journeyman has to win, or the cheaper source lowers the gate.
    Requirement const journeyman{ SKILL_RIDING, 150, 40 };
    Requirement const apprentice{ SKILL_RIDING, 75, 40 };

    Requirement const both = Stricter(journeyman, apprentice);
    EXPECT_EQ(both.skillRank, 150u);
    EXPECT_EQ(Stricter(apprentice, journeyman).skillRank, 150u);
}

TEST(CollectionRequirement, ASourceWithNoRequirementCannotEraseTheGate)
{
    // 35028 is the case that settles the policy: one item asks for Riding 150
    // and another asks for nothing. Reading the permissive one would hand the
    // mount to anybody, which is the bug all over again.
    Requirement const gated{ SKILL_RIDING, 150, 40 };
    Requirement const free{ 0, 0, 40 };

    Requirement const both = Stricter(gated, free);
    EXPECT_EQ(both.skillId, SKILL_RIDING);
    EXPECT_EQ(both.skillRank, 150u);
    EXPECT_EQ(both.level, 40u);
    EXPECT_FALSE(Meets(both, 80, 0));

    EXPECT_EQ(Stricter(free, gated).skillRank, 150u);
}

TEST(CollectionRequirement, TheHigherLevelWins)
{
    // 54753 and 65917 each have a level 40 and a level 60 source.
    Requirement const low{ SKILL_RIDING, 150, 40 };
    Requirement const high{ SKILL_RIDING, 150, 60 };

    EXPECT_EQ(Stricter(low, high).level, 60u);
    EXPECT_EQ(Stricter(high, low).level, 60u);
}

TEST(CollectionRequirement, TwoDifferentSkillsKeepTheFirst)
{
    // Nothing sensible can be made of "needs Riding 150 AND Engineering 375"
    // in one field, so the requirement already recorded is kept rather than
    // being silently swapped for an unrelated skill.
    Requirement const riding{ SKILL_RIDING, 150, 40 };
    Requirement const engineering{ 202, 375, 70 };

    Requirement const both = Stricter(riding, engineering);
    EXPECT_EQ(both.skillId, SKILL_RIDING);
    EXPECT_EQ(both.skillRank, 150u);
    EXPECT_EQ(both.level, 70u);          // the stricter level still applies
}

TEST(CollectionRequirement, StricterOfNothingIsNothing)
{
    EXPECT_FALSE(Stricter({}, {}).Any());
}
