#!/usr/bin/env python3
"""Telegram control bot — connect and manage market pairs from a phone.

Built for running the bot suite while travelling, when editing a Python file or
hand-merging YAML on a laptop isn't practical. All the real work lives in
make_configs.py and reap_configs.py; this is a thin command surface over them, so
everything it does can also be done (and debugged) from a shell.

    /mlb                    today's MLB slate           /pairs      list active pairs
    /nba 7/30               a specific date             /drop 3     remove one
    /connect <k> <m>        one-off multi-choice event  /reap       archive finished
    /sports [nfl epl]       every ManifoldSports game   --overwrite update existing pairs
    /confirm                write the staged pairs      /status     config summary
    /fix 2 KXTICKER         pin a bad match, re-run     /restart    restart the trader
    /rewrite kimi = Andrea Kimi Antonelli               /help

Nothing is written to a config until you /confirm. Ambiguous and conflicting
matches are never staged for writing at all — see the matching notes in CLAUDE.md.

SETUP — this bot needs its OWN Telegram token. Telegram allows only one
getUpdates consumer per token, so sharing TELEGRAM_BOT_TOKEN with
manifold_user_alerts.py (or any *_watchlist_alerts.py) means one of them gets
HTTP 409 and stops receiving commands. Create a second bot with @BotFather and
set CONTROL_TELEGRAM_BOT_TOKEN. Sending alerts never conflicts — only receiving.

    CONTROL_TELEGRAM_BOT_TOKEN   dedicated bot token (falls back to TELEGRAM_BOT_TOKEN)
    TELEGRAM_CHAT_IDS            comma-separated allowlist of chat ids that may command
    CONTROL_CONFIG               config to manage (default: config_websocket.yaml)

/restart applies config changes (the trader reads its config only at startup),
so pick the mode matching how the trader is supervised:

    systemd (default)
      CONTROL_SERVICE_UNIT       e.g. manifold-bot.service
      CONTROL_SYSTEMCTL_SCOPE    user (default) | system

    tmux / anything else
      CONTROL_RESTART_MODE=command
      CONTROL_RESTART_COMMAND    e.g. pkill -f websocket_trading_bot.py
                                 (works when the trader runs inside a respawn
                                 loop: `while true; do python ...; sleep 3; done`)

    CONTROL_PROCESS_PATTERN      pgrep pattern used to confirm the trader is
                                 alive (default: websocket_trading_bot.py)

Run: python control_bot.py
"""
import io
import json
import os
import re
import shlex
import subprocess
import sys
import time
import traceback
from contextlib import redirect_stdout
from datetime import date
from typing import Dict, List, Optional, Tuple

import requests
import yaml
from dotenv import load_dotenv

import make_configs as mc
import make_manifoldsports_configs as ms
import reap_configs

load_dotenv()

TOKEN = os.getenv("CONTROL_TELEGRAM_BOT_TOKEN") or os.getenv("TELEGRAM_BOT_TOKEN")
_raw_ids = os.getenv("TELEGRAM_CHAT_IDS") or os.getenv("TELEGRAM_CHAT_ID") or ""
AUTHORIZED_CHAT_IDS = {c.strip() for c in _raw_ids.split(",") if c.strip()}
DEFAULT_CONFIG = os.getenv("CONTROL_CONFIG", "config_websocket.yaml")

# How /restart applies a config change. The trader reads its config ONCE at
# startup (bot_core.load_config), so a restart is the only way a config edit
# takes effect — which makes this the difference between a working phone
# workflow and one that silently writes files nobody reads.
#   systemd  — systemctl <scope> restart CONTROL_SERVICE_UNIT
#   command  — run CONTROL_RESTART_COMMAND (for tmux/pkill/docker setups)
RESTART_MODE = (os.getenv("CONTROL_RESTART_MODE") or "systemd").strip().lower()
SERVICE_UNIT = os.getenv("CONTROL_SERVICE_UNIT", "").strip()
SYSTEMCTL_SCOPE = (os.getenv("CONTROL_SYSTEMCTL_SCOPE") or "user").strip().lower()
# Never sourced from a Telegram message — operator-set env only, and run without
# a shell (shlex.split + no shell=True), so there is no injection path.
RESTART_COMMAND = os.getenv("CONTROL_RESTART_COMMAND", "").strip()
# Used by pgrep to tell whether the trader is alive, for /status and to verify a
# restart actually brought it back.
PROCESS_PATTERN = os.getenv("CONTROL_PROCESS_PATTERN", "websocket_trading_bot.py").strip()
RESTART_SETTLE_SEC = float(os.getenv("CONTROL_RESTART_SETTLE_SEC", "6"))

STATE_FILE = "control_bot_state.json"
POLL_TIMEOUT = 30          # long-poll seconds
TELEGRAM_MAX_CHARS = 4000  # hard limit is 4096; leave room for chunk markers

# Sport shortcut command -> (make_configs SPORTS key, human label)
SPORT_COMMANDS = {
    "mlb": "mlb", "nba": "nba", "nhl": "nhl", "nfl": "nfl",
    "epl": "premier_league", "ncaa": "ncaa_basketball",
    "atp": "tennis_atp", "wta": "tennis_wta",
}


# ── Telegram plumbing ─────────────────────────────────────────────────────────

def send(chat_id: str, text: str) -> None:
    """Send plain text, chunked.

    No parse_mode on purpose: market names and answer texts are arbitrary
    user-generated strings that routinely break Telegram's Markdown parser, and
    an unparseable alert is a *dropped* alert.
    """
    if not text.strip():
        text = "(no output)"
    chunks: List[str] = []
    remaining = text
    while len(remaining) > TELEGRAM_MAX_CHARS:
        cut = remaining.rfind("\n", 0, TELEGRAM_MAX_CHARS)
        if cut <= 0:
            cut = TELEGRAM_MAX_CHARS
        chunks.append(remaining[:cut])
        remaining = remaining[cut:].lstrip("\n")
    chunks.append(remaining)

    for i, chunk in enumerate(chunks):
        prefix = f"({i + 1}/{len(chunks)})\n" if len(chunks) > 1 else ""
        try:
            resp = requests.post(
                f"https://api.telegram.org/bot{TOKEN}/sendMessage",
                json={"chat_id": chat_id, "text": prefix + chunk,
                      "disable_web_page_preview": True},
                timeout=15,
            )
            if not resp.ok:
                print(f"[send] HTTP {resp.status_code}: {resp.text[:200]}")
        except requests.RequestException as e:
            print(f"[send] failed: {e}")


_warned_conflict = False


def get_updates(offset: Optional[int]) -> List[dict]:
    """Long-poll for updates. Explains a 409 once, then backs off quietly."""
    global _warned_conflict
    params = {"timeout": POLL_TIMEOUT}
    if offset is not None:
        params["offset"] = offset
    try:
        resp = requests.get(f"https://api.telegram.org/bot{TOKEN}/getUpdates",
                            params=params, timeout=POLL_TIMEOUT + 15)
        if resp.status_code == 409:
            if not _warned_conflict:
                print("[poll] HTTP 409: another process is already consuming "
                      "getUpdates for this token. Give this bot its own token via "
                      "CONTROL_TELEGRAM_BOT_TOKEN (see the module docstring). "
                      "Backing off 15s per poll.")
                _warned_conflict = True
            time.sleep(15)
            return []
        if not resp.ok:
            print(f"[poll] HTTP {resp.status_code}: {resp.text[:200]}")
            time.sleep(5)
            return []
        _warned_conflict = False
        return (resp.json() or {}).get("result", []) or []
    except requests.RequestException as e:
        print(f"[poll] {e}")
        time.sleep(5)
        return []


# ── State ─────────────────────────────────────────────────────────────────────

def load_state() -> dict:
    if os.path.exists(STATE_FILE):
        try:
            with open(STATE_FILE, "r", encoding="utf-8") as f:
                return json.load(f) or {}
        except (OSError, json.JSONDecodeError):
            print(f"[state] {STATE_FILE} unreadable — starting fresh")
    return {}


def save_state(state: dict) -> None:
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, STATE_FILE)


STATE = load_state()
# Per-chat working data: staged pairs awaiting /confirm, the last /connect request
# (so /fix can re-run it), and the last /pairs listing (so /drop numbers are stable).
SESSION: Dict[str, dict] = {}


def session(chat_id: str) -> dict:
    return SESSION.setdefault(chat_id, {"config": DEFAULT_CONFIG})


# ── Helpers ───────────────────────────────────────────────────────────────────

def capture(fn, *args, **kwargs) -> Tuple[object, str]:
    """Run fn, returning (result, printed_output).

    The generators report misses and confidence by printing; capturing lets the
    same diagnostics reach Telegram without touching them.
    """
    buf = io.StringIO()
    with redirect_stdout(buf):
        result = fn(*args, **kwargs)
    return result, buf.getvalue()


def diagnostic_lines(output: str) -> List[str]:
    """Keep the interesting lines from a generator's printed output."""
    keep = []
    for line in output.splitlines():
        s = line.strip()
        if not s:
            continue
        if "Available options" in s:  # long and repeated per miss
            continue
        if any(t in s for t in ("⚠", "REAP", "hold ", "No Kalshi match",
                                "Could not", "no match", "ambiguous", "AMBIGUOUS",
                                "Matched", "answers from", "CONFLICT", "Withholding",
                                "already in", "✓", "no longer on")):
            keep.append(s.lstrip("# ").rstrip())
    return keep


def format_specs(specs: List[dict], limit: int = 40) -> str:
    lines = []
    for i, spec in enumerate(specs[:limit], 1):
        source = spec.get("kalshi_ticker") or spec.get("polymarket_token_id") or "?"
        lines.append(f"{i}. {spec.get('name', '?')}\n    {source}\n    "
                     f"{spec.get('manifold_slug')} / {spec.get('manifold_answer_id')}")
    if len(specs) > limit:
        lines.append(f"... and {len(specs) - limit} more")
    return "\n".join(lines)


def stage(chat_id: str, specs: List[dict], label: str, report: List[str],
          overrides: dict = None, overwrite: bool = False,
          listing: Optional[str] = None) -> str:
    """Withhold conflicts, stash the rest for /confirm, and build the reply.

    overwrite: /confirm updates pairs already in the config (merging these
    specs' keys over them) instead of skipping them — how a parameter is changed
    on pairs that are already connected.
    listing:   replaces the default per-pair listing (/sports lists games).
    """
    sess = session(chat_id)
    if overrides:
        mc.apply_overrides(specs, overrides)
        report.append("Per-pair overrides: "
                      + ", ".join(f"{k}={v}" for k, v in sorted(overrides.items())))
    conflicted, conflict_report = capture(mc.report_conflicts, specs)
    for line in diagnostic_lines(conflict_report):
        report.append(line)
    if conflicted:
        specs = [s for i, s in enumerate(specs) if i not in conflicted]
        report.append(f"Withheld {len(conflicted)} conflicting pair(s).")

    # Say up front what /confirm will do against the config as it is now —
    # otherwise re-running a connect to change a parameter reports "N ready"
    # and then writes nothing, because every pair is already there.
    try:
        existing = read_config_pairs(sess["config"])
    except (OSError, yaml.YAMLError) as e:
        sess.pop("staged", None)
        return (f"{label}: {sess['config']} is unreadable ({type(e).__name__}) — "
                f"nothing staged. /undo restores the last backup.")
    new, updates, unchanged, skipped = mc.plan_pair_write(specs, existing, overwrite)
    counts = f"{len(new)} new"
    if overwrite:
        counts += f", {len(updates)} to update"
        if unchanged:
            counts += f", {len(unchanged)} already up to date"
    elif skipped:
        counts += f", {len(skipped)} already in config (skipped — add --overwrite to update them)"
    if updates:
        report.append("--overwrite will change:")
        report += [f"  {line}" for line in mc.describe_updates(existing, updates)]

    # A pending drop and a pending add would both answer to /confirm, and
    # cmd_confirm runs the drop first — so a /drop preview followed by a
    # connect used to make /confirm remove pairs instead of adding these.
    sess.pop("staged_drop", None)
    sess["staged"] = specs
    sess["staged_label"] = label
    sess["staged_overwrite"] = overwrite
    body = "\n".join(report)
    if not specs:
        return f"{label}: nothing safe to add.\n\n{body}"
    if not new and not updates:
        sess.pop("staged", None)
        return f"{label}: nothing to write ({counts}).\n\n{body}"
    return (f"{label}: {len(specs)} pair(s) — {counts}.\n\n"
            f"{listing or format_specs(specs)}\n\n"
            f"{body}\n\n/confirm to write to {sess['config']}, /cancel to discard.")


FILTER_FLAGS = {"min-prob", "max-prob", "date", "contains"}
# Bare on/off switches that change how /confirm writes rather than what is
# written into each pair. They ride in the filters dict (never into a spec) and
# never swallow the next token unless it is a yes/no, so
# `/nfl --overwrite week-7-nfl-...` keeps its slug.
SWITCH_FLAGS = {"overwrite"}


def parse_prob(text: str) -> float:
    """'0.5%' -> 0.005, '5' -> 0.05, '0.005' -> 0.005.

    A bare number >1 can only mean a percent; <=1 is read as a fraction. The
    interpreted value is echoed back so the reading is never in doubt.
    """
    s = str(text).strip()
    is_pct = s.endswith("%")
    value = float(s.rstrip("%").strip())
    if is_pct:
        return value / 100.0
    return value / 100.0 if value > 1 else value


def parse_flags(args: List[str]) -> Tuple[List[str], dict, dict, List[str]]:
    """Split Telegram args into (positional, filters, per-pair overrides, errors)."""
    positional: List[str] = []
    filters: dict = {}
    overrides: dict = {}
    errors: List[str] = []

    i = 0
    while i < len(args):
        token = args[i]
        if not token.startswith("--"):
            positional.append(token)
            i += 1
            continue

        name = token[2:].lower()
        if name in SWITCH_FLAGS:
            value = None
            if i + 1 < len(args) and args[i + 1].lower() in (mc.BOOL_TRUE | mc.BOOL_FALSE):
                value = args[i + 1]
                i += 1
            i += 1
            filters[name] = mc.parse_bool_override(value)
            continue

        value = None
        if i + 1 < len(args) and not args[i + 1].startswith("--"):
            value = args[i + 1]
            i += 1
        i += 1

        if name in FILTER_FLAGS:
            if value is None:
                errors.append(f"--{name} needs a value")
            else:
                filters[name] = value
            continue

        key = mc.resolve_override(name)
        if key is None:
            errors.append(f"unknown flag --{name}")
        elif key in mc.BOOL_OVERRIDES:
            # --reverse / --amm work as bare flags or with a value (--amm false).
            parsed = mc.parse_bool_override(value)
            if parsed is None:
                errors.append(f"--{name} needs true/false, got {value!r}")
            else:
                overrides[key] = parsed
        elif key in getattr(mc, "CHOICE_OVERRIDES", {}):
            # A fixed vocabulary, not a number: --tie-resolves no|half|ignore.
            choices = mc.CHOICE_OVERRIDES[key]
            v = (value or "").strip().lower()
            if v in choices:
                overrides[key] = v
            else:
                errors.append(f"--{name} needs one of {'/'.join(sorted(choices))}, got {value!r}")
        elif value is None:
            errors.append(f"--{name} needs a value")
        else:
            try:
                overrides[key] = float(value) if "." in value else int(value)
            except ValueError:
                errors.append(f"--{name} needs a number, got {value!r}")

    return positional, filters, overrides, errors


def resolve_filters(filters: dict) -> Tuple[dict, List[str], List[str]]:
    """Turn raw filter flags into kwargs for filter_answers(). (kwargs, notes, errors)."""
    kwargs: dict = {}
    notes: List[str] = []
    errors: List[str] = []
    for name, raw in filters.items():
        try:
            if name in ("min-prob", "max-prob"):
                value = parse_prob(raw)
                kwargs["min_prob" if name == "min-prob" else "max_prob"] = value
                notes.append(f"{name} = {value:.4%}")
            elif name == "date":
                kwargs["date_filter"] = mc.resolve_date_arg(raw)
                notes.append(f"date = {kwargs['date_filter']}")
            elif name == "contains":
                kwargs["contains"] = raw
                notes.append(f"contains = {raw!r}")
        except ValueError as e:
            errors.append(str(e))
    return kwargs, notes, errors


def read_config_pairs(path: str) -> List[dict]:
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f) or {}
    return [p for p in (cfg.get("market_pairs") or []) if isinstance(p, dict)]


# ── Command handlers ──────────────────────────────────────────────────────────

def cmd_help(chat_id: str, args: List[str]) -> str:
    sess = session(chat_id)
    return (
        "Market pair control\n\n"
        "CONNECT\n"
        "  /mlb [date]  /nba  /nhl  /nfl  /epl  /ncaa  /atp  /wta\n"
        "     date: today (default), tomorrow, 7/30, 2026-07-30\n"
        "  /fixtures <manifold> <KALSHI_SERIES>\n"
        "     matchday markets ('<A> beat <B>'), wired to A's leg\n"
        "     e.g. /fixtures uefa-champions-league-fixtures-jan-... KXUCLGAME\n"
        "  /connect <kalshi> <manifold>\n"
        "     one-off multi-choice events (F1, Fed, awards)\n"
        "  /add <manifold> <answer_id> <TICKER> [name]\n"
        "     one pair by hand; both legs verified first\n"
        "  /sports [nfl epl nba nhl f1 ...] [--date today]\n"
        "     every open ManifoldSports game Kalshi lists; two-way\n"
        "     games (NFL/NBA/NHL) get one leg, soccer all three,\n"
        "     F1 every driver in race winner / pole / podium\n\n"
        "DISCARD A BAD CONNECTION\n"
        "  /drop <manifold-slug>   every pair for that market\n"
        "  /drop <KALSHI-EVENT>    every leg under that event\n"
        "  /drop <n>               one pair from the last /pairs listing\n"
        "     the first two preview first and wait for /confirm\n\n"
        "FLAGS (any connect command)\n"
        "  --min-prob 0.5%     skip answers below this probability\n"
        "  --max-prob 99%      skip answers above it\n"
        "  --date / --contains further filter the answers\n"
        "  --max-position 500  --max-bet 50   --min-edge 0.03\n"
        "  --max-edge 0.15     --clearing-edge 0   --max-spread 0.1\n"
        "  --cooldown 2        --extreme-mult 20  --reverse\n"
        "  --amm               size to move the price to fair, then stop\n"
        "                      (bare = on; --amm false to opt a pair out)\n"
        "  --amm-fraction 0.9  --liquidity 5000   (AMM tuning, rarely needed)\n"
        "  --settle-slam       act on Kalshi's stated result (determined frame)\n"
        "  --tie-resolves no   a 0.50 tie settlement means: no | half | ignore\n"
        "  --settle-bet 200    mana per slam (default max-bet x extreme-mult)\n"
        "     these ride along as per-pair overrides\n"
        "  --overwrite         pairs already in the config get these\n"
        "                      overrides instead of being skipped\n"
        "     e.g. /sports nfl --min-edge 0.0001 --overwrite\n"
        "     for ALL pairs, set enable_amm in the config's bot: block instead\n\n"
        "LOOK UP\n"
        "  /answers <manifold> [search]   answer ids + probabilities\n"
        "  /kalshi <ticker> [search]      Kalshi tickers + prices\n\n"
        "REVIEW\n"
        "  /confirm   /cancel\n"
        "  /fix <n> <TICKER>                pin entry n, re-run\n"
        "  /rewrite <text> = <replacement>  durable name alias, re-run\n\n"
        "MANAGE\n"
        "  /pairs   /drop <n>   /reap [dry]   /status\n"
        "  /bot [key value]    read or set a bot: parameter\n"
        "  /dryrun on|off      stop/resume real orders\n"
        "  /log [lines]        tail the trader log\n"
        "  /undo               restore the config from its .bak\n"
        "  /config <path>      switch which config is managed\n"
        "  /restart            restart the trader\n\n"
        f"Config: {sess['config']}\n"
        "Nothing is written until /confirm."
    )


def cmd_sport(chat_id: str, args: List[str], sport_key: str, label: str) -> str:
    sess = session(chat_id)
    dated = mc.SPORTS[sport_key]["parser"] in mc.DATED_PARSERS

    positional, filters, overrides, errors = parse_flags(args)
    fkwargs, notes, ferrors = resolve_filters(filters)
    errors += ferrors
    if errors:
        return "Bad flags:\n" + "\n".join(f"  {e}" for e in errors) + "\n\n/help for usage."

    # A positional is either a date or a Manifold slug/URL. The built-in
    # SPORTS[...] manifold_url is a fixed market, but the league mega-markets are
    # WEEKLY ("(Week 1) NFL 2026 Mega-Market", "(Week 2) ..."), so the built-in
    # goes stale by construction and /nfl had no way to point somewhere else.
    # Anything that doesn't parse as a date is taken as the slug.
    when = fkwargs.pop("date_filter", None)
    slug_override = None
    for tok in positional:
        if when is None:
            try:
                when = mc.resolve_date_arg(tok)
                continue
            except ValueError:
                pass
        if slug_override is None:
            slug_override = mc.extract_manifold_slug(tok)
        else:
            return f"Don't know what to do with '{tok}'. Usage: /{sport_key} [date] [manifold-slug]"
    if when is None and dated and slug_override is None:
        when = date.today()

    slug = slug_override or mc.extract_manifold_slug(mc.SPORTS[sport_key]["manifold_url"])
    report = list(notes)
    if slug_override:
        report.append(f"Manifold market: {slug} (override)")
        if when is None:
            report.append("No date given — taking every unresolved answer.")

    try:
        raw, out = capture(mc.manifold_answers_block, slug,
                           date_filter=when if dated else None, **fkwargs)
    except requests.HTTPError as e:
        # Slugs are typed by hand now that a market can be overridden, so a typo
        # is routine. Say which slug failed instead of surfacing a raw traceback.
        if e.response is not None and e.response.status_code == 404:
            return (f"No Manifold market with slug '{slug}'.\n"
                    + ("Check the URL — the slug is the last path segment."
                       if slug_override else
                       f"The built-in slug for /{sport_key} is stale; pass one: "
                       f"/{sport_key} <manifold-slug>"))
        return f"Manifold lookup failed for '{slug}': {e}"
    report += diagnostic_lines(out)
    if not raw.strip():
        return (f"{label}: no unresolved answers"
                + (f" for {when.month}/{when.day}" if when else "") + ".")

    year = (when or date.today()).strftime("%y")
    game_date = f"{when.month}/{when.day}" if (when and not dated) else None
    # Pass the slug through. generate_job_specs otherwise falls back to
    # SPORTS[sport]["manifold_url"] for the manifold_slug it stamps on every
    # spec — so an overridden market produced pairs whose ANSWER IDS came from
    # the requested market but whose SLUG named the built-in one. That pairing
    # does not merely look wrong, it cannot work: the bot resolves the slug to a
    # contract and then looks for answer ids that are not in it.
    specs, out = capture(mc.generate_job_specs, raw, sport=sport_key,
                         year_two_digits=year, game_date=game_date,
                         manifold_slug_override=slug)
    report += diagnostic_lines(out)

    if not specs:
        return f"{label}: nothing matched.\n\n" + "\n".join(report)
    title = label + (f" {when.month}/{when.day}" if when else "")
    return stage(chat_id, specs, title, report, overrides,
                 overwrite=bool(filters.get("overwrite")))


def cmd_sports(chat_id: str, args: List[str]) -> str:
    """Connect every open @ManifoldSports game to its Kalshi game market.

    /sports                      every mapped league
    /sports nfl epl f1           just these (ms.ALL_LEAGUES)
    /sports --date tomorrow      one day's games
    /sports nfl --min-edge 0.0001 --overwrite   change a parameter on them

    Matching and the leg rule live in make_manifoldsports_configs.py: two-way
    games (NFL/NBA/NHL/MLB) are sum-to-one, so only the first-named team's leg
    is connected; soccer (home/away/draw) gets all three.
    """
    sess = session(chat_id)
    positional, filters, overrides, errors = parse_flags(args)
    leagues, unknown = ms.parse_leagues(positional)
    if unknown:
        errors.append(f"unknown league(s) {', '.join(unknown)} — known: {', '.join(ms.ALL_LEAGUES)}")
    for name in ("min-prob", "max-prob"):
        if name in filters:
            errors.append(f"--{name} doesn't apply to /sports (it connects whole games)")
    game_date = None
    if "date" in filters:
        try:
            game_date = mc.resolve_date_arg(filters["date"])
        except ValueError as e:
            errors.append(str(e))
    if errors:
        return "Bad arguments:\n" + "\n".join(f"  {e}" for e in errors) + "\n\n/help for usage."

    diags: dict = {}
    try:
        specs, _out = capture(ms.build_market_pairs, verbose=False, leagues=leagues,
                              game_date=game_date, diagnostics=diags)
    except requests.RequestException as e:
        return f"Lookup failed: {e}"

    # Filter whole games on the question, never single legs: `--contains draw`
    # matching only the Draw leg would leave a soccer game looking two-way.
    contains = (filters.get("contains") or "").lower()
    if contains:
        specs = [s for s in specs if contains in s["name"].rsplit(" - ", 1)[0].lower()]

    try:
        existing = read_config_pairs(sess["config"])
    except (OSError, yaml.YAMLError) as e:
        return f"{sess['config']} is unreadable ({type(e).__name__}). /undo restores the last backup."
    specs, withheld = ms.split_connected_two_way(specs, existing, diags.get("two_way"))

    scope = ", ".join(sorted(leagues)) if leagues else "all leagues"
    if game_date:
        scope += f", {game_date.month}/{game_date.day}"
    report = [f"Matched {diags.get('matched', 0)}/{diags.get('markets', 0)} "
              f"ManifoldSports markets ({scope})."]
    if diags.get("not_listed"):
        report.append(f"{len(diags['not_listed'])} not on Kalshi yet (re-run later): "
                      + ", ".join(diags["not_listed"][:8])
                      + (" ..." if len(diags["not_listed"]) > 8 else ""))
    for p in withheld:
        report.append(f"other leg already connected, skipped: {p['name']}")
    if diags.get("unmatched"):
        # Usually a driver Kalshi doesn't list (a reserve, or "Another driver"
        # is already skipped). Grouped by name: the same driver is missing from
        # every session of a weekend.
        counts: Dict[str, int] = {}
        for row in diags["unmatched"]:
            for name in row.split(": ", 1)[-1].split(", "):
                counts[name] = counts.get(name, 0) + 1
        report.append("No Kalshi leg, not connected: "
                      + ", ".join(f"{n} (x{c})" if c > 1 else n for n, c in sorted(counts.items())))
    if diags.get("inferred"):
        report.append("Codes matched by team name (worth adding to KALSHI_CODE_ALIASES):")
        report += [f"  {n}" for n in diags["inferred"]]
    if diags.get("misses"):
        report.append("Not connected:")
        report += [f"  {m}" for m in diags["misses"]]

    if not specs:
        return "ManifoldSports: nothing matched.\n\n" + "\n".join(report)
    return stage(chat_id, specs, "ManifoldSports", report, overrides,
                 overwrite=bool(filters.get("overwrite")), listing=format_games(specs))


def format_games(specs: List[dict]) -> str:
    """One line per market, grouped by league: 'Everton vs Chelsea, Oct 17 — EVE/CHE/Draw',
    '2026 Mexico City Grand Prix: podium — 22 drivers'."""
    games: Dict[str, List[dict]] = {}
    for s in specs:
        games.setdefault(s["manifold_slug"], []).append(s)
    by_league: Dict[str, List[str]] = {}
    for legs in games.values():
        league = ms.series_league(legs[0]["kalshi_ticker"].split("-", 1)[0]) or "?"
        title = legs[0]["name"].rsplit(" - ", 1)[0]
        if league in ms.RACE_LEAGUES:
            labels = f"{len(legs)} driver{'s' if len(legs) != 1 else ''}"
        else:
            labels = "/".join(leg["name"].rsplit(" - ", 1)[-1] for leg in legs)
        by_league.setdefault(league, []).append(f"  {title} — {labels}")
    out = []
    for league, rows in by_league.items():
        unit = "market" if league in ms.RACE_LEAGUES else "game"
        out.append(f"{league.upper()} ({len(rows)} {unit}{'s' if len(rows) != 1 else ''})")
        out += rows
    return "\n".join(out)


def cmd_connect(chat_id: str, args: List[str]) -> str:
    positional, filters, overrides, errors = parse_flags(args)
    if errors:
        return "Bad flags:\n" + "\n".join(f"  {e}" for e in errors)
    if len(positional) < 2:
        return ("Usage: /connect <kalshi-url-or-ticker> <manifold-url-or-slug> [flags]\n"
                "e.g. /connect KXNEWPOPE-70 who-will-be-the-next-pope --min-prob 0.5%")
    sess = session(chat_id)
    sess["last_connect"] = {"kalshi": positional[0], "manifold": positional[1],
                            "filters": filters, "overrides": overrides}
    return run_connect(chat_id)


def run_connect(chat_id: str) -> str:
    """Run (or re-run) the staged /connect request."""
    sess = session(chat_id)
    req = sess.get("last_connect")
    if not req:
        return "No /connect to run. Use /connect <kalshi> <manifold> first."

    kalshi, manifold = req["kalshi"], req["manifold"]
    overrides = req.get("overrides") or {}
    fkwargs, notes, ferrors = resolve_filters(req.get("filters") or {})
    if ferrors:
        return "Bad filters:\n" + "\n".join(f"  {e}" for e in ferrors)

    slug = mc.extract_manifold_slug(manifold)
    report: List[str] = list(notes)

    raw, out = capture(mc.manifold_answers_block, slug, **fkwargs)
    report += diagnostic_lines(out)
    if not raw.strip():
        return f"No unresolved answers on Manifold market '{slug}'."

    diags: dict = {}
    try:
        specs, out = capture(mc.generate_generic_configs,
                             kalshi_url_or_ticker=kalshi, manifold_slug=slug,
                             raw_manifold_inputs=raw, diagnostics=diags)
    except requests.RequestException as e:
        return f"Lookup failed: {e}"
    report += diagnostic_lines(out)

    sess["last_diags"] = diags
    unresolved = diags.get("ambiguous", []) + diags.get("misses", [])
    if unresolved:
        lines = ["", "Unresolved (numbered for /fix):"]
        for i, item in enumerate(unresolved, 1):
            lines.append(f"  {i}. {item['text']} — {item['detail']}")
        lines.append("  /fix <n> <KALSHI_TICKER>  or  /rewrite <text> = <replacement>")
        report += lines

    if not specs:
        return f"{slug}: nothing matched.\n\n" + "\n".join(report)
    return stage(chat_id, specs, slug, report, overrides,
                 overwrite=bool((req.get("filters") or {}).get("overwrite")))


def cmd_answers_lookup(chat_id: str, args: List[str]) -> str:
    """List a Manifold market's answer ids — the other half of what /add needs."""
    positional, filters, _o, errors = parse_flags(args)
    fkwargs, notes, ferrors = resolve_filters(filters)
    errors += ferrors
    if errors:
        return "Bad flags:\n" + "\n".join(f"  {e}" for e in errors)
    if not positional:
        return ("Usage: /answers <manifold-url-or-slug> [search] [--min-prob 1%] [--date today]\n"
                "e.g. /answers major-league-baseball-live --date today")

    manifold = positional[0]
    slug = mc.extract_manifold_slug(manifold)
    if len(positional) > 1 and "contains" not in fkwargs:
        fkwargs["contains"] = " ".join(positional[1:])

    try:
        answers = mc.fetch_manifold_answers(slug)
    except requests.RequestException as e:
        return f"Manifold lookup failed: {e}"
    if not answers:
        return f"No answers on '{slug}' (binary market? then /add needs no answer id)."

    kept, dropped = mc.filter_answers(answers, **fkwargs)
    header = f"{slug} — {len(kept)} answer(s)"
    if dropped:
        header += f" (skipped {mc.describe_drops(dropped)})"
    if not kept:
        return header
    # Text collapsed outside the f-string: a backslash in an f-string expression
    # is a SyntaxError before Python 3.12, and the bot host runs 3.11.
    rows = []
    for a in kept[:50]:
        text = re.sub(r"\s+", " ", (a.get("text") or "")).strip()
        rows.append(f"{mc.answer_prob(a):6.1%}  {a.get('id')}  {text[:70]}")
    body = "\n".join(rows)
    more = f"\n... and {len(kept) - 50} more" if len(kept) > 50 else ""
    return (header + "\n\n" + body + more
            + "\n\nUse an id with /add <slug> <answer_id> <KALSHI_TICKER>.")


def cmd_fixtures(chat_id: str, args: List[str]) -> str:
    """Connect a whole matchday market ('<A> beat <B>') to a Kalshi game series."""
    positional, filters, overrides, errors = parse_flags(args)
    fkwargs, notes, ferrors = resolve_filters(filters)
    errors += ferrors
    if errors:
        return "Bad flags:\n" + "\n".join(f"  {e}" for e in errors)
    if len(positional) < 2:
        return ("Usage: /fixtures <manifold-url-or-slug> <KALSHI_SERIES> [flags]\n"
                "e.g. /fixtures uefa-champions-league-fixtures-jan-gLCgsERuh2 KXUCLGAME\n"
                "     /fixtures english-premier-league-matchweek-23 KXEPLGAME --min-prob 1%\n\n"
                "Each answer must read '<TeamA> beat <TeamB> (when)'; the pair is wired "
                "to TeamA's Kalshi leg.")

    manifold, series = positional[0], positional[1]
    slug = mc.extract_manifold_slug(manifold)
    report: List[str] = list(notes)

    try:
        answers = mc.fetch_manifold_answers(slug)
    except requests.RequestException as e:
        return f"Manifold lookup failed: {e}"
    if not answers:
        return f"No answers on Manifold market '{slug}'."

    kept, dropped = mc.filter_answers(answers, **fkwargs)
    if dropped:
        report.append(f"Filtered out: {mc.describe_drops(dropped)}")
    if not kept:
        return f"{slug}: every answer was filtered out.\n\n" + "\n".join(report)

    diags: dict = {}
    try:
        specs, out = capture(mc.generate_fixture_configs, slug, series,
                             answers=kept, diagnostics=diags)
    except requests.RequestException as e:
        return f"Kalshi lookup failed: {e}"
    report += diagnostic_lines(out)

    sess = session(chat_id)
    sess["last_diags"] = diags
    unresolved = diags.get("ambiguous", []) + diags.get("misses", [])
    if unresolved:
        report.append("")
        report.append("Unresolved (numbered for /fix):")
        for i, item in enumerate(unresolved, 1):
            report.append(f"  {i}. {item['text']} — {item['detail']}")
        report.append("  /rewrite <manifold name> = <kalshi name>  usually fixes these")

    if not specs:
        return f"{slug}: nothing matched.\n\n" + "\n".join(report)
    return stage(chat_id, specs, f"{slug} ({series})", report, overrides,
                 overwrite=bool(filters.get("overwrite")))


def cmd_add(chat_id: str, args: List[str]) -> str:
    """Add one pair by hand: market + answer id + Kalshi ticker."""
    positional, filters, overrides, errors = parse_flags(args)
    if errors:
        return "Bad flags:\n" + "\n".join(f"  {e}" for e in errors)
    if len(positional) < 2:
        return ("Usage: /add <manifold-url-or-slug> <answer_id> <KALSHI_TICKER> [name]\n"
                "Binary markets take no answer id:\n"
                "  /add <manifold-slug> <KALSHI_TICKER>\n"
                "e.g. /add who-will-be-the-next-pope 5qpddI9pzz KXNEWPOPE-70-MZUP")

    # The Kalshi ticker is the argument that looks like one; that tells us
    # whether an answer id was supplied.
    if len(positional) >= 3:
        manifold, answer_id, ticker = positional[0], positional[1], positional[2]
        name = " ".join(positional[3:]) or None
    else:
        manifold, answer_id, ticker = positional[0], None, positional[1]
        name = None

    spec, problems = mc.build_manual_pair(manifold, answer_id, ticker, name)
    if problems:
        return "Could not add that pair:\n" + "\n".join(f"  • {p}" for p in problems)
    return stage(chat_id, [spec], "manual pair", ["Both legs verified against their APIs."],
                 overrides, overwrite=bool(filters.get("overwrite")))


def cmd_kalshi(chat_id: str, args: List[str]) -> str:
    """List a Kalshi event's or series' markets — the Kalshi find_manifold_answers."""
    positional, _f, _o, errors = parse_flags(args)
    if errors:
        return "Bad flags:\n" + "\n".join(f"  {e}" for e in errors)
    if not positional:
        return ("Usage: /kalshi <event-or-series-ticker-or-url> [search]\n"
                "e.g. /kalshi KXUCLGAME lyon\n"
                "     /kalshi KXNEWPOPE-70")
    ident = positional[0]
    search = " ".join(positional[1:]) or None
    try:
        markets, out = capture(mc.lookup_kalshi, ident, search)
    except requests.RequestException as e:
        return f"Kalshi lookup failed: {e}"
    if not markets:
        return (f"No active markets for {ident}"
                + (f" matching {search!r}" if search else "")
                + ".\n" + "\n".join(diagnostic_lines(out)))
    return (f"{ident}" + (f" ~ {search!r}" if search else "")
            + f" — {len(markets)} market(s)\n\n" + mc.format_kalshi_markets(markets)
            + "\n\nUse a ticker with /add <manifold> <answer_id> <TICKER>.")


def cmd_confirm(chat_id: str, args: List[str]) -> str:
    sess = session(chat_id)
    staged_drop = sess.get("staged_drop")
    if staged_drop:
        return _confirm_drop(chat_id, staged_drop)
    specs = sess.get("staged")
    if not specs:
        return "Nothing staged. Run /mlb, /connect, etc. first."
    overwrite = bool(sess.pop("staged_overwrite", False))
    added, out = capture(mc.append_market_pairs, specs, sess["config"], overwrite=overwrite)
    sess.pop("staged", None)
    # The whole write report: it is short, and with --overwrite the per-key
    # "old -> new" lines are the part worth reading.
    lines = [ln.strip().lstrip("# ").rstrip() for ln in out.splitlines() if ln.strip()]

    # The trader reads its config once at startup, so a write alone changes
    # nothing about what is currently being traded. Saying so here is the
    # difference between "added 14 games" and actually trading them.
    if added:
        ok, _ = restart_available()
        lines.append("")
        lines.append("NOT LIVE YET — the trader reads its config only at startup."
                     + ("\nSend /restart to apply." if ok
                        else "\nRestart it to apply (/restart is disabled — see /status)."))
    verb = "Wrote/updated" if overwrite else "Wrote"
    return f"{verb} {added} pair(s) in {sess['config']}.\n\n" + "\n".join(lines)


def cmd_cancel(chat_id: str, args: List[str]) -> str:
    sess = session(chat_id)
    dropped = sess.pop("staged_drop", None)
    if dropped:
        return f"Kept {len(dropped['keys'])} pair(s) — drop cancelled."
    n = len(sess.pop("staged", []) or [])
    sess.pop("staged_overwrite", None)
    return f"Discarded {n} staged pair(s)."


def cmd_fix(chat_id: str, args: List[str]) -> str:
    sess = session(chat_id)
    diags = sess.get("last_diags")
    if not diags:
        return "No recent /connect to fix. Run /connect first."
    if len(args) < 2:
        return "Usage: /fix <n> <KALSHI_TICKER>"
    try:
        idx = int(args[0])
    except ValueError:
        return f"'{args[0]}' is not a number. Usage: /fix <n> <KALSHI_TICKER>"

    unresolved = diags.get("ambiguous", []) + diags.get("misses", [])
    if not 1 <= idx <= len(unresolved):
        return f"Pick 1-{len(unresolved)} (from the last /connect report)."

    item = unresolved[idx - 1]
    ticker = args[1].strip().upper()
    mc.add_alias(item["text"], ticker, kind="pins")
    return (f"Pinned '{item['text']}' -> {ticker} in {mc.ALIASES_FILE}.\n"
            f"Re-running...\n\n" + run_connect(chat_id))


def cmd_rewrite(chat_id: str, args: List[str]) -> str:
    raw = " ".join(args)
    if "=" not in raw:
        return ("Usage: /rewrite <manifold text> = <text to match with>\n"
                "e.g. /rewrite kimi = Andrea Kimi Antonelli")
    left, right = raw.split("=", 1)
    left, right = left.strip(), right.strip()
    if not left or not right:
        return "Both sides of the = are required."
    mc.add_alias(left, right, kind="rewrites")
    msg = f"Rewrite saved: '{left}' -> '{right}' in {mc.ALIASES_FILE}."
    if session(chat_id).get("last_connect"):
        return msg + "\nRe-running...\n\n" + run_connect(chat_id)
    return msg


def cmd_pairs(chat_id: str, args: List[str]) -> str:
    sess = session(chat_id)
    path = args[0] if args else sess["config"]
    pairs = read_config_pairs(path)
    if not pairs:
        return f"{path}: no market pairs."
    # Snapshot the keys so /drop <n> can't act on a shifted list.
    sess["listing"] = {"path": path, "keys": [list(mc.pair_key(p)) for p in pairs]}
    return f"{path}: {len(pairs)} pair(s)\n\n{format_specs(pairs, limit=60)}"


def cmd_drop(chat_id: str, args: List[str]) -> str:
    """/drop <n> removes one pair from the last /pairs listing.
    /drop <slug|ticker|url> removes every pair for a whole market — the case
    that matters when a connection was wrong, because connecting a multi-choice
    market wires up every option at once and unpicking it one index at a time
    means re-running /pairs between each removal."""
    sess = session(chat_id)
    if not args:
        return ("Usage:\n"
                "  /drop <n>                  one pair from the last /pairs listing\n"
                "  /drop <manifold-slug>      every pair for that market\n"
                "  /drop <KALSHI-EVENT>       every leg under that event\n"
                "  (a Manifold or Kalshi URL works too)")

    if not args[0].lstrip("-").isdigit():
        return _drop_by_target(chat_id, args[0])

    listing = sess.get("listing")
    if not listing:
        return "Run /pairs first so the numbers are unambiguous."
    try:
        idx = int(args[0])
    except ValueError:
        return f"'{args[0]}' is not a number."

    keys = listing["keys"]
    if not 1 <= idx <= len(keys):
        return f"Pick 1-{len(keys)}."

    path = listing["path"]
    current = [list(mc.pair_key(p)) for p in read_config_pairs(path)]
    if current != keys:
        sess.pop("listing", None)
        return f"{path} changed since that listing — run /pairs again."

    removed, out = capture(mc.remove_market_pairs, {tuple(keys[idx - 1])}, path)
    sess.pop("listing", None)
    if not removed:
        return f"Nothing removed from {path}."
    return (f"Removed: {removed[0].get('name', '?')}\n"
            + "\n".join(diagnostic_lines(out)))


def _drop_by_target(chat_id: str, target: str) -> str:
    """Stage a whole-market removal for /confirm."""
    sess = session(chat_id)
    path = sess["config"]
    pairs = read_config_pairs(path)
    matched, how = mc.select_pairs(pairs, target)
    if not matched:
        markets = ("\n\nManifold markets in it:\n" + mc.market_summary(pairs)) if pairs else ""
        return (f"{how}.\n{path} has {len(pairs)} pair(s) — /pairs to list them.\n"
                "Give a manifold slug, a Kalshi ticker or event, or a URL."
                f"{markets}\n\nWrong config? /config <path> to switch.")

    # A pending add and a pending drop would both answer to /confirm.
    sess.pop("staged", None)
    sess.pop("staged_overwrite", None)
    sess["staged_drop"] = {"path": path, "how": how,
                           "keys": [list(mc.pair_key(p)) for p in matched]}
    return (f"Drop {len(matched)} of {len(pairs)} pair(s) from {path}\n"
            f"matched by {how}\n\n{format_specs(matched)}\n\n"
            f"/confirm to remove, /cancel to keep.")


def _confirm_drop(chat_id: str, staged: dict) -> str:
    sess = session(chat_id)
    path = staged["path"]
    keys = {tuple(k) for k in staged["keys"]}
    present = {mc.pair_key(p) for p in read_config_pairs(path)}
    sess.pop("staged_drop", None)
    sess.pop("listing", None)

    missing = keys - present
    if missing == keys:
        return f"{path} no longer has any of those pairs — nothing removed."
    if missing:
        return (f"{path} changed since that preview ({len(missing)} of "
                f"{len(keys)} pair(s) already gone) — run /drop again.")

    removed, out = capture(mc.remove_market_pairs, keys, path)
    if not removed:
        return f"Nothing removed from {path}."
    lines = diagnostic_lines(out)
    # Removing a pair from the file does not stop a running trader from trading
    # it — the config is read once at startup. For a WRONG connection that gap
    # is the whole point of dropping it, so say so loudly.
    ok, _ = restart_available()
    lines.append("")
    lines.append("STILL BEING TRADED — the running trader holds the old config."
                 + ("\nSend /restart to stop these pairs." if ok
                    else "\nRestart it to stop these pairs (/restart is disabled — see /status)."))
    return f"Removed {len(removed)} pair(s) from {path}.\n\n" + "\n".join(lines)


def cmd_reap(chat_id: str, args: List[str]) -> str:
    sess = session(chat_id)
    dry = bool(args) and args[0].lower() in ("dry", "dry-run", "--dry-run", "test")

    class Args:
        config = None
        archive = reap_configs.DEFAULT_ARCHIVE
        grace_minutes = float(os.getenv("CONTROL_REAP_GRACE", "60"))
        converge_high = None
        dry_run = dry
        telegram = False

    lookup = reap_configs.Lookup()
    state = reap_configs.load_state()
    reaped, out = capture(reap_configs.reap, sess["config"], Args(), lookup, state)
    if not dry:
        reap_configs.save_state(state)

    header = f"Reaped {len(reaped)} pair(s)" + (" (dry run)" if dry else "")
    body = "\n".join(f"• {line}" for line in reaped) if reaped else ""
    detail = "\n".join(diagnostic_lines(out))
    return "\n\n".join(x for x in (header, body, detail) if x)


def cmd_status(chat_id: str, args: List[str]) -> str:
    sess = session(chat_id)
    path = sess["config"]
    pairs = read_config_pairs(path)
    slugs = {p.get("manifold_slug") for p in pairs if p.get("manifold_slug")}
    kalshi = sum(1 for p in pairs if p.get("kalshi_ticker"))
    poly = sum(1 for p in pairs if p.get("polymarket_token_id"))

    lines = [
        f"Config: {path}",
        f"Pairs: {len(pairs)}  ({kalshi} kalshi, {poly} polymarket)",
        f"Manifold markets: {len(slugs)}",
        f"Staged: {len(sess.get('staged') or [])}",
    ]
    try:
        with open(path, "r", encoding="utf-8") as f:
            bot_cfg = (yaml.safe_load(f) or {}).get("bot") or {}
        lines.append(f"dry_run: {bot_cfg.get('dry_run')}  "
                     f"max_bet_size: {bot_cfg.get('max_bet_size')}  "
                     f"max_position: {bot_cfg.get('max_position')}")
    except (OSError, yaml.YAMLError):
        pass

    lines.append(f"Trader: {service_state()}")
    ok, detail = restart_available()
    lines.append(f"Restart: {detail}" if ok else "Restart: DISABLED — /restart for why")
    return "\n".join(lines)


def _systemctl(*argv: str) -> List[str]:
    cmd = ["systemctl"]
    if SYSTEMCTL_SCOPE == "user":
        cmd.append("--user")
    return cmd + list(argv)


def process_running() -> Optional[bool]:
    """Is the trader alive? None when pgrep is unavailable.

    pgrep exits 0 if something matched, 1 if not, >1 on error.
    """
    if not PROCESS_PATTERN:
        return None
    try:
        out = subprocess.run(["pgrep", "-f", PROCESS_PATTERN],
                             capture_output=True, text=True, timeout=10)
    except (subprocess.SubprocessError, FileNotFoundError):
        return None
    if out.returncode > 1:
        return None
    return out.returncode == 0


def restart_available() -> Tuple[bool, str]:
    """(usable?, explanation) for the configured restart mode."""
    if RESTART_MODE == "command":
        if not RESTART_COMMAND:
            return False, ("CONTROL_RESTART_MODE=command but CONTROL_RESTART_COMMAND "
                           "is empty. Set it to whatever restarts your trader, e.g. "
                           "'pkill -f websocket_trading_bot.py' when it runs inside a "
                           "tmux respawn loop.")
        return True, f"command: {RESTART_COMMAND}"
    if RESTART_MODE == "systemd":
        if not SERVICE_UNIT:
            return False, ("CONTROL_SERVICE_UNIT is unset. Either set it to the systemd "
                           "unit running the trader, or — if you run under tmux — set "
                           "CONTROL_RESTART_MODE=command and CONTROL_RESTART_COMMAND="
                           "'pkill -f websocket_trading_bot.py'.")
        return True, f"systemd: {SYSTEMCTL_SCOPE} unit {SERVICE_UNIT}"
    return False, (f"CONTROL_RESTART_MODE={RESTART_MODE!r} is not recognised "
                   f"(use 'systemd' or 'command').")


def service_state() -> str:
    """A human description of the trader's current state."""
    if RESTART_MODE == "systemd" and SERVICE_UNIT:
        try:
            out = subprocess.run(_systemctl("is-active", SERVICE_UNIT),
                                 capture_output=True, text=True, timeout=10)
            return (out.stdout or out.stderr).strip() or "unknown"
        except (subprocess.SubprocessError, FileNotFoundError) as e:
            return f"unknown ({e})"
    alive = process_running()
    if alive is None:
        return "unknown"
    return "running" if alive else "NOT RUNNING"


def cmd_restart(chat_id: str, args: List[str]) -> str:
    ok, detail = restart_available()
    if not ok:
        return "/restart is disabled.\n\n" + detail

    if RESTART_MODE == "command":
        argv = shlex.split(RESTART_COMMAND)
        label = RESTART_COMMAND
    else:
        argv = _systemctl("restart", SERVICE_UNIT)
        label = SERVICE_UNIT

    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    except (subprocess.SubprocessError, FileNotFoundError) as e:
        return f"Restart failed: {e}"

    note = ""
    if out.returncode != 0:
        # pkill exits 1 when nothing matched — for a restart that means the
        # trader wasn't running, which the supervising loop may still fix.
        stderr = (out.stderr or "").strip()[:300]
        note = (f"\n(command exited {out.returncode}"
                + (f": {stderr}" if stderr else ", no process matched")
                + ")")

    # Give the supervisor time to respawn: ~3s loop sleep + ~1s startup.
    time.sleep(RESTART_SETTLE_SEC)
    alive = process_running()
    if alive is True:
        return f"Restarted via {label}.\nTrader is running.{note}"
    if alive is False:
        return (f"Ran {label}, but no process matches '{PROCESS_PATTERN}' "
                f"{RESTART_SETTLE_SEC:.0f}s later — the trader may be down.{note}")
    return f"Ran {label}. Could not verify (pgrep unavailable).{note}"


def _restart_hint() -> str:
    ok, _ = restart_available()
    return ("\n\nNOT LIVE YET — the trader reads its config only at startup."
            + ("\nSend /restart to apply." if ok else "\nRestart the trader to apply."))


def cmd_bot(chat_id: str, args: List[str]) -> str:
    """Read or set a parameter in the config's bot: block."""
    sess = session(chat_id)
    path = sess["config"]
    if not args:
        try:
            with open(path, "r", encoding="utf-8") as f:
                bot_cfg = (yaml.safe_load(f) or {}).get("bot") or {}
        except (OSError, yaml.YAMLError) as e:
            return f"Could not read {path}: {e}"
        body = "\n".join(f"  {k}: {v}" for k, v in sorted(bot_cfg.items()))
        return f"{path} bot: block\n\n{body}\n\nSet one with /bot <key> <value>."
    if len(args) < 2:
        return "Usage: /bot <key> <value>   (or /bot with no args to list)"

    key, value = args[0], " ".join(args[1:])
    changed, message = mc.set_bot_param(path, key, value)
    if not changed:
        return message
    return f"{path}\n{message}" + _restart_hint()


def cmd_dryrun(chat_id: str, args: List[str]) -> str:
    """Flip dry_run — a soft pause that needs no process control."""
    sess = session(chat_id)
    path = sess["config"]
    if not args:
        try:
            with open(path, "r", encoding="utf-8") as f:
                current = ((yaml.safe_load(f) or {}).get("bot") or {}).get("dry_run")
        except (OSError, yaml.YAMLError) as e:
            return f"Could not read {path}: {e}"
        return f"dry_run is {current}. Use /dryrun on  (stop trading) or /dryrun off."

    word = args[0].strip().lower()
    if word in ("on", "true", "yes", "1"):
        target = "true"
    elif word in ("off", "false", "no", "0"):
        target = "false"
    else:
        return "Usage: /dryrun on|off"

    changed, message = mc.set_bot_param(path, "dry_run", target)
    if not changed:
        return message
    note = ("\n\nDRY RUN — the bot will log trades without placing them."
            if target == "true" else "\n\nLIVE — the bot will place real orders.")
    return f"{path}\n{message}{note}" + _restart_hint()


def cmd_log(chat_id: str, args: List[str]) -> str:
    """Tail the trader log, so diagnosing doesn't need an SSH session."""
    path = os.getenv("CONTROL_TRADER_LOG", "trader.log")
    try:
        count = int(args[0]) if args else 25
    except ValueError:
        return "Usage: /log [lines]"
    count = max(1, min(count, 200))
    if not os.path.exists(path):
        return (f"No log at {path}. Start the trader with output redirected, e.g.\n"
                f"  ... | tee -a trader.log\n"
                f"or point CONTROL_TRADER_LOG at an existing file.")
    try:
        lines = _tail_lines(path, count)
    except OSError as e:
        return f"Could not read {path}: {e}"
    return f"{path} (last {len(lines)} lines)\n\n" + "".join(lines)


def _tail_lines(path: str, count: int, block: int = 65536) -> List[str]:
    """Last `count` lines, read backwards from the end: the trader log only grows, and reading
    all of it on the trader's VM would compete with the trader for CPU and memory."""
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        pos = f.tell()
        data = b""
        while pos > 0 and data.count(b"\n") <= count:
            step = min(block, pos)
            pos -= step
            f.seek(pos)
            data = f.read(step) + data
    lines = data.decode("utf-8", errors="replace").splitlines(keepends=True)
    return lines[-count:]


def cmd_undo(chat_id: str, args: List[str]) -> str:
    """Swap the config with its .bak — every write leaves one behind."""
    sess = session(chat_id)
    ok, message = mc.restore_backup(sess["config"])
    sess.pop("listing", None)
    return message + (_restart_hint() if ok else "")


def cmd_config(chat_id: str, args: List[str]) -> str:
    sess = session(chat_id)
    if not args:
        return f"Config: {sess['config']}\nUsage: /config <path>"
    path = args[0]
    if not os.path.exists(path):
        return f"{path} does not exist."
    sess["config"] = path
    sess.pop("listing", None)
    return f"Now managing {path} ({len(read_config_pairs(path))} pairs)."


HANDLERS = {
    "help": cmd_help, "start": cmd_help,
    "connect": cmd_connect, "fixtures": cmd_fixtures, "add": cmd_add,
    "sports": cmd_sports,
    "confirm": cmd_confirm, "cancel": cmd_cancel,
    "fix": cmd_fix, "rewrite": cmd_rewrite, "alias": cmd_rewrite,
    "kalshi": cmd_kalshi, "answers": cmd_answers_lookup,
    "pairs": cmd_pairs, "drop": cmd_drop, "reap": cmd_reap,
    "status": cmd_status, "restart": cmd_restart, "config": cmd_config,
    "bot": cmd_bot, "dryrun": cmd_dryrun, "log": cmd_log, "undo": cmd_undo,
}


def dispatch(chat_id: str, text: str) -> Optional[str]:
    text = text.strip()
    if not text.startswith("/"):
        return None
    try:
        parts = shlex.split(text)
    except ValueError:
        parts = text.split()
    if not parts:
        return None

    cmd = parts[0][1:].lower()
    cmd = cmd.split("@", 1)[0]  # /cmd@BotName in groups
    args = parts[1:]

    if cmd in SPORT_COMMANDS:
        return cmd_sport(chat_id, args, SPORT_COMMANDS[cmd], cmd.upper())
    handler = HANDLERS.get(cmd)
    if not handler:
        return f"Unknown command /{cmd}. /help for the list."
    return handler(chat_id, args)


# ── Main loop ─────────────────────────────────────────────────────────────────

def main() -> int:
    if not TOKEN:
        print("✗ No token: set CONTROL_TELEGRAM_BOT_TOKEN (preferred) or "
              "TELEGRAM_BOT_TOKEN.")
        return 1
    if not AUTHORIZED_CHAT_IDS:
        print("✗ No TELEGRAM_CHAT_IDS set. Refusing to start: this bot writes "
              "configs and can restart the trader, so it must not accept commands "
              "from arbitrary chats.")
        return 1

    offset = STATE.get("telegram_offset")
    if offset is None:
        # Skip whatever accumulated while this bot was down, so stale commands
        # don't fire on startup.
        pending = get_updates(None)
        if pending:
            offset = pending[-1]["update_id"] + 1
            print(f"[init] skipped {len(pending)} backlog update(s)")
        STATE["telegram_offset"] = offset
        save_state(STATE)

    print(f"✓ Control bot up. Config: {DEFAULT_CONFIG} | "
          f"authorized chats: {len(AUTHORIZED_CHAT_IDS)} | "
          f"service: {SERVICE_UNIT or '(none)'}")

    while True:
        for update in get_updates(offset):
            offset = update["update_id"] + 1
            STATE["telegram_offset"] = offset
            save_state(STATE)

            message = update.get("message") or update.get("edited_message") or {}
            text = message.get("text") or ""
            chat_id = str((message.get("chat") or {}).get("id") or "")
            if not text or not chat_id:
                continue

            if chat_id not in AUTHORIZED_CHAT_IDS:
                # Don't reply — an unauthorized chat gets no signal that the bot
                # is even listening.
                print(f"[auth] ignored message from chat {chat_id}")
                continue

            print(f"[cmd] {chat_id}: {text[:120]}")
            try:
                reply = dispatch(chat_id, text)
            except Exception as e:  # a bad command must never kill the loop
                traceback.print_exc()
                reply = f"Command failed: {type(e).__name__}: {e}"
            if reply:
                send(chat_id, reply)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nbye")
