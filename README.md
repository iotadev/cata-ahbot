# Cataclysm Auction House Bot module

Optional static AHBot module for this TrinityCore Cataclysm 4.3.4 fork. It owns
the native seller, buyer, command script, runtime-derived market catalog,
default-off supply profile, stack shaping, staggered turnover, and buyer cadence
controls developed in this checkout.

This is a separate source repository intended to be checked out at
`modules/mod-ahbot` inside a matching core fork. It is not installable against
stock TrinityCore or an arbitrary core revision. A compatible core commit will
be pinned here when the coordinated first public snapshot is selected.

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

The current build boundary has been verified both enabled and with
`MODULE_MOD_AHBOT=OFF`. The enabled disposable replay loads an active
`ahbot.conf`, exercises both command forms, realizes the exact seller target,
and completes the capped buyer phase with clean shutdowns.

Pricing is intentionally unchanged. Current valuation is suitable for mechanics
tests, not a claim about a healthy Cataclysm economy. Market candidates continue
to come from authoritative loaded Cata data rather than imported WotLK tables.

This module carries TrinityCore-derived GPL-2.0-or-later source. Original file
notices remain in place; see [LICENSE](LICENSE),
[TrinityCore authors](TRINITYCORE_AUTHORS), and [provenance](PORTING.md).
