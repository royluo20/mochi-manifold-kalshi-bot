#!/usr/bin/env python3
"""Archive market pairs whose underlying markets are over.

Config hygiene, not a safety net: the trading engine already stops trading a
resolved market on its own (`handle_lifecycle` in price_sources.py flips
`is_live = False` and nulls the quotes on a terminal Kalshi lifecycle event). The
reaper's job is to keep configs from accumulating dead weight — every stale pair
costs a WebSocket subscription slot, a REST position poll, and startup time.

Because it isn't protecting against bad trades, it is deliberately conservative:

  * Reap when the **Manifold leg** is resolved — the authoritative "this is over".
  * Reap when the source leg is terminal **and** Manifold has already converged
    past `extreme_converge_high`.
  * Otherwise wait out `--grace-minutes` after the source leg first went
    terminal.

That grace period exists to protect the end-of-game slam. Kalshi settles
*before* Manifold resolves, and in that window the bot's extreme path is still
slamming Manifold toward the terminal value (`extreme_bet_multiplier`) — reaping
on the Kalshi leg alone would cut off exactly the most profitable minutes.

Pairs are moved to an archive file rather than deleted, so a false positive costs
a copy-paste instead of a rebuild.

Run it on a cron on the bot host:
    */30 * * * * cd /path/to/manifold-bot && python reap_configs.py --config config_websocket.yaml

    python reap_configs.py --config config_websocket.yaml --dry-run
    python reap_configs.py --config config_websocket.yaml --telegram
"""
import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from typing import Dict, List, Optional, Tuple

import requests
import yaml
from dotenv import load_dotenv

from make_configs import pair_key, remove_market_pairs, render_pair_entries

load_dotenv()

KALSHI_API_BASE = "https://api.elections.kalshi.com/trade-api/v2"
MANIFOLD_API_BASE = "https://api.manifold.markets/v0"

DEFAULT_CONFIG = "config_websocket.yaml"
DEFAULT_ARCHIVE = "config_archive.yaml"
STATE_FILE = "reap_state.json"

# REST statuses meaning "no longer tradeable". Distinct from the WebSocket
# lifecycle events in bot_core.TERMINAL_EVENTS ({determined, settled,
# deactivated}) — the REST market endpoint reports a trading-lifecycle status,
# so 'closed' (trading shut, settlement pending) counts as terminal here and
# starts the grace clock.
TERMINAL_STATUSES = {"closed", "settled", "finalized", "determined", "deactivated"}

DEFAULT_CONVERGE_HIGH = 0.97
DEFAULT_GRACE_MINUTES = 60


# ── Telegram (optional digest) ────────────────────────────────────────────────

def send_telegram(message: str) -> None:
    token = os.getenv("TELEGRAM_BOT_TOKEN")
    raw_ids = os.getenv("TELEGRAM_CHAT_IDS") or os.getenv("TELEGRAM_CHAT_ID") or ""
    chat_ids = [c.strip() for c in raw_ids.split(",") if c.strip()]
    if not token or not chat_ids:
        print("# TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_IDS not set — skipping digest")
        return
    for chat_id in chat_ids:
        try:
            resp = requests.post(
                f"https://api.telegram.org/bot{token}/sendMessage",
                json={"chat_id": chat_id, "text": message,
                      "disable_web_page_preview": True},
                timeout=10,
            )
            if not resp.ok:
                print(f"# Telegram error for {chat_id}: {resp.text[:200]}")
        except requests.RequestException as e:
            print(f"# Telegram send failed for {chat_id}: {e}")


# ── State (first time each pair was seen terminal) ────────────────────────────

def load_state(path: str = STATE_FILE) -> Dict[str, float]:
    if not os.path.exists(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f) or {}
    except (OSError, json.JSONDecodeError):
        print(f"# {path} unreadable — starting fresh")
        return {}


def save_state(state: Dict[str, float], path: str = STATE_FILE) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2, sort_keys=True)
    os.replace(tmp, path)


def state_key(pair: dict) -> str:
    source, answer = pair_key(pair)
    return f"{source}|{answer}"


# ── Market state lookups ──────────────────────────────────────────────────────

class Lookup:
    """Fetches and caches market state for both legs."""

    def __init__(self, timeout: int = 20):
        self.timeout = timeout
        self.session = requests.Session()
        self._manifold: Dict[str, Optional[dict]] = {}
        self._kalshi: Dict[str, Optional[dict]] = {}

    def manifold_market(self, slug: str) -> Optional[dict]:
        if slug not in self._manifold:
            try:
                r = self.session.get(f"{MANIFOLD_API_BASE}/slug/{slug}",
                                     timeout=self.timeout)
                self._manifold[slug] = r.json() if r.ok else None
                if not r.ok:
                    print(f"#   ⚠ Manifold {slug}: HTTP {r.status_code}")
            except (requests.RequestException, ValueError) as e:
                print(f"#   ⚠ Manifold {slug}: {e}")
                self._manifold[slug] = None
        return self._manifold[slug]

    def kalshi_market(self, ticker: str) -> Optional[dict]:
        if ticker not in self._kalshi:
            try:
                r = self.session.get(f"{KALSHI_API_BASE}/markets/{ticker}",
                                     timeout=self.timeout)
                self._kalshi[ticker] = (r.json() or {}).get("market") if r.ok else None
                if not r.ok:
                    print(f"#   ⚠ Kalshi {ticker}: HTTP {r.status_code}")
            except (requests.RequestException, ValueError) as e:
                print(f"#   ⚠ Kalshi {ticker}: {e}")
                self._kalshi[ticker] = None
        return self._kalshi[ticker]


def manifold_leg_closed(market: dict, now: float) -> bool:
    """True when the Manifold market is past its closeTime but NOT yet resolved.

    This is the state that actually costs money, and the reaper used to be blind
    to it because it only looked at `isResolved`. A closed market takes no bets,
    so its probability FREEZES — while the source venue keeps moving. The gap
    between them grows into a large phantom edge that the bot will keep firing
    at, every order rejected.

    Measured 2026-10-12: `muse-spark-14-meta-release-date` closed that morning
    unresolved, froze at 0.861, and Polymarket had since moved to 0.215 — a 64
    point "edge" into a market that cannot accept a trade. Resolution can lag a
    close by days on these.
    """
    close_ms = market.get("closeTime")
    return bool(close_ms) and (close_ms / 1000.0) < now and not market.get("isResolved")


def manifold_leg_state(lookup: Lookup, pair: dict,
                       now: Optional[float] = None) -> Tuple[Optional[bool], Optional[float]]:
    """(is_over, probability) for the Manifold leg. (None, None) if unknown.

    "Over" means resolved OR past close — either way the leg cannot be traded,
    which is what the caller needs to know.

    Unknown is not "over" — a transient API failure must never reap a live
    pair, so callers treat None as "leave it alone".
    """
    slug = pair.get("manifold_slug")
    if not slug:
        return None, None
    market = lookup.manifold_market(slug)
    if market is None:
        return None, None

    if now is not None and manifold_leg_closed(market, now):
        return True, None

    answer_id = pair.get("manifold_answer_id")
    if answer_id:
        for answer in market.get("answers") or []:
            if answer.get("id") == answer_id:
                prob = answer.get("probability")
                if prob is None:
                    prob = answer.get("prob")
                resolved = (answer.get("resolution") is not None
                            or bool(market.get("isResolved")))
                return resolved, prob
        # The answer is gone from the market — treat as over.
        print(f"#   answer {answer_id} no longer on {slug} — treating as resolved")
        return True, None

    return bool(market.get("isResolved")), market.get("probability")


def source_leg_terminal(lookup: Lookup, pair: dict) -> Tuple[Optional[bool], str]:
    """(is_terminal, detail) for the Kalshi leg.

    Polymarket pairs have no equivalent check here — they carry only a CLOB token
    id, with no cheap status endpoint keyed by it — so they are reaped on the
    Manifold leg alone, which is the authoritative signal anyway.
    """
    ticker = pair.get("kalshi_ticker")
    if not ticker:
        return None, "no kalshi_ticker (polymarket or binary pair)"
    market = lookup.kalshi_market(ticker)
    if market is None:
        return None, "kalshi lookup failed"
    status = (market.get("status") or "").lower()
    result = market.get("result") or ""
    terminal = status in TERMINAL_STATUSES or result not in ("", "void")
    detail = f"status={status or '?'}" + (f" result={result}" if result else "")
    return terminal, detail


# ── Reap decision ─────────────────────────────────────────────────────────────

def decide(pair: dict, lookup: Lookup, state: Dict[str, float],
           converge_high: float, grace_minutes: float,
           now: float) -> Tuple[bool, str]:
    """(reap?, reason). Conservative: anything unknown means keep."""
    resolved, prob = manifold_leg_state(lookup, pair, now)
    if resolved:
        market = lookup.manifold_market(pair.get("manifold_slug"))
        if market is not None and manifold_leg_closed(market, now):
            return True, "manifold past closeTime (unresolved — price frozen)"
        return True, "manifold resolved"

    terminal, detail = source_leg_terminal(lookup, pair)
    key = state_key(pair)

    if not terminal:
        state.pop(key, None)  # recovered or never terminal — reset the clock
        return False, "active"

    first_seen = state.setdefault(key, now)

    if prob is not None:
        converged_high = prob >= converge_high
        converged_low = prob <= (1.0 - converge_high)
        if converged_high or converged_low:
            return True, f"source terminal ({detail}) + manifold converged at {prob:.3f}"

    waited_min = (now - first_seen) / 60.0
    if waited_min >= grace_minutes:
        prob_str = f"{prob:.3f}" if prob is not None else "unknown"
        return True, (f"source terminal ({detail}) for {waited_min:.0f}min "
                      f"> {grace_minutes:.0f}min grace; manifold at {prob_str}")

    return False, (f"source terminal ({detail}) but only {waited_min:.0f}min ago — "
                   f"holding for the extreme slam ({grace_minutes:.0f}min grace)")


# ── Archiving ─────────────────────────────────────────────────────────────────

def archive_pairs(pairs: List[dict], reasons: Dict[str, str], archive_path: str,
                  source_config: str, dry_run: bool = False) -> None:
    if not pairs:
        return
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    annotated = []
    for p in pairs:
        entry = dict(p)
        entry["notes"] = (f"reaped {stamp} from {source_config}: "
                          f"{reasons.get(state_key(p), 'resolved')}")
        annotated.append(entry)

    block = render_pair_entries(annotated)
    if dry_run:
        print(f"# --- would archive {len(pairs)} pairs to {archive_path} ---")
        return

    exists = os.path.exists(archive_path)
    with open(archive_path, "a", encoding="utf-8") as f:
        if not exists:
            f.write("# Pairs reaped from the live configs by reap_configs.py.\n"
                    "# Append-only history — safe to prune by hand. Not read by any bot.\n"
                    "market_pairs:\n")
        f.write(block)
    print(f"# ✓ Archived {len(pairs)} pairs to {archive_path}")


# ── Main ──────────────────────────────────────────────────────────────────────

def reap(config_path: str, args, lookup: Lookup, state: Dict[str, float]) -> List[str]:
    """Reap one config. Returns human-readable lines describing what was reaped."""
    if not os.path.exists(config_path):
        print(f"# {config_path} not found — skipping")
        return []

    with open(config_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f) or {}
    pairs = [p for p in (cfg.get("market_pairs") or []) if isinstance(p, dict)]
    if not pairs:
        print(f"# {config_path}: no market pairs")
        return []

    bot_cfg = cfg.get("bot") or {}
    converge_high = float(args.converge_high
                          if args.converge_high is not None
                          else bot_cfg.get("extreme_converge_high", DEFAULT_CONVERGE_HIGH))

    print(f"\n# === {config_path}: {len(pairs)} pairs "
          f"(converge_high={converge_high}, grace={args.grace_minutes}min) ===")

    now = time.time()
    doomed, reasons, held = [], {}, 0
    for pair in pairs:
        should_reap, reason = decide(pair, lookup, state, converge_high,
                                    args.grace_minutes, now)
        name = pair.get("name") or pair_key(pair)[0]
        if should_reap:
            doomed.append(pair)
            reasons[state_key(pair)] = reason
            print(f"#   REAP {name}: {reason}")
        else:
            if reason != "active":
                held += 1
                print(f"#   hold {name}: {reason}")

    if not doomed:
        print(f"# Nothing to reap in {config_path}"
              + (f" ({held} terminal but within grace)" if held else ""))
        return []

    archive_pairs(doomed, reasons, args.archive, config_path, dry_run=args.dry_run)
    removed = remove_market_pairs({pair_key(p) for p in doomed}, config_path,
                                  dry_run=args.dry_run)

    if not args.dry_run:
        for pair in doomed:
            state.pop(state_key(pair), None)

    return [f"{p.get('name') or pair_key(p)[0]} — {reasons.get(state_key(p), '')}"
            for p in (removed or doomed)]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", action="append", metavar="PATH",
                    help=f"config to reap; repeatable (default: {DEFAULT_CONFIG})")
    ap.add_argument("--archive", default=DEFAULT_ARCHIVE,
                    help=f"where reaped pairs are appended (default: {DEFAULT_ARCHIVE})")
    ap.add_argument("--grace-minutes", type=float, default=DEFAULT_GRACE_MINUTES,
                    help="wait this long after the source leg goes terminal before "
                         f"reaping, unless Manifold already converged "
                         f"(default: {DEFAULT_GRACE_MINUTES})")
    ap.add_argument("--converge-high", type=float, default=None,
                    help="override the config's extreme_converge_high")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be reaped, change nothing")
    ap.add_argument("--telegram", action="store_true",
                    help="send a digest of what was reaped")
    args = ap.parse_args(argv)

    configs = args.config or [DEFAULT_CONFIG]
    lookup = Lookup()
    state = load_state()

    all_reaped: List[str] = []
    for config_path in configs:
        all_reaped.extend(reap(config_path, args, lookup, state))

    if not args.dry_run:
        save_state(state)

    if args.telegram and all_reaped:
        header = f"🧹 Reaped {len(all_reaped)} market pair(s)"
        send_telegram(header + "\n\n" + "\n".join(f"• {line}" for line in all_reaped))

    print(f"\n# Done — {len(all_reaped)} pairs reaped"
          + (" (dry run, nothing written)" if args.dry_run else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
