# mochi-manifold-kalshi-bot

A low-latency trading bot that keeps [Manifold Markets](https://manifold.markets) prices in line with
[Kalshi](https://kalshi.com). It streams Kalshi's order books and Manifold's prices over WebSockets.
When a Manifold answer drifts past the Kalshi quote, it sends an immediate-or-cancel limit order that
moves Manifold back to the quote.

The repo has two parts:

| Part | What it is |
|---|---|
| `cpp/` | **The trader** (`hybrid_bot`). A single-threaded C++20 event loop (epoll on Linux, kqueue on macOS) whose only dependency is OpenSSL 3. Reads `config_websocket.yaml` and `.env`. |
| `control_bot.py` + `make_configs.py`, `make_manifoldsports_configs.py`, `reap_configs.py` | **The control bot**. A Telegram bot that finds and connects Manifold↔Kalshi market pairs, edits the config, and restarts the trader, all from your phone. |

> **Risk.** This places real orders with your Manifold account. Run with `dry_run: true` until the
> log looks right. No warranty; see [Disclaimer](#disclaimer).

---

## How it trades

For every pair (one Manifold answer ↔ one Kalshi market), on every price change from either side:

1. **Quote.** The Kalshi best bid and ask come from the live order book (`orderbook_delta`). The 1 Hz
   `ticker` feed is used only when the book is empty or stale. On two-way games (NBA/NFL/NHL/MLB), the
   other team's leg (`partner_ticker`) also prices the question, and the two are merged:
   - The tighter of the two quotes is used on each side.
   - If the legs cross during a live game, the bot follows the leg that moved last, for a short trust window.
2. **Decide.** Buy YES when the Kalshi bid is above Manifold's price; buy NO when the Kalshi ask is
   below it. The edge required to add inventory grows with your position (`min_edge_threshold` up to
   `max_edge_threshold`). Reducing a position only needs `clearing_edge_threshold`.
   Optionally, buying a long shot needs extra edge (`underdog_threshold` / `underdog_min_edge`).
3. **Order.** The bot sends an IOC limit order at the Kalshi touch, or `limit_offset` inside it, sized by
   edge (`max_bet_size`, `size_edge_power`). Orders go out on pre-warmed keep-alive connections with
   Manifold's `deps=[]` fast path.
4. **Game over.** Once Kalshi pins at the extreme band (`extreme_high`), one large order pushes Manifold
   toward the final value.

Optional: with `kalshi_probe_candidates > 0`, the trader opens N extra Kalshi connections at startup,
measures them for about a minute, and keeps the fastest. A connection's speed is fixed for its
lifetime, so each restart re-rolls it.

---

## Repository layout

```
cpp/
  Makefile                 make -> build/hybrid_bot, build/replay, build/unit_tests
  src/                     engine (bot_core), Kalshi/Manifold clients (price_sources, net), loop, json/yaml
  tests/unit_tests.cpp     make test
  deploy/run_trader_cpp.sh respawn loop for tmux (what control_bot /restart kills)
  deploy/manifold-bot-cpp.service   systemd user unit, the alternative to tmux
control_bot.py             Telegram control bot
make_configs.py            pair matching + config writer (also a CLI)
make_manifoldsports_configs.py   @ManifoldSports games -> Kalshi legs (/sports)
reap_configs.py            archives pairs whose markets are over (/reap, or cron)
kalshi_aliases.yaml        manual name-matching fixes (/rewrite writes here)
config.example.yaml        base config -> copy to config_websocket.yaml
.env.example               secrets template -> copy to .env
```

---

## Setting it up on a cloud server

Manifold's API runs in Google Cloud **us-east4** (behind Cloudflare). A small VM in that region keeps
order latency low. The steps below are for Debian/Ubuntu on GCP; any Linux host works the same way.

### 1. Create the VM

```bash
gcloud compute instances create mochi-bot \
  --zone us-east4-a --machine-type e2-small \
  --image-family debian-12 --image-project debian-cloud
gcloud compute ssh mochi-bot --zone us-east4-a
```

One trader uses a few percent of a vCPU and about 30 MB of RAM, so `e2-small` is plenty.

### 2. Install the toolchain

```bash
sudo apt update
sudo apt install -y build-essential g++ pkg-config libssl-dev git tmux python3 python3-venv
```

You need g++ 11 or newer (C++20).

### 3. Get the code and build

```bash
git clone https://github.com/royluo20/mochi-manifold-kalshi-bot.git ~/mochi-manifold-kalshi-bot
cd ~/mochi-manifold-kalshi-bot
make -C cpp -j"$(nproc)"
./cpp/build/unit_tests            # -> "all built-in checks passed"
```

After copying in changed sources (scp, unzip), build with **`make -B -C cpp`**. Copied files can keep
older timestamps than the existing objects, so plain `make` may think nothing changed and keep running
the old binary. Check what you built with `strings cpp/build/hybrid_bot | grep <some_new_key>`.

### 4. Secrets

```bash
cp .env.example .env && chmod 600 .env
# Kalshi: Account -> API keys -> create; save the private key next to .env
nano kalshi_key.pem && chmod 600 kalshi_key.pem
nano .env
```

Fill in `MANIFOLD_API_KEY`, `MANIFOLD_USER_ID`, `KALSHI_API_KEY` and `KALSHI_PRIVATE_KEY_FILE`. Keep
comments in `.env` on their own lines, never after a value. `.env` and `*.pem` are git-ignored; keep
them that way.

### 5. Base config

```bash
cp config.example.yaml config_websocket.yaml
```

It starts with `dry_run: true` and conservative edges and sizes. See
[Configuration](#configuration) for what to tune.

### 6. Control bot (Telegram)

1. Create a bot with [@BotFather](https://t.me/BotFather) and put its token in `CONTROL_TELEGRAM_BOT_TOKEN`.
   Give the control bot its **own** bot: Telegram allows one command listener per token.
2. Send your new bot any message. Then read your chat id from
   `curl -s https://api.telegram.org/bot<TOKEN>/getUpdates` (`message.chat.id`), and put it in
   `TELEGRAM_CHAT_IDS`. The control bot refuses to start without an allowlist and ignores every other chat.
3. Install its Python dependencies:

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

### 7. Run both under tmux

```bash
tmux new -s arb
# pane 1: the trader, in a respawn loop (logs to trader.log)
./cpp/deploy/run_trader_cpp.sh config_websocket.yaml
# Ctrl-b c  -> pane 2: the control bot
.venv/bin/python control_bot.py
# Ctrl-b d to detach; `tmux attach -t arb` to come back
```

With the `.env.example` restart settings (`CONTROL_RESTART_COMMAND=pkill -f cpp/build/hybrid_bot`),
`/restart` kills the trader and the loop starts it again within about 3 s.

**Or use systemd** for the trader:

```bash
mkdir -p ~/.config/systemd/user && cp cpp/deploy/manifold-bot-cpp.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now manifold-bot-cpp
sudo loginctl enable-linger "$USER"     # keep user services running after logout
```

Then switch `.env` to the systemd block (`CONTROL_RESTART_MODE=systemd`,
`CONTROL_SERVICE_UNIT=manifold-bot-cpp.service`). The unit assumes the repo is at `~/mochi-manifold-kalshi-bot`.

### 8. First run checklist

With `dry_run: true`, watch `tail -f trader.log` for a few minutes:

- The startup banner names the `epoll` backend and an open-file limit in the thousands.
- There's one `[Kalshi] Subscribed to N markets` line per socket (90 tickers each).
- You see `Connected to Manifold WebSocket`, and **no** `[Kalshi] error frame` lines.
- `[DRY RUN]` decisions appear on pairs you expect to be mispriced.

Then `/dryrun off` and `/restart`.

---

## Using the control bot

Nothing is written until `/confirm`, and **nothing is live until `/restart`**. The trader reads its
config once, at startup.

**Connect today's games:**
```
/sports nba --date today          preview: every @ManifoldSports NBA game Kalshi lists
/confirm                          write the pairs to config_websocket.yaml
/restart                          trader picks them up
```

**Change settings on pairs already connected** with `--overwrite`. Flags you pass win; keys you don't
pass are left as they were:
```
/sports nba --date today --min-edge 0.01 --limit-offset 0 --max-bet 500 --overwrite
/confirm
/restart
```

Rules learned the hard way:

- **One connect, then `/confirm`.** Each connect command (`/sports`, `/nba`, `/connect`, ...)
  *replaces* whatever is staged. A second `/sports` before `/confirm` discards the first one's flags.
- **Check the preview.** With `--overwrite` it lists every `key: old -> new (n pairs)`. If your new
  keys aren't listed, they won't be written.
- **Write thresholds as decimals:** `--underdog-threshold 0.15`, not `15` or `15%`.
- **Overrides apply only to the pairs that command wrote.** Tomorrow's games are new pairs and need
  the flags again. Settings you want everywhere belong in the `bot:` block: `/bot <key> <value>`.
- **Verify after `/restart`.** The trader prints each pair's overrides at startup:
  `grep "Override \[" trader.log | tail`. A key missing there is a key the running binary ignored.

Commands (`/help` in Telegram lists them all):

| | |
|---|---|
| Connect | `/sports [nba nfl nhl epl ...] [--date D]`, `/nba` `/nfl` `/nhl` `/mlb` `/epl` `/ncaa` `/atp` `/wta` `[date or market]`, `/fixtures <manifold> <SERIES>`, `/connect <kalshi> <manifold>`, `/add <manifold> <answer_id> <TICKER>` |
| Review | `/confirm`, `/cancel`, `/fix <n> <TICKER>`, `/rewrite <text> = <replacement>` |
| Look up | `/answers <manifold> [search]`, `/kalshi <ticker> [search]` |
| Manage | `/pairs`, `/drop <slug or event or n>`, `/reap [dry]`, `/status`, `/bot [key value]`, `/dryrun on/off`, `/log [lines]`, `/undo`, `/config <path>`, `/restart` |

Per-pair flags on any connect command: `--min-edge`, `--min-edge-floor`, `--max-edge`,
`--clearing-edge`, `--min-clearing-room`, `--limit-offset`, `--max-bet`, `--max-position`,
`--size-edge-power`, `--max-spread`, `--cooldown`, `--extreme-mult`, `--amm [false]`,
`--settle-slam`, `--tie-resolves no|half|ignore`, `--underdog-threshold`, `--underdog-min-edge`,
`--partner-trust-ms`, `--partner-max-lead`, `--reverse`. Filters: `--date`, `--contains`,
`--min-prob`, `--max-prob`.

The same matching runs from a shell, for example `python3 make_configs.py sport nba --write config_websocket.yaml`.
`python3 reap_configs.py --config config_websocket.yaml` archives finished games; run it from cron.

---

## Configuration

`config.example.yaml` documents every key. The ones that matter most:

| Key | Effect |
|---|---|
| `dry_run` | `true` = log only. |
| `min_edge_threshold`, `min_edge_floor` | Edge needed to add inventory. Kalshi and Manifold move in whole cents, so `0.01` trades every 1-cent gap; `0.02` skips them. |
| `max_edge_threshold`, `edge_scale_start`, `max_position` | Inventory brake: the required edge rises as the position grows. |
| `clearing_edge_threshold` | Edge needed to reduce a position. |
| `limit_offset` | `0.01` places the limit one cent inside the Kalshi touch (safer); `0` places it at the touch (more fills). |
| `max_bet_size`, `size_edge_power` | Order size at a given edge. |
| `underdog_threshold`, `underdog_min_edge` | Extra edge required when an order buys an outcome priced under the threshold (adding or clearing). Off by default. |
| `partner_legs`, `partner_trust_ms`, `partner_max_lead` | Two-leg quoting on two-way games. |
| `kalshi_probe_candidates` | Connection selection at startup (C++ only). Off by default. |

Any key marked `(pair)` in the example can be set on a single pair, where it overrides `bot:`.

Environment knobs, for testing: `KALSHI_WS_URL`, `MANIFOLD_API_BASE`, `MANIFOLD_WS_URL`,
`ORDER_TRACE_FILE` (one JSON line per completed order), `BOT_DEBUG=1` (verbose decisions; slows the hot path).

---

## Operating notes

- **Logs.** `trader.log` (`/log 100` from Telegram). The trader logs each decision, fill, reconnect and Kalshi error frame.
- **Updating.** `git pull && make -B -C cpp && ./cpp/build/unit_tests`, then `/restart`. A running
  trader keeps its old binary in memory until it restarts.
- **One trader per account and config.** Two copies send every order twice. Separate accounts running
  this same code also queue behind each other on Manifold's per-market order queue.
- **Manifold restarts** its WebSocket daily around 08:00 UTC. The trader reconnects by itself;
  an exit during startup is restarted by the loop or by systemd.
- **Kalshi limits.** 200 WebSocket connections per user (the probe stays under
  `kalshi_probe_max_conns`). Each socket carries 90 tickers.

## Building on macOS

`brew install openssl@3 && make -C cpp`. The Makefile finds Homebrew's OpenSSL. Never build with
`-ffast-math`, and keep `-ffp-contract=off` (already in the Makefile). The engine's price rounding
relies on exact IEEE arithmetic.

## Disclaimer

Provided as-is, without warranty of any kind. You are responsible for your account, your orders, and for
following Manifold's and Kalshi's terms of service and API rules. Nothing here is financial advice.

## License

TBD. Add a `LICENSE` file before publishing; without one, nobody else has the right to use the code.
