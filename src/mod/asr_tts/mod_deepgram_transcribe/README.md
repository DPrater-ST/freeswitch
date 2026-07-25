# mod_deepgram_transcribe

A FreeSWITCH module that streams a call leg's audio to `rtc-transcription`'s
self-hosted-Deepgram `/ws/transcribe` endpoint for real-time transcription.

Unlike [deepgram/freeswitch_modules'](https://github.com/deepgram/freeswitch_modules)
`mod_deepgram_transcribe` (which speaks Deepgram's own hosted streaming API
directly), this module talks to `rtc-transcription` — an internal
ServiceTitan service wrapping a self-hosted Deepgram engine — so it speaks
that service's own small control protocol
(see `rtc-transcription/CLAUDE.md` "Protocol") rather than Deepgram's.

## API

### Commands

```
uuid_deepgram_transcribe <uuid> start [stereo] [bugname]
```
Attaches a media bug to the channel and starts streaming to
`rtc-transcription`.

```
uuid_deepgram_transcribe <uuid> stop [bugname]
```
Stops transcription on the channel.

### Channel Variables

| variable | Description |
| --- | ----------- |
| `DEEPGRAM_TRANSCRIPTION_HOST` | Host/IP of the `rtc-transcription` instance (required). |
| `DEEPGRAM_TRANSCRIPTION_PORT` | Port of the `rtc-transcription` instance (default: 80). |

### Events

- `deepgram_transcribe::connect` — the WebSocket session to `rtc-transcription` is up and any buffered audio has been flushed.
- `deepgram_transcribe::connect_failed` — connect/handshake/start failed. Body is a JSON `{"type":"error","error":"..."}`.
- `deepgram_transcribe::transcription` — an `utterance` event from `rtc-transcription`, passed through as-is (see `rtc-transcription/CLAUDE.md` for the body shape).
- `deepgram_transcribe::disconnect` — the WebSocket session ended (clean close or after `stop`).

## Audio path

FreeSWITCH's linear PCM is resampled to 8kHz (if the channel's native rate
differs) and encoded to mu-law before being sent as binary WebSocket frames —
`rtc-transcription`'s `DeepgramOptions` defaults to `mulaw@8kHz`, not raw
linear PCM.

## WebSocket client

`ws_client.c`/`.h` is a minimal, dependency-free (beyond OpenSSL's EVP digest
API for the handshake) RFC 6455 client, scoped to exactly what
`rtc-transcription`'s protocol needs: masked client frames out, unmasked
server frames in, no TLS/wss, no extensions, no ping/pong. See the doc
comment in `ws_client.h` for the full scope statement.
