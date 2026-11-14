// Standalone test of the professions rules (Economy/ProfessionsRules.h), see tests/run_engine_test.sh

#include "Economy/ProfessionsRules.h"
#include <cstdio>

using namespace Playerbots::Prof::Rules;

namespace
{
int Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
}

int main()
{
    // outfit storage round trip; malformed parts and slots past the set are dropped
    std::vector<OutfitPiece> pieces = { { 0, 12345 }, { 4, 9876543210ull }, { 15, 7 } };
    CHECK(EncodeOutfit(pieces) == "0:12345;4:9876543210;15:7");
    CHECK(DecodeOutfit(EncodeOutfit(pieces), 19) == pieces);
    CHECK(DecodeOutfit("0:1;x:2;3:;;4:0;20:5;5:6", 19) == std::vector<OutfitPiece>({ { 0, 1 }, { 5, 6 } }));
    CHECK(DecodeOutfit("", 19).empty());

    // names: lower case, trimmed, no separators, 24 characters at most
    CHECK(CleanOutfitName("  PvP Set ") == "pvp set");
    CHECK(CleanOutfitName("a|b;c:d") == "abcd");
    CHECK(CleanOutfitName("   ").empty());
    CHECK(CleanOutfitName("abcdefghijklmnopqrstuvwxyz").size() == 24);
    CHECK(SplitNames("tank|heal||pvp set") == std::vector<std::string>({ "tank", "heal", "pvp set" }));
    CHECK(JoinNames({ "tank", "pvp set" }) == "tank|pvp set");
    CHECK(SplitNames("").empty());

    // orders
    CHECK(ParseOutfitOrder("").Verb == OutfitVerb::List);
    CHECK(ParseOutfitOrder(" list ").Verb == OutfitVerb::List);
    OutfitOrder save = ParseOutfitOrder("save PvP Set");
    CHECK(save.Verb == OutfitVerb::Save && save.Name == "pvp set");
    OutfitOrder plus = ParseOutfitOrder("+tank");
    CHECK(plus.Verb == OutfitVerb::Save && plus.Name == "tank");
    OutfitOrder minus = ParseOutfitOrder("-tank");
    CHECK(minus.Verb == OutfitVerb::Delete && minus.Name == "tank");
    OutfitOrder show = ParseOutfitOrder("?tank");
    CHECK(show.Verb == OutfitVerb::Show && show.Name == "tank");
    OutfitOrder wear = ParseOutfitOrder("Tank");
    CHECK(wear.Verb == OutfitVerb::Equip && wear.Name == "tank");
    OutfitOrder equip = ParseOutfitOrder("equip heal");
    CHECK(equip.Verb == OutfitVerb::Equip && equip.Name == "heal");
    CHECK(ParseOutfitOrder("delete").Name.empty());

    // reagents: the scarcest one decides, reagent-free entries do not
    CHECK(AffordableCasts({ { 10, 2 }, { 3, 1 } }, 5) == 3);
    CHECK(AffordableCasts({ { 10, 2 }, { 3, 1 } }, 2) == 2);
    CHECK(AffordableCasts({ { 1, 2 } }, 5) == 0);
    CHECK(AffordableCasts({ { 0, 0 } }, 4) == 4);

    if (Failures)
        std::printf("professions_rules_test: %d failures\n", Failures);
    else
        std::printf("professions_rules_test: all passed\n");
    return Failures ? 1 : 0;
}
