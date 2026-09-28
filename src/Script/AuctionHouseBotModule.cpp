/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Released under GNU GPL v2 or any later version.
 */
#include "AuctionHouseBotModule.h"
#include "AuctionHouseBot.h"
#include "Log.h"

void InitializeAuctionHouseBotModule()
{
    TC_LOG_INFO("server.loading", "Initialize optional mod-ahbot...");
    sAuctionBot->Initialize();
}

void UpdateAuctionHouseBotModule()
{
    sAuctionBot->Update();
}

bool IsAuctionHouseBotCharacter(uint32 guidLow)
{
    return sAuctionBotConfig->IsBotChar(guidLow);
}
