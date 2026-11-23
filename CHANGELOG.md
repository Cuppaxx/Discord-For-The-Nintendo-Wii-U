# Changelog

## v1.0.0 (first public release)

**Chat**
* Servers, folders, channels, DMs and group DMs, with unread markers and toasts for new DMs.
* Send, reply, edit, delete, pin, mention and react; markdown, link cards, typing indicators.
* Emoji picker with an *All servers* tab, a GIF search, stickers, animated GIF/WebP/MP4.
* Voice messages (2 min) and video clips (30 s).

**Voice & video**
* Voice channels and DM calls with Discord's end-to-end encryption (DAVE).
* Outgoing call ringing, and incoming calls with a ringtone.
* Per-user volume up to 300%, noise suppression, echo reduction, push to talk, mic test.
* GamePad camera with adjustable exposure and saturation; watch other people's cameras and Go Live streams.
* Video tiles: the member/volume menu is reachable from the name strip, "…" chip, Options chip or −.

**Reliability**
* Compressed gateway (zlib-stream): large accounts log in in about 7 s on hardware.
* Automatic reconnect plus session resume after network drops.
* Fixes for real hardware: UDP receive buffer alignment, friends who send 40/60 ms Opus packets, and a hang when closing the app.
* Desktop H.264 cameras at High@5.2 decode on the Wii U decoder.

**UI**
* QR-code login, a new logo and launch screen, and a bird.
* Log out moved from − into Settings (press twice).
