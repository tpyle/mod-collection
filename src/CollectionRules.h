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

    // The pre-TBC per-race riding skills. They were merged into SKILL_RIDING
    // long before 3.3.5, so no character has any of them - but eleven mounts
    // are still taught by items that ask for one. Taking those at face value
    // would block those mounts for ever, so they are read as Riding instead.
    inline bool IsLegacyRidingSkill(std::uint32_t skill)
    {
        switch (skill)
        {
            case 148:   // SKILL_RIDING_HORSE
            case 149:   // SKILL_RIDING_WOLF
            case 150:   // SKILL_RIDING_TIGER
            case 152:   // SKILL_RIDING_RAM
            case 533:   // SKILL_RIDING_RAPTOR
            case 554:   // SKILL_RIDING_UNDEAD_HORSE
                return true;
            default:
                return false;
        }
    }

    constexpr std::uint32_t RIDING_APPRENTICE = 75;

    // What a character has to be and know before a collected spell is handed
    // over. This does not come from the spell: a mount spell carries no
    // requirement at all, which is the whole reason this exists. The gate in
    // retail 3.3.5 is on the ITEM that teaches the mount -
    // item_template.RequiredSkill / RequiredSkillRank / RequiredLevel - so
    // sharing a learned spell walks straight past it, and a level 14 warlock
    // ends up riding without ever having bought riding.
    //
    // Reading it from the item generalises past riding for free: seven mounts
    // are gated on Tailoring or Engineering rather than on Riding, and the same
    // rule keeps a chopper away from somebody who is not an engineer.
    struct Requirement
    {
        std::uint32_t skillId   = 0;
        std::uint32_t skillRank = 0;
        std::uint32_t level     = 0;

        bool Any() const { return skillId || level; }
    };

    inline Requirement Normalise(Requirement req)
    {
        if (IsLegacyRidingSkill(req.skillId))
        {
            req.skillId   = SKILL_RIDING;
            req.skillRank = RIDING_APPRENTICE;
        }

        return req;
    }

    // Several items can teach the same mount with different requirements -
    // 22717 through 22724 are each sold both as a Journeyman and as an
    // Apprentice item, and 35028 has one source asking for Riding 150 and
    // another asking for nothing whatsoever. The strictest reading wins,
    // because the permissive one would let that second source erase the gate
    // entirely, which is the bug this is here to close. The cost is that nine
    // war steeds ask for one rank more than their cheapest source did.
    inline Requirement Stricter(Requirement const& a, Requirement const& b)
    {
        Requirement out;

        out.level = a.level > b.level ? a.level : b.level;

        // A skill requirement beats no skill requirement, and between two of
        // the same skill the higher rank wins. Two different skills cannot be
        // combined, so the one already there is kept.
        if (!a.skillId)
            out.skillId = b.skillId, out.skillRank = b.skillRank;
        else if (!b.skillId || a.skillId != b.skillId)
            out.skillId = a.skillId, out.skillRank = a.skillRank;
        else
        {
            out.skillId   = a.skillId;
            out.skillRank = a.skillRank > b.skillRank ? a.skillRank : b.skillRank;
        }

        return out;
    }

    // skillRank is the character's rank in req.skillId, which the caller looks
    // up; this header knows nothing about players.
    inline bool Meets(Requirement const& req, std::uint32_t level, std::uint32_t skillRank)
    {
        if (req.level && level < req.level)
            return false;

        if (req.skillId && skillRank < req.skillRank)
            return false;

        return true;
    }

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
