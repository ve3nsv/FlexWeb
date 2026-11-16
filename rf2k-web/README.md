# rf2k-web — a web page for the RF-KIT RF2K-S

A small server for the LAN that is the RF2K-S's **only** REST client. It polls the
amp, pushes the readings to every open browser over Server-Sent Events, and
passes commands to the amp in the same queue as the polls. No FlexWeb and no radio
needed.

```
browsers ──SSE / POST──▶ rf2kweb.py ──one HTTP request at a time──▶ RF2K-S :8080
```

Python 3.9 or newer, standard library only (no `pip install`). Runs on the shack
PC or a Raspberry Pi.

## Run it

```bash
python3 rf2kweb.py --amp 10.0.0.25:8080          # then open http://<this machine>:8088/
```

| Option | Default | |
|---|---|---|
| `--amp host[:port]` | `10.0.0.25:8080` | the amp's REST server (`$RF2K_AMP`) |
| `--listen host[:port]` | `0.0.0.0:8088` | where browsers connect (`$RF2K_LISTEN`) |
| `--password-file FILE` | — | control password (or `$RF2K_WEB_PASSWORD`). With one, browsers are view-only until they sign in |
| `--read-only` | off | nobody can control the amp from the page |
| `--keep-awake SECS` | `60` | repeat the wake request this often while a controlling browser is open (`0` = once) |
| `--grace SECS` | `30` | stop polling this long after the last browser closes |
| `-v` | | debug log |

`?compact` in the URL (or a tap on the AMP chip) shows only the chip, e.g. for a
small always-on-top window.

`rf2k-web.service` is a systemd unit to start it at boot; adjust the paths and the
user and copy it to `/etc/systemd/system/`.

### Only one program may poll the amp

The amp's REST server handles one connection at a time. **Run FlexWeb without
`--rfkit` while rf2k-web is running.**

## Who may control it

- **No password (the default):** every browser that reaches port 8088 can control
  the amp. The server prints a warning at startup. Fine on a trusted shack LAN.
- **Password** (`--password-file` or `RF2K_WEB_PASSWORD`): everyone can watch; only
  a browser that has signed in can press OPERATE, TUNE, RESET or the antennas.
  Sign-in lasts 30 days, until the server restarts, or until you sign out.
- **`--read-only`:** a display only.

Commands must be POSTed as JSON from the same origin, and the sign-in cookie is
`SameSite=Strict`, so another web page cannot use your browser to press buttons.
The server speaks plain HTTP. Don't expose it to the internet. For access from
outside, use a VPN, or a reverse proxy with HTTPS and its own login.

## What it does to the amp

These are the rules FlexWeb learned from the real amp:

1. **One request at a time.** Polls, wakes and commands share one queue. Commands
   go ahead of queued polls but stay in click order.
2. **A request is never abandoned early.** The only timeout is 15 s with no bytes,
   and it restarts whenever bytes arrive. Even when polling stops, the request in
   flight runs to its end.
3. **Polling.** One GET per tick, and only when nothing is queued or in flight. A
   slow answer (over 1 s) pauses polling for as long as the answer took.
   - Receive, 250 ms ticks: `/power` and `/data` every 0.75 s, `/operate-mode`
     every 1.5 s, `/tuner` about every 1.5 s. `/antennas` and
     `/operational-interface` every 10 s.
   - Transmit (forward ≥ 1 W, and for 3 s after), 25 ms ticks: `/power` four
     times in five, `/data` once. For 30 s after TUNE, or while the amp reports
     tuning, one `/power` in five is replaced by `/tuner`, so TUNING shows while
     your carrier is up.
   - `/data` is read straight away after an OPERATE→STANDBY you didn't ask for,
     or after forward power collapses mid-over.
4. **Wake.** `PUT /tuner {}` is sent 3 s after connecting while a browser that may
   control is open, then every `--keep-awake` seconds. Its HTTP status shows
   whether TUNE exists: 2xx/400 = yes, 404/405/501 = no. A wake that gets no
   answer doesn't count toward dropping the link.
5. **Offline.** 3 failures in a row, or one request with no bytes for the full
   15 s, means the amp is offline. Before that the page shows **NO ANSWER N s**
   from 2 s. Retries come at 5, 15 and 30 s, then every 60 s. The interval goes
   back to 5 s once the amp answers `/info` as an RF-KIT.
6. **Never assume.** After OPERATE, an antenna change, RESET or TUNE, the server
   reads the state back, and the page repaints only from that readback. The
   amp's refusal text (400/409 `reason`) is returned to the browser that sent the
   command.
7. **Only while someone looks.** Nothing is sent before the first browser opens,
   and polling stops `--grace` seconds after the last one closes.

`/api/state` returns the latest state as JSON (for scripts). It doesn't count as
a browser, so it doesn't start polling.

## Develop and test

```bash
python3 mock_amp.py --port 8090 --simulate     # a fake RF2K-S; keys an over every 20 s in OPERATE
python3 rf2kweb.py --amp 127.0.0.1:8090
python3 -m unittest discover -s tests -v       # ~45 s, no amp needed
```

The mock is single-threaded like the real server. Its modified `PUT /tuner`
answers `{}` with 400 and `START` with a tune. Tests can make it go silent,
answer slowly, refuse commands, send a non-RF-KIT body, or act as a stock server
without `PUT /tuner`. The tests check the rules above against it, including that
the mock never sees two requests at once.

## Files

| File | |
|---|---|
| `rf2kweb.py` | entry point / options |
| `ampclient.py` | the HTTP client, request queue, poll schedule, wake, offline/backoff (port of FlexWeb's `RfkitConnection`) |
| `rfkit.py` | the API codec and presentation helpers (port of FlexWeb's `rfkitprotocol`) |
| `webapp.py` | HTTP server, SSE, commands, sign-in |
| `static/` | the page (`index.html`, `app.js`, `style.css`) |
| `mock_amp.py` | fake RF2K-S for development and tests |
| `tests/` | `unittest` suites |
