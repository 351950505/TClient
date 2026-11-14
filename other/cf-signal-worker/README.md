# TClient public Cloudflare signal relay

Windows v1.7.8.82 connects directly to `wss://signal.me3.dpdns.org/ws` without a test code. This is a new Worker (`tclient-signal-production`) and new SQLite Durable Objects; the private prototype and legacy `tc1` remain separate.

## Protocol and lifecycle

- One globally addressed `SignalRoom` per exact game-server address.
- WebSocket upgrade query: `room`, `uid` (16 lowercase hex), `session` (16 lowercase hex).
- `W|uid|boot|members` welcomes a connection.
- `S|sequence|payload` sends a PING; the payload uses the existing exact `⟨B:` plus six alphabet characters.
- `D|uid|session|sequence|payload` is broadcast to other connected members.
- `A|sequence|accepted|boot` counts accepted server socket writes, not end-device delivery.
- `PING`/`PONG` uses the hibernation auto-response API. Per-socket attachments restore identity, sequence and rate window after reconstruction. No offline message history or room timers.
- Disconnects require client reconnect/backoff. The client does not replay uncertain or expired messages.

## Resource bounds

- 32 connections per room, 8 per observed IP hash within a room.
- Frames at most 512 bytes; signals at most160 bytes with exact PING encoding validation.
- 12 application messages per5 seconds per connection; auto-response heartbeats are separate.
- Admission: 20 connection attempts per IP/minute, 2000 admitted attempts/day and64 distinct rooms/day; UTC day resets. Counters survive admission-object reconstruction. Old per-IP timestamps are pruned.
- IPs are SHA-256 hashed with the current UTC day before storage/forwarding. The hash is never broadcast to players.

These are operational bounds for a small deployment, not authentication or comprehensive denial-of-service protection. Room names/uids are discovery hints: a modified client can join a known room and send valid signals. Global quotas can be consumed by hostile traffic and cause429 for legitimate users. Observe Cloudflare requests/duration and429 logs; change limits deliberately, not by declaring the free tier unbounded.

## Deployment and validation

Run `node worker.test.mjs` (mocked unit tests), then `npx wrangler deploy --config wrangler.toml`. The configuration owns only `signal.me3.dpdns.org`; do not attach `sig`, `git` or the private prototype hostname.

Before a client release, validate actual no-key WSS send/receive, reconnect, room isolation, disabled switch, zero UDP registration in CF-only mode, and normal TLS verification. Tests at one time/network do not prove evening-peak availability. Stable Windows builds default CF enabled and CF-only; old saved disabled settings are preserved, and only known prototype URLs are migrated.

No Aliyun service is stopped or changed by deployment. Old clients on Aliyun cannot exchange signals with CF-only clients; coordinate client upgrades. Non-Windows clients currently retain UDP because the WSS transport uses WinHTTP.
