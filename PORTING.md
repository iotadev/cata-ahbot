# Source provenance and compatibility

This Cataclysm AHBot module was extracted from the native AuctionHouseBot
implementation in the Cataclysm Preservation Project TrinityCore fork. The
starting upstream core revision for this development checkout is
`9da95e6cc9c2cdd82c0d3778a3720d2fe77be8ff` at
https://github.com/The-Cataclysm-Preservation-Project/TrinityCore.

The source files formerly under `src/server/game/AuctionHouseBot/` now live in
`src/Bot/`; `src/server/scripts/Commands/cs_ahbot.cpp` now lives in
`src/Script/`. The extraction also includes local Cata market-policy changes
developed in the core checkout. The original per-file notices and the
TrinityCore project attribution are retained. This new repository begins from
a reviewed source snapshot; the earlier per-file commit history remains in the
upstream/core Git history, not in this repository's initial commit.

The matching core fork provides optional-module discovery, configuration
loading, and narrow AHBot initialization/update/auction-mail identity hooks.
This module is compiled statically into that core. It is not an AzerothCore
binary module or a promise of compatibility with other TrinityCore revisions.
The first public release must record an exact matching core commit and replay
the enabled/disabled build and disposable seller/buyer checks from those exact
revisions.

License: GNU GPL version 2 or later, consistent with the retained source
headers. See [LICENSE](LICENSE) and [TRINITYCORE_AUTHORS](TRINITYCORE_AUTHORS).
