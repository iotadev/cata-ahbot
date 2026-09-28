# Cataclysm Auction House Bot module

Optional static AHBot module for this TrinityCore Cataclysm 4.3.4 fork. It owns
the native seller, buyer, command script, runtime-derived market catalog,
default-off supply profile, stack shaping, staggered turnover, and buyer cadence
controls developed in this checkout.

This is a separate source repository intended to be checked out at
`modules/mod-ahbot` inside a matching core fork. It is not installable against
stock TrinityCore or an arbitrary core revision. Use the matching source
snapshot of the [iotadev TrinityCore fork](https://github.com/iotadev/TrinityCore);
the core README records the AHBot revision tested with that snapshot.

This is a source module, not a drop-in AzerothCore binary module. It uses the
fork's optional-module build convention and a narrow core bridge for world
initialization, updates, and auction-mail identity checks.

From the matching core root, the module builds by default when present.
Disable it at configure time with:

```text
cmake -S . -B build -DMODULE_MOD_AHBOT=OFF
```

Copy `conf/ahbot.conf.dist` to `modules/ahbot.conf` beside the active
`worldserver.conf` before adding overrides. The template is deliberately inert:
no account is selected and seller/buyer behavior is disabled. Legacy
`AuctionHouseBot.*` main-config settings remain compatible during this first
migration slice.

The module boundary has been build-tested both enabled and with
`MODULE_MOD_AHBOT=OFF`. A prior development-checkpoint replay loaded an active
`ahbot.conf`, exercised both command forms, realized the seller target, and
completed the capped buyer phase with clean shutdowns. The publication candidate
passed the four core/module build combinations documented in the core repository;
its exact final revisions have not received a new seller/buyer runtime replay.

Pricing is intentionally unchanged. Current valuation is suitable for mechanics
tests, not a claim about a healthy Cataclysm economy. Market candidates continue
to come from authoritative loaded Cata data rather than imported WotLK tables.

This module carries TrinityCore-derived GPL-2.0-or-later source. Original file
notices remain in place; see [LICENSE](LICENSE),
[TrinityCore authors](TRINITYCORE_AUTHORS), and [provenance](PORTING.md).

Reports should include core/module commit IDs and reproduction steps. Remove
credentials and private account details from configuration excerpts and logs;
do not upload database dumps, clients, or extracted game data.
