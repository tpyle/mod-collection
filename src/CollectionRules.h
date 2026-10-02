/*
 * mod-collection - the parts that are just rules, with no server behind them.
 *
 * Deliberately includes nothing but the standard library, so tests/ can build
 * and run this without AzerothCore present. A spell's SkillLineAbility rows
 * are passed in reduced to the three fields that matter, which is also what
 * lets the tests state the awkward cases as data.
 */

#ifndef MOD_COLLECTION_RULES_H
#define MOD_COLLECTION_RULES_H

#include <cstdint>
#include <optional>
#include <vector>

namespace Collection
{
    // SkillLine ids from the core's SkillType enum. Riding is listed only to
    // say that it is not here by accident: no mount or companion spell appears
    // in it (checked against SkillLineAbility.dbc - the five riding spells are
    // in 762 alone), so filtering on the two collection lines cannot share
    // riding skill however the policy is set.
    constexpr std::uint32_t SKILL_MOUNTS     = 777;
    constexpr std::uint32_t SKILL_COMPANIONS = 778;
    constexpr std::uint32_t SKILL_RIDING     = 762;

    enum Kind : std::uint8_t
    {
        KIND_MOUNT     = 0,
        KIND_COMPANION = 1
    };

    // One SkillLineAbility row, cut down to what the rules read.
    struct SkillLine
    {
        std::uint32_t skillLine = 0;
        std::uint32_t raceMask  = 0;
        std::uint32_t classMask = 0;
    };

    using SkillLines = std::vector<SkillLine>;

    struct Policy
    {
        bool mounts        = true;
        bool companions    = true;
        // Share a mount whose SkillLineAbility row restricts it to the other
        // faction's races. Most mounts carry no race mask at all, so this only
        // decides the handful that do - the Mechano-hog, the Mekgineer's
        // Chopper and the racial paladin chargers.
        bool crossFaction  = true;
    };

    // Which collection a spell belongs to, if either. ALL of a spell's rows are
    // searched, not just the first.
    inline std::optional<Kind> KindOf(SkillLines const& lines)
    {
        for (SkillLine const& line : lines)
            if (line.skillLine == SKILL_MOUNTS)
                return KIND_MOUNT;

        for (SkillLine const& line : lines)
            if (line.skillLine == SKILL_COMPANIONS)
                return KIND_COMPANION;

        return std::nullopt;
    }

    // Whether any row restricts the spell to particular classes.
    //
    // This has to read every row, and that is the whole subtlety of the module.
    // A class mount appears in the mounts line AND in its class line, and the
    // two disagree: the paladin chargers carry ClassMask 0x2 in both, but
    // Summon Felsteed and Summon Dreadsteed carry ClassMask 0x100 only in the
    // Demonology line (354) and 0x00 in the mounts line (777). Reading the
    // mounts row alone finds four class-locked mounts; reading all rows finds
    // seven, and the three it would otherwise miss are the two warlock steeds
    // and Acherus Deathcharger. The core's own Player::IsSpellFitByClassAndRace
    // is no help here, because it passes a spell as soon as ANY row fits - and
    // the unrestricted mounts row always fits.
    inline bool IsClassLocked(SkillLines const& lines)
    {
        for (SkillLine const& line : lines)
            if (line.classMask)
                return true;

        return false;
    }

    inline bool IsRaceLocked(SkillLines const& lines)
    {
        for (SkillLine const& line : lines)
            if (line.raceMask)
                return true;

        return false;
    }

    // An unrestricted spell fits everybody. A restricted one fits a character
    // who matches at least one of the restricting rows.
    inline bool FitsClass(SkillLines const& lines, std::uint32_t classMask)
    {
        if (!IsClassLocked(lines))
            return true;

        for (SkillLine const& line : lines)
            if (line.classMask && (line.classMask & classMask))
                return true;

        return false;
    }

    inline bool FitsRace(SkillLines const& lines, std::uint32_t raceMask)
    {
        if (!IsRaceLocked(lines))
            return true;

        for (SkillLine const& line : lines)
            if (line.raceMask && (line.raceMask & raceMask))
                return true;

        return false;
    }

    // Whether this spell may be handed to a character of this class and race.
    //
    // Class is always enforced: a mage with Summon Felsteed is not a shared
    // collection, it is a bug that happens to be castable. Race is enforced
    // only when crossFaction is off, because a race mask on a mount is a
    // faction gate rather than a mechanical requirement - nothing in the server
    // stops the mount working, and Player::IsSpellFitByClassAndRace is only
    // consulted by the trainer code.
    inline bool MayTeach(SkillLines const& lines, std::uint32_t classMask, std::uint32_t raceMask,
                         Policy const& policy)
    {
        std::optional<Kind> const kind = KindOf(lines);
        if (!kind)
            return false;

        if (*kind == KIND_MOUNT && !policy.mounts)
            return false;

        if (*kind == KIND_COMPANION && !policy.companions)
            return false;

        if (!FitsClass(lines, classMask))
            return false;

        if (!policy.crossFaction && !FitsRace(lines, raceMask))
            return false;

        return true;
    }

    // Whether this spell is worth recording when a character learns it. Wider
    // than MayTeach on purpose: a paladin's charger belongs in the account's
    // collection even though only its paladins will ever be given it back.
    inline bool MayCollect(SkillLines const& lines, Policy const& policy)
    {
        std::optional<Kind> const kind = KindOf(lines);
        if (!kind)
            return false;

        return *kind == KIND_MOUNT ? policy.mounts : policy.companions;
    }
}

#endif // MOD_COLLECTION_RULES_H
