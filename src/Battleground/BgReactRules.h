/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef Playerbots_BgReactRules_h__
#define Playerbots_BgReactRules_h__

// The rules of the reactive battleground behaviours (docs/design/BG_PLAN.md §4, phase 4) that need no game objects: threats and
// retakes, falling back, the score outlook and the team's stance, focus classes, the /bg call parser, archetypes and team skill,
// deaths at one spot and the carrier's kite spot. Header only, tested by tests/bg_rules_test.cpp without TrinityCore

#include "Battleground/BgRules.h"
#include "Travel/LegRules.h"
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>

namespace Playerbots::BgRules
{
// ---- decisions that hold for one event ----

/// A yes/no that a bot makes once per event: the same team seed, bot and event always give the same answer, so a decision
/// made with a chance (team skill) does not flip from one planner tick to the next
inline bool EventRoll(uint64 seed, uint64 guidLow, uint64 eventId, uint32 pct)
{
    if (pct >= 100)
        return true;
    if (pct == 0)
        return false;
    return Mix(seed ^ Mix(guidLow * 0x9E3779B97F4A7C15ull ^ (eventId + 0x632BE59BD9B4E019ull))) % 100 < pct;
}

// ---- threats at a node (#1), retakes (#2), falling back (#8) ----

/// How many more allies a threatened node needs to outnumber its attackers by one, at most cap. 0 when it is not outnumbered
inline uint32 ThreatNeed(uint32 enemies, uint32 allies, uint32 cap)
{
    if (enemies == 0 || allies > enemies)
        return 0;
    return std::min(cap, enemies + 1 - allies);
}

/// A lost fight: even with every helper we can send the node stays outnumbered ratio to one (2 into 8 is not sent)
inline bool Hopeless(uint32 enemies, uint32 allies, uint32 helpers, uint32 ratio, uint32 minEnemies)
{
    return enemies >= minEnemies && enemies >= ratio * (allies + helpers);
}

enum class Urgency : uint8
{
    None,       // no assault of theirs runs on this node
    Normal,     // more than half of the capture time is left
    Half,       // half of it or less: the attackers help too
    Last        // the last seconds: everyone who can get there in time goes
};

inline char const* UrgencyName(Urgency urgency)
{
    switch (urgency)
    {
        case Urgency::Normal: return "normal";
        case Urgency::Half: return "half time";
        case Urgency::Last: return "last seconds";
        default: return "none";
    }
}

/// What is left of an assault that began at sinceMs and takes totalMs
inline uint32 TimeLeft(uint32 sinceMs, uint32 nowMs, uint32 totalMs)
{
    uint32 gone = nowMs - sinceMs;
    return gone >= totalMs ? 0 : totalMs - gone;
}

inline Urgency RetakeUrgency(uint32 leftMs, uint32 totalMs, uint32 lastMs)
{
    if (!totalMs)
        return Urgency::None;
    if (leftMs <= lastMs)
        return Urgency::Last;
    if (uint64(leftMs) * 2 <= totalMs)
        return Urgency::Half;
    return Urgency::Normal;
}

/// A bot this far away at this speed (yards per second) gets there before the assault runs out
inline bool InTime(float distance, float speed, uint32 leftMs)
{
    return speed > 0.0f && distance / speed * 1000.0f < float(leftMs);
}

/// Outnumbered ratio to one around a bot (allies count the bot itself), with at least minEnemies enemies: it falls back
inline bool Outnumbered(uint32 enemies, uint32 allies, uint32 ratio, uint32 minEnemies)
{
    return enemies >= minEnemies && enemies >= ratio * std::max<uint32>(1, allies);
}

/// A push on a boss that gathers, charges, fights or regroups: its bots are never helpers, a mine group or a ninja (a helper
/// taken from it would still count on the push and be pulled back by the leash)
inline bool PushActive(RallyState state)
{
    return state == RallyState::Gathering || state == RallyState::Charging || state == RallyState::Fighting || state == RallyState::Regroup;
}

/// A push past its gather: nothing but the carrier overrides it, not even a fall back
inline bool PushCharged(RallyState state)
{
    return state == RallyState::Charging || state == RallyState::Fighting || state == RallyState::Regroup;
}

/// Which plan a bot follows when several want it, highest first. A charged push keeps its bots (they never become helpers, a
/// mine group or a ninja), and a carrier always plays its flag
enum class Overlay : uint8
{
    None,       // its job from the split
    Brawl,      // the losing side farms kills (#15)
    Mine,       // the mine group (#13)
    Ninja,      // the ninja capper (#17)
    Respond,    // a threat, a retake or a call sends it to one of our nodes (#1, #2, #10)
    FallBack,   // outnumbered: back to friends (#8)
    Push,       // its push on a boss charges, fights or regroups
    Carrier     // it carries a flag
};

inline char const* OverlayName(Overlay overlay)
{
    switch (overlay)
    {
        case Overlay::Brawl: return "brawl";
        case Overlay::Mine: return "mine";
        case Overlay::Ninja: return "ninja";
        case Overlay::Respond: return "respond";
        case Overlay::FallBack: return "fall back";
        case Overlay::Push: return "push";
        case Overlay::Carrier: return "carrier";
        default: return "-";
    }
}

// ---- the score outlook (#4) and the end of a match (#15) ----

constexpr uint32 Never = std::numeric_limits<uint32>::max();

/// Seconds until score reaches win at pointsPerTick every tickMs; Never when it does not grow
inline uint32 SecondsToWin(uint32 score, uint32 win, uint32 pointsPerTick, uint32 tickMs)
{
    if (score >= win)
        return 0;
    if (!pointsPerTick || !tickMs)
        return Never;
    uint64 ticks = (uint64(win - score) + pointsPerTick - 1) / pointsPerTick;
    return uint32(std::min<uint64>(Never - 1, ticks * tickMs / 1000));
}

struct Outlook
{
    int8 Ahead = 0;                 // 1 we win first, -1 they do, 0 too close to call
    bool Certain = false;           // the result is certain (Ahead tells whose)
    bool Late = false;              // the side behind is running out of time
    uint32 UsSeconds = Never;       // race maps: seconds to win at the current rates
    uint32 ThemSeconds = Never;
};

struct OutlookParams
{
    uint32 MarginPct = 10;          // ahead: the other side needs this much longer (race), this share of the start more (Valley)
    uint32 CertainSeconds = 90;     // race maps: certain once the leader needs less than this and the other twice as long
    uint32 LateSeconds = 180;       // race maps: late once the leader needs less than this; flag maps: less than this left
};

/// Race maps (Arathi Basin, Battle for Gilneas, Eye of the Storm): who reaches the win score first at today's rates
inline Outlook ProjectRace(uint32 secondsUs, uint32 secondsThem, OutlookParams const& params)
{
    Outlook outlook;
    outlook.UsSeconds = secondsUs;
    outlook.ThemSeconds = secondsThem;
    auto clearly = [&](uint32 fast, uint32 slow)
    {
        return fast < slow && (slow == Never || uint64(slow - fast) * 100 >= uint64(slow) * params.MarginPct);
    };
    if (clearly(secondsUs, secondsThem))
        outlook.Ahead = 1;
    else if (clearly(secondsThem, secondsUs))
        outlook.Ahead = -1;

    uint32 lead = outlook.Ahead > 0 ? secondsUs : secondsThem;
    uint32 trail = outlook.Ahead > 0 ? secondsThem : secondsUs;
    if (outlook.Ahead)
    {
        outlook.Certain = lead <= params.CertainSeconds && (trail == Never || uint64(trail) >= uint64(lead) * 2);
        outlook.Late = lead <= params.LateSeconds;
    }
    return outlook;
}

/// Flag maps (Warsong Gulch, Twin Peaks): three captures win, the clock ends the match at limitMs of elapsed time
inline Outlook ProjectCaptures(uint32 capsUs, uint32 capsThem, uint32 elapsedMs, uint32 limitMs, OutlookParams const& params)
{
    Outlook outlook;
    outlook.Ahead = capsUs > capsThem ? 1 : capsThem > capsUs ? -1 : 0;
    uint32 leftMs = elapsedMs >= limitMs ? 0 : limitMs - elapsedMs;
    uint32 lead = capsUs > capsThem ? capsUs - capsThem : capsThem - capsUs;
    outlook.Late = outlook.Ahead && leftMs <= params.LateSeconds * 1000;
    // two up with three minutes left, or one up with the last minute running
    outlook.Certain = outlook.Ahead && ((lead >= 2 && leftMs <= params.CertainSeconds * 2000) || leftMs <= 60000);
    return outlook;
}

/// Alterac Valley: the reinforcements of both sides against the starting number
inline Outlook ProjectReinforcements(uint32 us, uint32 them, uint32 start, OutlookParams const& params)
{
    Outlook outlook;
    if (!start)
        return outlook;
    uint64 margin = uint64(start) * params.MarginPct / 100;
    if (us > them && us - them >= margin)
        outlook.Ahead = 1;
    else if (them > us && them - us >= margin)
        outlook.Ahead = -1;
    uint32 low = outlook.Ahead > 0 ? them : us;
    uint32 high = outlook.Ahead > 0 ? us : them;
    if (outlook.Ahead)
    {
        outlook.Certain = uint64(low) * 10 < start && high > low * 3;
        outlook.Late = uint64(low) * 100 <= uint64(start) * 30;
    }
    return outlook;
}

enum class Stance : uint8
{
    Normal,     // the rolled defence share
    Turtle,     // ahead: more defenders, a majority of the nodes is held and not overextended
    AllIn,      // behind and running out of time: nearly everyone attacks
    Brawl,      // a certain loss: the attackers fight where the enemies are instead of the objectives (#15)
    Finish      // a certain win: Alterac Valley pushes the general at once (#15)
};

inline char const* StanceName(Stance stance)
{
    switch (stance)
    {
        case Stance::Turtle: return "turtle";
        case Stance::AllIn: return "all in";
        case Stance::Brawl: return "brawl";
        case Stance::Finish: return "finish";
        default: return "normal";
    }
}

/// The team's stance from the outlook. scoreClock: Playerbots.Bg.ScoreClock (#4), endgame: Playerbots.Bg.Endgame (#15)
inline Stance PickStance(Outlook const& outlook, bool scoreClock, bool endgame)
{
    if (outlook.Certain && endgame)
        return outlook.Ahead > 0 ? Stance::Finish : Stance::Brawl;
    if (!scoreClock)
        return Stance::Normal;
    if (outlook.Ahead > 0)
        return Stance::Turtle;
    if (outlook.Ahead < 0 && (outlook.Late || outlook.Certain))
        return Stance::AllIn;
    return Stance::Normal;
}

/// The defence share of a stance: Turtle adds turtlePct (at most 70), AllIn and Brawl leave allInPct, the rest keep rolled
inline uint32 StanceDefendPct(Stance stance, uint32 rolled, uint32 turtlePct, uint32 allInPct)
{
    switch (stance)
    {
        case Stance::Turtle: return std::min<uint32>(70, rolled + turtlePct);
        case Stance::AllIn:
        case Stance::Brawl: return std::min(rolled, allInPct);
        default: return rolled;
    }
}

/// Node race maps while turtling: holding a majority, at most harassPct of the bots attack; with neverLast (Arathi Basin,
/// Battle for Gilneas) holding all but one, nobody chases the last one (the 5-cap). Returns the most attackers allowed (bots
/// when there is no cap). Eye of the Storm passes neverLast false: its flag scores too
inline uint32 TurtleAttackers(uint32 bots, uint32 held, uint32 total, uint32 harassPct, bool neverLast)
{
    if (!total || held * 2 <= total)
        return bots;
    if (neverLast && total >= 3 && held + 1 >= total)
        return 0;
    return std::min(bots, ShareOf(bots, harassPct));
}

// ---- focus fire (#7) and carrier play (#14) ----

/// Why a unit is worth a switch, higher first
enum class Focus : uint8
{
    None,
    Assist,         // what a real player of the team near the bot fights
    Finish,         // under 35% health and a teammate hits it
    Healer,         // an enemy healer in reach
    Marked,         // our raid's Skull or Cross
    CarrierFar,     // the enemy flag carrier within 40 yd
    Peel,           // it attacks our flag carrier
    CarrierNear     // the enemy flag carrier within 20 yd
};

inline char const* FocusName(Focus focus)
{
    switch (focus)
    {
        case Focus::Assist: return "assist";
        case Focus::Finish: return "finish";
        case Focus::Healer: return "healer";
        case Focus::Marked: return "marked";
        case Focus::CarrierFar: return "carrier";
        case Focus::Peel: return "peel";
        case Focus::CarrierNear: return "carrier near";
        default: return "-";
    }
}

/// A bot changes its target to a focus unit only for a better reason than the one it has, and not more often than minMs;
/// a target that is no focus at all (current None) gives way at once
inline bool ShouldSwitch(Focus current, Focus candidate, uint32 sinceSwitchMs, uint32 minMs)
{
    if (candidate == Focus::None || candidate <= current)
        return false;
    return current == Focus::None || sinceSwitchMs >= minMs;
}

struct Point2
{
    float X = 0.0f;
    float Y = 0.0f;
};

inline float DistSq(Point2 const& a, Point2 const& b)
{
    float dx = a.X - b.X, dy = a.Y - b.Y;
    return dx * dx + dy * dy;
}

/// count points on a circle around centre, starting at phase radians
inline std::vector<Point2> RingPoints(Point2 const& centre, float radius, uint32 count, float phase)
{
    std::vector<Point2> points;
    for (uint32 i = 0; i < count; ++i)
    {
        float angle = phase + 6.2831853f * float(i) / float(count);
        points.push_back({ centre.X + radius * std::cos(angle), centre.Y + radius * std::sin(angle) });
    }
    return points;
}

/// The kite spot of a carrier: the candidate whose nearest enemy is farthest away (the first one when there are no enemies)
inline int PickKiteSpot(std::vector<Point2> const& candidates, std::vector<Point2> const& enemies)
{
    int best = -1;
    float bestDist = -1.0f;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        float nearest = std::numeric_limits<float>::max();
        for (Point2 const& enemy : enemies)
            nearest = std::min(nearest, DistSq(candidates[i], enemy));
        if (nearest > bestDist)
        {
            best = int(i);
            bestDist = nearest;
        }
    }
    return best;
}

// ---- long routes in legs (Playerbots.Bg.LegYards): the shared rules of Travel/LegRules.h ----
using LegRules::Point3;
using LegRules::LegGoal;
using LegRules::PickLeg;

// ---- moving on (Playerbots.Bg.MoveOn) ----

constexpr uint32 Uncapped = std::numeric_limits<uint32>::max();

/// Spread (weights over targets) with a cap per target: a node under our own capture timer keeps a guard or two, what it cannot take goes
/// to the other targets by their weights. When every target is full, the rest stays unplaced (the caller sends those bots on)
inline std::vector<uint32> SpreadCapped(std::vector<float> const& weights, std::vector<uint32> const& caps, uint32 total)
{
    std::vector<uint32> counts(weights.size(), 0);
    if (weights.empty() || caps.size() != weights.size())
        return counts;

    uint32 left = total;
    while (left)
    {
        std::vector<float> open(weights.size(), 0.0f);
        float sum = 0.0f;
        size_t openCount = 0;
        for (size_t i = 0; i < weights.size(); ++i)
            if (counts[i] < caps[i])
            {
                open[i] = std::max(0.0f, weights[i]);
                sum += open[i];
                ++openCount;
            }
        if (!openCount)
            break;
        // open targets that all weigh nothing share equally
        if (sum <= 0.0f)
            for (size_t i = 0; i < weights.size(); ++i)
                if (counts[i] < caps[i])
                    open[i] = 1.0f;

        std::vector<uint32> add = Spread(open, left);
        uint32 placed = 0;
        bool clipped = false;
        for (size_t i = 0; i < weights.size(); ++i)
        {
            if (counts[i] >= caps[i])
                continue;
            uint32 room = caps[i] == Uncapped ? Uncapped : caps[i] - counts[i];
            uint32 take = std::min(add[i], room);
            counts[i] += take;
            placed += take;
            clipped |= take < add[i];
        }
        left -= placed;
        if (!clipped || !placed)
            break;
    }
    return counts;
}

/// The most bots the targets take in all (Uncapped when one of them has no cap)
inline uint32 CapTotal(std::vector<uint32> const& caps)
{
    uint64 total = 0;
    for (uint32 cap : caps)
    {
        if (cap == Uncapped)
            return Uncapped;
        total += cap;
    }
    return uint32(std::min<uint64>(total, Uncapped - 1));
}

/// Of the defenders a team wants, the ones it keeps when a target takes at most its cap: what the plain spread would put past a cap
/// moves on (attacks) rather than going to the uncapped targets, which keep their own share
inline uint32 CappedDefenders(std::vector<float> const& weights, std::vector<uint32> const& caps, uint32 want)
{
    if (weights.empty() || caps.size() != weights.size())
        return want;
    std::vector<uint32> quota = Spread(weights, want);
    uint32 keep = 0;
    for (size_t i = 0; i < quota.size(); ++i)
        keep += std::min(quota[i], caps[i]);
    return keep;
}

// ---- /bg calls of a real player (#10) ----

enum class CallKind : uint8
{
    None,
    Incoming,   // "inc LM 3": enemies come to one of our nodes
    Help,       // "help BS": the node needs people
    Defend,     // "def stables": a few more guard it
    Attack,     // "go farm": the team's next target
    Everyone    // "everyone to Drek": every attacker goes there
};

inline char const* CallKindName(CallKind kind)
{
    switch (kind)
    {
        case CallKind::Incoming: return "incoming";
        case CallKind::Help: return "help";
        case CallKind::Defend: return "defend";
        case CallKind::Attack: return "attack";
        case CallKind::Everyone: return "everyone";
        default: return "none";
    }
}

struct Call
{
    CallKind Kind = CallKind::None;
    int Node = -1;
    uint32 Count = 0;
};

/// One spelling of a node, lowercase, words separated by single spaces ("lumber mill", "lm")
struct Alias
{
    std::string Words;
    int Node = -1;
};

/// Lowercase words of a chat line: letters and digits, everything else separates
inline std::vector<std::string> CallWords(std::string_view text)
{
    std::vector<std::string> words;
    std::string word;
    for (char c : text)
    {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || u == '\'')
        {
            if (u != '\'')
                word.push_back(char(std::tolower(u)));
        }
        else if (!word.empty())
        {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty())
        words.push_back(std::move(word));
    return words;
}

/// The call in a line of instance chat. The node is the longest alias found as whole words; a number from 1 to 40 is the count;
/// the verb decides the kind ("everyone"/"all"/"zerg" > "inc" > "help" > "def" > "go"); a node with a count and no verb is an
/// "incoming" ("3 lm"). Anything else is no call
inline Call ParseCall(std::string_view text, std::vector<Alias> const& aliases)
{
    Call call;
    std::vector<std::string> words = CallWords(text);
    if (words.empty() || words.size() > 12)
        return call;

    // the node: the alias with the most words that appears as consecutive whole words (ties: the longer spelling, then the first)
    size_t bestWords = 0, bestChars = 0;
    std::vector<bool> used(words.size(), false);
    size_t usedFrom = 0, usedCount = 0;
    for (Alias const& alias : aliases)
    {
        std::vector<std::string> parts = CallWords(alias.Words);
        if (parts.empty() || parts.size() > words.size())
            continue;
        size_t chars = 0;
        for (std::string const& part : parts)
            chars += part.size();
        if (parts.size() < bestWords || (parts.size() == bestWords && chars <= bestChars))
            continue;
        for (size_t start = 0; start + parts.size() <= words.size(); ++start)
        {
            bool match = true;
            for (size_t k = 0; k < parts.size() && match; ++k)
                match = words[start + k] == parts[k];
            if (!match)
                continue;
            bestWords = parts.size();
            bestChars = chars;
            call.Node = alias.Node;
            usedFrom = start;
            usedCount = parts.size();
            break;
        }
    }
    for (size_t k = usedFrom; k < usedFrom + usedCount; ++k)
        used[k] = true;
    if (call.Node < 0)
        return call;

    bool everyone = false, incoming = false, help = false, defend = false, attack = false;
    for (size_t i = 0; i < words.size(); ++i)
    {
        if (used[i])
            continue;
        std::string const& w = words[i];
        if (w == "everyone" || w == "everybody" || w == "all" || w == "zerg")
            everyone = true;
        else if (w == "inc" || w == "incoming" || w == "incs")
            incoming = true;
        else if (w == "help" || w == "hlp" || w == "need" || w == "needs")
            help = true;
        else if (w == "def" || w == "defend" || w == "guard" || w == "defense" || w == "protect")
            defend = true;
        else if (w == "go" || w == "attack" || w == "push" || w == "cap" || w == "take" || w == "hit")
            attack = true;
        else if (!call.Count && w.size() <= 2 && std::all_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; }))
        {
            uint32 n = uint32(std::stoul(w));
            if (n >= 1 && n <= 40)
                call.Count = n;
        }
    }

    if (everyone)
        call.Kind = CallKind::Everyone;
    else if (incoming)
        call.Kind = CallKind::Incoming;
    else if (help)
        call.Kind = CallKind::Help;
    else if (defend)
        call.Kind = CallKind::Defend;
    else if (attack)
        call.Kind = CallKind::Attack;
    else if (call.Count)
        call.Kind = CallKind::Incoming;
    else
        call.Node = -1;
    return call;
}

// ---- archetypes (#12) and team skill (#20) ----

enum class Archetype : uint8
{
    Objective,      // plays the planner's job as given
    Brawler,        // fights whatever comes within 60 yd on the way, never a defender
    Turtle,         // defends first and does not leave its node for a far call
    Ninja,          // a stealth class: the first pick to sneak to an empty enemy node (#17)
    StickyHealer,   // a healer who stays with the biggest group of the team, or our carrier
    RoadFighter     // Alterac Valley: fights at the front instead of clicking banners
};

inline char const* ArchetypeName(Archetype archetype)
{
    switch (archetype)
    {
        case Archetype::Brawler: return "brawler";
        case Archetype::Turtle: return "turtle";
        case Archetype::Ninja: return "ninja";
        case Archetype::StickyHealer: return "sticky healer";
        case Archetype::RoadFighter: return "road fighter";
        default: return "objective";
    }
}

/// A bot's archetype for one match. skillPct is its team's skill: (100 - skill)% of the bots play poorly (brawlers, road
/// fighters on Alterac Valley); of the rest, stealth classes are ninjas 30% of the time, 25% turtle, the others objective
/// players. Healers stick to a group 60% of the time
inline Archetype PickArchetype(uint64 seed, uint64 guidLow, bool healer, bool stealthClass, bool valley, uint32 skillPct)
{
    uint64 roll = Mix(seed ^ Mix(guidLow ^ 0xA5A5A5A5DEADBEEFull));
    uint32 first = uint32(roll % 100);
    uint32 second = uint32((roll / 100) % 100);
    if (healer)
        return first < 60 ? Archetype::StickyHealer : Archetype::Objective;
    if (first < 100 - std::min<uint32>(100, skillPct))
        return valley && second < 50 ? Archetype::RoadFighter : Archetype::Brawler;
    if (stealthClass && second < 30)
        return Archetype::Ninja;
    if (second < 55)
        return Archetype::Turtle;
    return Archetype::Objective;
}

/// A team's skill for one match, minPct..maxPct from its seed
inline uint32 RollSkill(uint64 seed, uint32 minPct, uint32 maxPct)
{
    return RollDefendPct(seed ^ 0x51E7C0DE51E7C0DEull, minPct, maxPct);
}

/// The skill of the side a real player is on after a streak since boot: streak > 0 it won that many in a row (its bots play
/// worse), < 0 it lost that many (they play better). Clamped to minPct..maxPct
inline uint32 SkillAfterStreak(uint32 skill, int32 streak, uint32 stepPct, uint32 minPct, uint32 maxPct)
{
    if (minPct > maxPct)
        std::swap(minPct, maxPct);
    int64 value = int64(skill) - int64(streak) * int64(stepPct);
    return uint32(std::clamp<int64>(value, int64(minPct), int64(std::min<uint32>(100, maxPct))));
}

/// The streak after one more result of the side a real player was on
inline int32 NextStreak(int32 streak, bool won)
{
    if (won)
        return streak > 0 ? streak + 1 : 1;
    return streak < 0 ? streak - 1 : -1;
}

// ---- deaths at one spot (#19) ----

struct Death
{
    Point2 Where;
    uint32 AtMs = 0;
};

/// The last count deaths all fell within windowMs before now and within radius of each other
inline bool DiesAtOneSpot(std::vector<Death> const& deaths, uint32 nowMs, uint32 windowMs, float radius, uint32 count)
{
    if (count == 0 || deaths.size() < count)
        return false;
    size_t from = deaths.size() - count;
    for (size_t i = from; i < deaths.size(); ++i)
    {
        if (nowMs - deaths[i].AtMs > windowMs)
            return false;
        for (size_t j = i + 1; j < deaths.size(); ++j)
            if (DistSq(deaths[i].Where, deaths[j].Where) > radius * radius)
                return false;
    }
    return true;
}
}

#endif
