/**
  * @name MarketBotMgr.h
  *   system for automating/emulating buy and sell orders on the market.
  * idea and some code taken from AuctionHouseBot - Copyright (C) 2008-2014 TrinityCore <http://www.trinitycore.org/>
  * @Author:         Allan
  * @date:   10 August 2016
  * @version:  0.15 (config version)
  */

#include "eve-server.h"
#include "EVEServerConfig.h"
#include "market/MarketBotConf.h"
#include "market/MarketBotMgr.h"
#include "market/MarketMgr.h"
#include "market/MarketProxyService.h"

// ---marketbot update; everything past this point has been completely changed.
#include "market/MarketDB.h"
#include "inventory/ItemType.h"
#include "inventory/ItemFactory.h"
#include "inventory/InventoryItem.h"
#include "station/StationDataMgr.h"
#include "system/SystemManager.h"
#include "system/SystemEntity.h"
#include <random>
#include <cstdint>
#include <chrono>
#include <set>

extern SystemManager* sSystemMgr;

static constexpr int64 FILETIME_TICKS_PER_DAY = 864000000000;  // 100ns ticks per day; for expelorders to be removed prematurally

static const uint32 MARKETBOT_MAX_ITEM_ID = 30000;

// See A329 §3.4 — all published marketable categories (matches seed-market coverage)
static const std::set<uint32> VALID_CATEGORIES = {
    4,   // Material
    5,   // Accessories
    6,   // Ship
    7,   // Module
    8,   // Charge
    9,   // Blueprint
    16,  // Skill
    17,  // Commodity
    18,  // Drone
    22,  // Deployable
    23,  // Starbase
    24,  // Reaction
    25,  // Asteroid
    32,  // Subsystem
    34,  // Ancient Relics
    35,  // Decryptors
    39,  // Infrastructure Upgrades
    40,  // Sovereignty Structures
    41,  // Planetary Interaction
    42,  // Planetary Resources
    43,  // Planetary Commodities
    46   // Orbitals
};

static constexpr uint32 BOT_OWNER_ID = 1000125; // NPC corp owner, default CONCORD

// helper random generators
int GetRandomInt(int min, int max) {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng);
}

float GetRandomFloat(float min, float max) {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(min, max);
    return dist(rng);
}

MarketBotDataMgr::MarketBotDataMgr() {
    m_initalized = false;
}

int MarketBotDataMgr::Initialize() {
    m_initalized = true;
    sLog.Blue(" MarketBotDataMgr", "Market Bot Data Manager Initialized."); // load current data
    return 1;
}

MarketBotMgr::MarketBotMgr() {
    m_initalized = false;
}

int MarketBotMgr::Initialize() {
    if (!sMBotConf.ParseFile(sConfig.files.marketBotSettings.c_str())) {
        sLog.Error("       ServerInit", "Loading Market Bot Config file '%s' failed.", sConfig.files.marketBotSettings.c_str());
        return 0;
    }

    m_initalized = true;
    sMktBotDataMgr.Initialize();

    m_nextRunTime = Clock::now() + std::chrono::minutes(sMBotConf.main.DataRefreshTime);
    sLog.Cyan("     MarketBotMgr", "Timer initialized. First automated cycle will run in %d minutes.", sMBotConf.main.DataRefreshTime);

    sLog.Blue("     MarketBotMgr", "Market Bot Manager Initialized.");
    return 1;
}

// Called on minute tick from EntityList
void MarketBotMgr::Process(bool overrideTimer) {
    TimePoint now = Clock::now();

    sLog.Green("     MarketBotMgr", "MarketBot Process() invoked on tick.");
    codelog(MARKET__TRACE, "MarketBot Process() invoked on tick.");

    sLog.Green("     MarketBotMgr", "Entered MarketBotMgr::Process()\n");
    codelog(MARKET__TRACE, ">> Entered MarketBotMgr::Process()");

    if (!m_initalized) {
        sLog.Error("     MarketBotMgr", "MarketBotMgr not initialized � skipping run\n");
        codelog(MARKET__ERROR, "Process() called but MarketBotMgr is not initialized.");
        return;
    }

    if (!overrideTimer && now + std::chrono::seconds(5) < m_nextRunTime) { // ---marketbot update; 5 second jitter
        auto timeLeft = std::chrono::duration_cast<std::chrono::milliseconds>(m_nextRunTime - now).count();
        if (timeLeft > 0) {
            sLog.Green("     Trader Joe", "Update timer not ready yet. Next run in %lld seconds.", timeLeft / 1000);
            codelog(MARKET__TRACE, "Trader Joe waiting � next run in %lld seconds.", timeLeft);
            return;
        }
    }
    
    sLog.Green("     Market Bot Mgr", "Processing old orders...\n");
    codelog(MARKET__TRACE, "Processing old orders...");
    ExpireOldOrders();

    std::vector<uint32> eligibleSystems = GetEligibleSystems();
    sLog.Green("     Market Bot Mgr", "Trader Joe found %zu eligible systems for order placement.", eligibleSystems.size());
    codelog(MARKET__TRACE, "Trader Joe found %zu eligible systems for order placement.", eligibleSystems.size());

    int totalBuyOrders = 0;
    int totalSellOrders = 0;
    int expiredOrders = ExpireOldOrders();

    for (uint32 systemID : eligibleSystems) {
        sLog.Green("     Market Bot Mgr", "Trader Joe placing orders in systemID: %u", systemID);
        codelog(MARKET__TRACE, "Trader Joe placing orders in systemID: %u", systemID);

        totalBuyOrders += PlaceBuyOrders(systemID);
        totalSellOrders += PlaceSellOrders(systemID);
    }

    sLog.Green("     Trader Joe", "Master Summary: Created %d buy orders and %d sell orders across %u systems. Removed %d old orders.",
    totalBuyOrders, totalSellOrders, static_cast<uint32>(eligibleSystems.size()), expiredOrders);

    codelog(MARKET__TRACE, "Trader Joe Master Summary: Created %d buy orders and %d sell orders across %zu systems. Removed %d old orders.",
        totalBuyOrders, totalSellOrders, eligibleSystems.size(), expiredOrders);

    sLog.Green("     Trader Joe", "Cycle complete. Resetting timer.");
    codelog(MARKET__TRACE, "Trader Joe cycle complete. Resetting timer.");
    m_nextRunTime = Clock::now() + std::chrono::minutes(sMBotConf.main.DataRefreshTime);
    sLog.Green("     Trader Joe", "Timer reset. Next run in %d minutes.", sMBotConf.main.DataRefreshTime);
}

void MarketBotMgr::ForceRun(bool resetTimer) {
    sLog.Warning("     ForceRun", "Manually starting Trader Joe.");

    if (!m_initalized) {
        sLog.Yellow("     Trader Joe", "MarketBotMgr not initialized � skipping run.");
        return;
    }

    sLog.Green("     Trader Joe", "Running Process() now...");
    this->Process(true);  // force override
    sLog.Green("     Trader Joe", "Finished Process().");
    
    if (resetTimer) {
        m_nextRunTime = Clock::now() + std::chrono::minutes(sMBotConf.main.DataRefreshTime);
        sLog.Green("     Trader Joe", "Timer reset. Next run in %d minutes.", sMBotConf.main.DataRefreshTime);
    }
}

void MarketBotMgr::AddSystem() { /* To be implemented if needed */ }
void MarketBotMgr::RemoveSystem() { /* To be implemented if needed */ }

int MarketBotMgr::ExpireOldOrders() {
    uint64_t now = GetFileTimeNow();

    DBQueryResult res;
    DBResultRow row;

    int expiredCount = 0;

    sLog.Yellow("     Trader Joe", "ExpireOldOrders: now = %" PRIu64, now);
    codelog(MARKET__TRACE, "ExpireOldOrders: now = %" PRIu64, now);

    if (!sDatabase.RunQuery(res,
        "SELECT orderID FROM mktOrders WHERE (issued + CAST(duration AS UNSIGNED) * %" PRIu64 ") < CAST(%" PRIu64 " AS UNSIGNED) AND ownerID = %u AND volEntered != 550",
        FILETIME_TICKS_PER_DAY, now, BOT_OWNER_ID)) {
        codelog(MARKET__DB_ERROR, "Failed to query expired bot orders.");
        return 0;
    }

    while (res.GetRow(row)) {
        uint32 orderID = row.GetUInt(0);
        MarketDB::DeleteOrder(orderID);
        ++expiredCount;
        codelog(MARKET__TRACE, "Expired Trader Joe order %u", orderID);
    }

    return expiredCount;
}

int MarketBotMgr::PlaceBuyOrders(uint32 systemID) {
    SystemData sysData;
    if (!sDataMgr.GetSystemData(systemID, sysData)) {
        codelog(MARKET__ERROR, "Failed to get system data for system %u", systemID);
        return 0;
    }

    std::vector<uint32> availableStations;
    if (!sDataMgr.GetStationListForSystem(systemID, availableStations)) {
        codelog(MARKET__ERROR, "No stations found for system %u", systemID);
        return 0;
    }

    size_t stationCount = availableStations.size();
    size_t stationLimit = stationCount;
    std::shuffle(availableStations.begin(), availableStations.end(), std::mt19937{std::random_device{}()});

    int orderCount = 0;

    for (size_t i = 0; i < std::min<size_t>(stationLimit, sMBotConf.main.OrdersPerRefresh); ++i) {
        uint32 stationID = availableStations[i];
        uint32 itemID = SelectRandomItemID();
        const ItemType* type = sItemFactory.GetType(itemID);
        if (!type) continue;

        uint32 quantity = GetRandomQuantity(type->groupID());
        double price = CalculateBuyPrice(itemID);

        if (price * quantity > sMBotConf.main.MaxISKPerOrder) {
            if (quantity > 1) {
                quantity = 1;
                if (price > sMBotConf.main.MaxISKPerOrder) {
                    codelog(MARKET__TRACE, "Skipping itemID %u due to price %.2f ISK exceeding MaxISKPerOrder.", itemID, price);
                    continue;
                }
                codelog(MARKET__TRACE, "Price too high for bulk, retrying with quantity = 1 for itemID %u", itemID);
            } else {
                codelog(MARKET__TRACE, "Skipping itemID %u even at quantity = 1 due to price %.2f ISK", itemID, price);
                continue;
            }
        }

        double escrow = price * quantity;

        Market::SaveData order;
        order.typeID = itemID;
        order.regionID = sysData.regionID;
        order.stationID = stationID;
        order.solarSystemID = systemID;
        order.minVolume = 1;
        order.volEntered = quantity;
        order.volRemaining = quantity;
        order.price = price;
        order.escrow = escrow; // required, without this no one can sell items to bot orders.
        order.duration = sMBotConf.main.OrderLifetime;
        order.bid = true;
        order.issued = GetFileTimeNow();
        order.isCorp = false;
        order.ownerID = BOT_OWNER_ID;
        order.orderRange = 32767; // -1 station, 0 solarsystem, 1-5 10 20 30 40 jumps, 32767 region (default)
        // marketbot update; unsure if these are really needed, but aligns with how orders are created manually in-game and through seed-market.
        order.memberID = 0;       // default value for who placed the order (0 for char order)
        order.accountKey = 1000;  // default value for corp account key

        bool success = MarketDB::StoreOrder(order);
        if (success) {
            ++orderCount;
            codelog(MARKET__TRACE, "%s order created for typeID %u, qty %u, price %.2f ISK, station %u",
                (order.bid ? "BUY" : "SELL"), order.typeID, order.volEntered, order.price, order.stationID);
        } else {
            codelog(MARKET__ERROR, "Failed to store %s order for typeID %u at station %u",
                (order.bid ? "BUY" : "SELL"), order.typeID, order.stationID);
        }
    }
    return orderCount;
}

int MarketBotMgr::PlaceSellOrders(uint32 systemID) {
    SystemData sysData;
    if (!sDataMgr.GetSystemData(systemID, sysData)) {
        codelog(MARKET__ERROR, "Trader Joe: Failed to get system data for system %u", systemID);
        return 0;
    }

    std::vector<uint32> availableStations;

    if (!sDataMgr.GetStationListForSystem(systemID, availableStations)) {
        codelog(MARKET__ERROR, "Trader Joe: No stations found for system %u � skipping order creation.", systemID);
        return 0;
    } else {
        codelog(MARKET__TRACE, "Trader Joe: Found %zu stations in system %u", availableStations.size(), systemID);
    }

    size_t stationCount = availableStations.size();
    size_t stationLimit = stationCount;
    std::shuffle(availableStations.begin(), availableStations.end(), std::mt19937{std::random_device{}()});

    int orderCount = 0;

    for (size_t i = 0; i < std::min<size_t>(stationLimit, sMBotConf.main.OrdersPerRefresh); ++i) {
        uint32 stationID = availableStations[i];
        uint32 itemID = SelectRandomItemID();
        const ItemType* type = sItemFactory.GetType(itemID);
        if (!type) continue;

        uint32 quantity = GetRandomQuantity(type->groupID());
        double price = CalculateSellPrice(itemID);

        if (price * quantity > sMBotConf.main.MaxISKPerOrder) {
            if (quantity > 1) {
                quantity = 1;
                if (price > sMBotConf.main.MaxISKPerOrder) {
                    codelog(MARKET__TRACE, "Skipping itemID %u due to price %.2f ISK exceeding MaxISKPerOrder.", itemID, price);
                    continue;
                }
                codelog(MARKET__TRACE, "Price too high for bulk, retrying with quantity = 1 for itemID %u", itemID);
            } else {
                codelog(MARKET__TRACE, "Skipping itemID %u even at quantity = 1 due to price %.2f ISK", itemID, price);
                continue;
            }
        }

        Market::SaveData order;
        order.typeID = itemID;
        order.regionID = sysData.regionID;
        order.stationID = stationID;
        order.solarSystemID = systemID;
        order.minVolume = 1;
        order.volEntered = quantity;
        order.volRemaining = quantity;
        order.price = price;
        order.escrow = 0; // not required for sell orders; place here for possible future use.
        order.duration = sMBotConf.main.OrderLifetime;
        order.bid = false;
        order.issued = GetFileTimeNow();
        order.isCorp = false;
        order.ownerID = BOT_OWNER_ID;
        order.orderRange = -1; // -1 station (default), 0 solarsystem, 1-5 10 20 30 40 jumps, 32767 region
        // marketbot update; unsure if these are really needed, but aligns with how orders are created manually in-game and through seed-market.
        order.memberID = 0;       // default value for who placed the order (0 for char order)
        order.accountKey = 1000;  // default value for corp account key

        codelog(MARKET__TRACE, "System %u maps to region %u via GetSystemData", systemID, sysData.regionID);

        codelog(MARKET__TRACE, "Trader Joe: Storing sell order with orderRange = %u", order.orderRange);

        bool success = MarketDB::StoreOrder(order);
        if (success) {
            ++orderCount;
            codelog(MARKET__TRACE, "Trader Joe: Creating %s order for typeID %u, qty %u, price %.2f, station %u, region %u",
                (order.bid ? "BUY" : "SELL"), order.typeID, order.volEntered, order.price, order.stationID, order.regionID);
        } else {
            codelog(MARKET__ERROR, "Trader Joe: Failed to store %s order for typeID %u at station %u",
                (order.bid ? "BUY" : "SELL"), order.typeID, order.stationID);
        }
    }
    return orderCount;
}

std::vector<uint32> MarketBotMgr::GetEligibleSystems() {
    bool useStaticSystems = false; // ---marketbot update; only turn to true, for testing changes with markbot or want to have it only populate orders in one system
    if (useStaticSystems) {
        return { 30000142 };  // Jita (default system for testing)
    }

    // See A329 §3.5 — configurable system coverage per cycle
    std::vector<uint32> systemIDs;
    sDataMgr.GetRandomSystemIDs(sMBotConf.main.SystemsPerCycle, systemIDs);
    codelog(MARKET__TRACE, "GetEligibleSystems(): Pulled %zu systems from StaticDataMgr (configured: %u)", systemIDs.size(), sMBotConf.main.SystemsPerCycle);
    return systemIDs;
}

// See A329 §3.4 — category-based item selection (replaces group whitelist)
uint32 MarketBotMgr::SelectRandomItemID() {
    uint32 itemID = 0;
    const ItemType* type = nullptr;
    uint32 tries = 0;

    do {
        ++tries;
        itemID = GetRandomInt(10, MARKETBOT_MAX_ITEM_ID);
        type = sItemFactory.GetType(itemID);

        if (type && type->published() && type->basePrice() > 0 && VALID_CATEGORIES.count(type->categoryID())) {
            codelog(MARKET__TRACE, "Selected itemID %u (catID %u) after %u attempts", itemID, type->categoryID(), tries);
            return itemID;
        }
    } while (tries < 100);

    // If we fail after 100 attempts, log a warning and return fallback value
    codelog(MARKET__WARNING, "Failed to select valid itemID after %u attempts. Returning fallback itemID = 34 (Tritanium)", tries);
    return 34;  // Tritanium, as a safe default
}

// See A329 §3.6 — category-based quantity logic
uint32 MarketBotMgr::GetRandomQuantity(uint32 groupID) {
    // For category-based selection, we use groupID to determine bulk vs unit quantity.
    // Bulk groups: minerals (18), ores (450-469), ammo/charges (83-92, 372-396, 648-772)
    if (
        groupID == 18 ||                      // Minerals
        (groupID >= 83 && groupID <= 92) ||   // Basic ammo/charges
        (groupID >= 372 && groupID <= 396) || // Advanced ammo/missiles
        (groupID >= 450 && groupID <= 469) || // Raw ores
        groupID == 479 ||                     // Scanner Probes
        groupID == 482 ||                     // Mining Crystals
        groupID == 648 ||                     // Advanced Rocket
        (groupID >= 653 && groupID <= 657) || // Advanced Missiles
        groupID == 772                        // Assault Missiles
    ) {
        return GetRandomInt(1000, 1000000);  // Large stack sizes
    }

    // Medium-volume: modules, drones, implants, small items
    if (
        groupID < 450 ||   // Most modules, skills, etc.
        groupID >= 538     // Data miners, probes, etc.
    ) {
        return GetRandomInt(1, 50);  // Ships and expensive items get low quantities
    }

    // Fallback
    return GetRandomInt(10, 500);
}

// See A329 §3.3 — configurable price multipliers for economy reform
double MarketBotMgr::CalculateBuyPrice(uint32 itemID) {
    const ItemType* type = sItemFactory.GetType(itemID);
    return type ? type->basePrice() * sMBotConf.main.BuyPriceMultiplier * GetRandomFloat(0.9f, 1.1f) : 1000.0;
}

// See A329 §3.3 — sell at configured markup (default 5×) over basePrice
double MarketBotMgr::CalculateSellPrice(uint32 itemID) {
    const ItemType* type = sItemFactory.GetType(itemID);
    return type ? type->basePrice() * sMBotConf.main.SellPriceMultiplier * GetRandomFloat(0.9f, 1.1f) : 1000.0;
}
