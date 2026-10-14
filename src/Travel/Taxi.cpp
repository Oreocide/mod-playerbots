#include "Taxi.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "TaxiPathGraph.h"

namespace Playerbots
{
bool PickRandomTaxiDestination(Player* bot, uint32 fromNode, uint32& destNode)
{
    TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(fromNode);
    if (!from)
        return false;

    std::vector<uint32> candidates;
    uint32 nodeCount = sTaxiNodesStore.GetNumRows();
    for (uint32 id = 1; id < nodeCount; ++id)
    {
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(id);
        if (!node || node->ID == fromNode || node->ContinentID != from->ContinentID)
            continue;
        if (!node->IsPartOfTaxiNetwork())
            continue;

        std::vector<uint32> path;
        if (!TaxiPathGraph::GetCompleteNodeRoute(from, node, bot, path) || path.size() < 2)
            continue;
        candidates.push_back(node->ID);
    }

    if (candidates.empty())
        return false;

    destNode = candidates[urand(0, uint32(candidates.size() - 1))];
    return true;
}

bool TryTakeTaxi(Player* bot, Creature* flightMaster, uint32 destNode)
{
    if (!bot || !flightMaster || bot->IsInFlight() || bot->IsInCombat())
        return false;

    uint32 from = sObjectMgr->GetNearestTaxiNode(flightMaster->GetPositionX(), flightMaster->GetPositionY(),
        flightMaster->GetPositionZ(), flightMaster->GetMapId(), bot->GetTeam());
    if (!from)
        return false;

    bot->m_taxi.SetTaximaskNode(from);
    bot->m_taxi.SetTaximaskNode(destNode);

    TaxiNodesEntry const* fromEntry = sTaxiNodesStore.LookupEntry(from);
    TaxiNodesEntry const* toEntry = sTaxiNodesStore.LookupEntry(destNode);
    if (!fromEntry || !toEntry)
        return false;

    std::vector<uint32> nodes;
    if (!TaxiPathGraph::GetCompleteNodeRoute(fromEntry, toEntry, bot, nodes) || nodes.size() < 2)
        return false;

    return bot->ActivateTaxiPathTo(nodes, flightMaster);
}

bool TakeTaxi(Player* bot, uint32 fromNode, uint32 destNode)
{
    if (!bot || bot->IsInFlight() || bot->IsInCombat() || !fromNode || !destNode)
        return false;

    TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(fromNode);
    TaxiNodesEntry const* to = sTaxiNodesStore.LookupEntry(destNode);
    if (!from || !to)
        return false;

    // a bot has discovered every node it needs
    bot->m_taxi.SetTaximaskNode(fromNode);
    bot->m_taxi.SetTaximaskNode(destNode);

    std::vector<uint32> nodes;
    if (!TaxiPathGraph::GetCompleteNodeRoute(from, to, bot, nodes) || nodes.size() < 2)
        return false;

    for (uint32 node : nodes)
        bot->m_taxi.SetTaximaskNode(node);

    return bot->ActivateTaxiPathTo(nodes, nullptr);
}
}
