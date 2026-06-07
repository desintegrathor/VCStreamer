# Dev Log

## Spectator chat relay

- IDA finding: `game.dll:0xED3990` reads client chat packets but only relays when the sender is host/DC or has a live entity pointer at `player+0xF4`; it also excludes recipients whose `player+0x10 == 2`, so spectator-origin chat and spectator recipients are dropped by native flow.
- Implemented `SpectatorChatRelay` hook for `game.dll+0x163990`. Normal player chat falls through to native handling. Spectator/dummy senders are rebuilt as native server chat packets (`0x23`, 13-bit sender handle, public flag, 7-bit null-terminated text), unicasted to every connected player handle including spectators and the sender, then passed to `GNET_ProcessChatMessage` under the dedicated-server guard.
# 2026-06-06

- Replaced the YouTube normal-chat relay with the VCHD streamer export path. `YoutubeChatBridge` now queues structured `{author, message}` items, retries until VCHD reports VCAC readiness, and submits comments through `VCHD_StreamerSubmitYoutubeChat` without building game chat packets.
- Updated `tools/youtube_chat_bridge.py` and generated/default YouTube chat config for JSON author/message records, `max_chars=480`, and the current stream URL `https://youtube.com/live/xd1t2lS0Xb4?feature=share`.
