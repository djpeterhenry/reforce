# Stream guests

Viewers of a live stream can join the dance floor as dancers with their own name tag. They sign up on a web page
(a name and a dancer look); saving it or tapping "I'm here" while the stream is live checks them in. reforce polls
a small **guest service** for those check-ins and brings each guest on stage for `reforce.Guests.Minutes`.

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

**After a restart** (2026-11-27): the live instance keeps who is on the floor and when each one's stay ends in
`guests-floor.txt` next to settings.json, and brings them back when it starts again with the time they had left
(guests brought on by hand: until cleared, if reforce was gone at most 10 minutes). Blocked guests and those whose
time ran out stay away.

## Protocol

reforce polls every second:

```
GET {reforce.Guests.Url}/api/checkins?after=SEQ&msgAfter=SEQ&reqAfter=SEQ&floor=ID,ID&bust=ID:STATE:S,...&minutes=M
Authorization: Bearer {reforce.Guests.Key}        (only when the key is set)
User-Agent: reforce
```

`after`, `msgAfter` and `reqAfter` are the highest check-in, message and request sequence numbers reforce has already
handed over (`-1` at first). `floor` lists the ids of the guests on stage now (empty when nobody is); a service can show them on its
page (the maintainer's lists them so guests can pick someone to dance with). `bust` (2026-11-27) says where each
guest's "Bust a move" stands, so the service can show it on the guest's page: one `ID:STATE:S` per guest on stage
that has one (comma-separated; a guest without one is left out, so it is empty most of the time, but always sent):
`STATE` `wait` (asked, waiting for the next 8-bar point), `room` (an 8-bar point came without room to go down: put
off to the next one while the crowd makes room) or `on` (busting); `S`: whole seconds until it starts (`wait`,
`room`) or ends (`on`), left out (`ID:wait`) for the moment it is asked and not yet planned. A guest gone from the
list after `on`: it ended (or they left the floor). The seconds count down from poll to poll, so a service should
store only changes (the maintainer's keeps the state and the time it starts / ends, written again only when the state
changes or that time moves by 3 s or more: on a tempo change or a bust put off for room). A service that doesn't know
`bust` ignores it; one that does can tell an older reforce by its missing `bust`. reforce remembers them per URL (`guests-seen.txt` next to settings.json), so a restart never brings the
same check-in twice. The service answers `200` with JSON:

```json
{
  "seq": 42,
  "checkins": [
    { "seq": 41, "id": "g7f3", "name": "Sam", "dancer": { "Style": "house", "HairStyle": "afro", "Skin": "#5a3825", "Top": "#e04a2f" }, "minutes": 60 }
  ],
  "msgSeq": 9,
  "messages": [
    { "seq": 9, "id": "g7f3", "name": "Sam", "text": "hello from Berlin!" }
  ],
  "social": [
    { "id": "g7f3", "vibe": "crew", "with": "" },
    { "id": "a91c", "vibe": "mingle", "with": "g7f3" }
  ],
  "reqSeq": 5,
  "requests": [
    { "seq": 5, "id": "g7f3", "kind": "bust" }
  ]
}
```

- `seq` / `msgSeq`: the newest sequence numbers the service has. If one is lower than what reforce has seen, the
  service was reset and reforce starts over.
- `checkins`: check-ins with `seq > after`, oldest first. Return the recent ones only (the maintainer's service
  checks guests in only while reforce is live and returns the last 5 minutes, so nobody walks on after they've gone;
  every save of a dancer while live is a check-in too). `id` is stable per guest (a guest already on stage who checks
  in again is not doubled). `name` is the name tag (up to 48 characters are used).
  `dancer` holds dancer scene file keys (docs/scene-format.md, "Dancer"): letters-only keys, string, number or boolean
  values, up to 64 characters each; `Name`, `Meta`, `Vers` and `Random` are ignored. An unknown value falls back to
  the default, so a service may offer only some of the keys. A check-in for a guest already on stage changes their
  dancer in place to the look it carries (name, clothes, style) and starts their stay over from now: a service
  can send one when a guest edits their dancer while dancing. `minutes` (optional, 2026-11-27): how long the guest
  wants to stay, picked on the guest page (1 to 240 used); without it, `reforce.Guests.Minutes`. reforce sends that
  setting with every poll as `&minutes=M`, so a page can show it as the usual stay. `"session": true` (optional): the
  guest stays until the end of the stream (until reforce quits or Clear; back after a restart like those brought on
  by hand).
- `messages` (optional): speech bubbles over a guest's dancer, `seq > msgAfter`, oldest first; keep them short-lived
  (the maintainer's: 2 minutes). Up to 1024 characters of `text` are shown.

Any other status counts as an error and shows in the Dancers card (a `401` or `403` names the key). The service may
treat reforce's polls as "the stream is live", as the maintainer's sign-up page does.
- `requests` (optional, 2026-11-27): one-off requests from guests, `seq > reqAfter`, oldest first; keep them
  short-lived (the maintainer's: 2 minutes); a service may rate-limit them per guest (the maintainer's: no limit for
  now, only for a guest on the floor while live). `kind` `bust`: the page's "Bust a move" (Peter, 2026-11-27; the same button on the Dancers card): from their next 8-bar point
  their dancer does two whole break routines with floor work in them (32 bars), whatever their style, then their own
  moves again (docs/dancers-design.md "Floor moves by default, and bust a move"). reforce ignores a request for a
  guest who isn't on stage, one for a guest whose bust is already coming up or on, and kinds it doesn't know (the
  polls' `bust` shows what came of it); a service without requests just doesn't send them. `reqSeq`:
  as `seq` (lower than reforce has seen: the service was reset). reforce remembers `reqAfter` with the others.
- `social` (optional): how the guests on the floor (and those arriving) want to dance; when present, it replaces what
  reforce had, so send every guest that has a setting. `vibe`: `solo` (dances alone: never joins anyone, and nobody
  joins them), `mingle` (the default: like every other dancer; since 2026-11-27 every dancer, guest or generated, has
  the same personality, no hidden differences) or `crew` (always with others: joins the biggest group anywhere on the
  floor and stays). `with`: the id of a guest on stage to dance with: their dancer walks over to that guest (or the
  group they are in) and stays with them, following when they move on; with a `solo` guest they dance close by,
  watching. Unknown vibes count as `mingle`; a `with` not on stage is ignored.

## Testing

`reforce --background --guests-url URL [--guests-key KEY]` polls a test service (e.g. one running on this Mac at
`http://127.0.0.1:8787`) even from a background test copy, without disturbing a live reforce: guests on, nothing
remembered (no `guests-seen.txt`) and no past guests saved. Add `--dancer-test house,pop` for a few generated dancers
around them.
