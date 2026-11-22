# Stream guests

Viewers of a live stream can join the dance floor as dancers with their own name tag. They sign up on a web page
(a name and a dancer look) and tap "I'm here" while watching; reforce polls a small **guest service** for those
check-ins and brings each guest on stage for `reforce.Guests.Minutes`.

reforce contains only the client side. You run the guest service yourself, anywhere that serves HTTPS: the protocol
below is all reforce needs. (The maintainer's is a Cloudflare Worker with a sign-up page that makes a dancer look
from a photo; it is a separate project.)

## Settings

| Setting | Default | |
|---|---|---|
| `reforce.Guests.Enabled` | 0 | poll the guest service (also the Dancers card) |
| `reforce.Guests.Url` | (none) | the service's base URL, e.g. `https://guests.example.com`; nothing is polled until it is set |
| `reforce.Guests.Key` | (none) | a read key, sent as `Authorization: Bearer KEY` (keep it private) |
| `reforce.Guests.Minutes` | 30 | how long a guest stays on stage |
| `reforce.Guests.Blocked` | (none) | guest ids never put on stage |

```sh
reforce --write-setting "reforce.Guests.Url=https://guests.example.com" \
        --write-setting "reforce.Guests.Key=$(openssl rand -hex 24)" \
        --write-setting "reforce.Guests.Enabled=1"
```

Only the live instance polls: never offline (`--deterministic`) or in a `--background` test copy. Every guest's
latest dancer is also kept on this Mac (`~/Library/Application Support/reforce/Guests/`), so past guests can be
brought back from the Dancers card.

## Protocol

reforce polls every 5 seconds:

```
GET {reforce.Guests.Url}/api/checkins?after=SEQ&msgAfter=SEQ
Authorization: Bearer {reforce.Guests.Key}        (only when the key is set)
User-Agent: reforce
```

`after` and `msgAfter` are the highest check-in and message sequence numbers reforce has already handed over (`-1`
at first). reforce remembers them per URL (`guests-seen.txt` next to settings.json), so a restart never brings the
same check-in twice. The service answers `200` with JSON:

```json
{
  "seq": 42,
  "checkins": [
    { "seq": 41, "id": "g7f3", "name": "Sam", "dancer": { "Style": "house", "HairStyle": "afro", "Skin": "#5a3825", "Top": "#e04a2f" } }
  ],
  "msgSeq": 9,
  "messages": [
    { "seq": 9, "id": "g7f3", "name": "Sam", "text": "hello from Berlin!" }
  ]
}
```

- `seq` / `msgSeq`: the newest sequence numbers the service has. If one is lower than what reforce has seen, the
  service was reset and reforce starts over.
- `checkins`: check-ins with `seq > after`, oldest first. Return the recent ones only (the maintainer's service:
  the last 3 hours), so guests who tapped before the stream started still arrive. `id` is stable per guest (a guest
  already on stage who checks in again is not doubled). `name` is the name tag (up to 48 characters are used).
  `dancer` holds dancer scene file keys (docs/scene-format.md, "Dancer"): letters-only keys, string, number or boolean
  values, up to 64 characters each; `Name`, `Meta`, `Vers` and `Random` are ignored. An unknown value falls back to
  the default, so a service may offer only some of the keys.
- `messages` (optional): speech bubbles over a guest's dancer, `seq > msgAfter`, oldest first; keep them short-lived
  (the maintainer's: 2 minutes). Up to 1024 characters of `text` are shown.

Any other status counts as an error and shows in the Dancers card (a `401` or `403` names the key). The service may
treat reforce's polls as "the stream is live", as the maintainer's sign-up page does.
