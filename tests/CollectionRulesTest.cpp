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
