#!/usr/bin/env python3
"""Auto-connect ManifoldSports markets to Kalshi.

Fetches every open sports match created by the official Manifold sports
account (https://manifold.markets/ManifoldSports), matches each one to the
corresponding Kalshi game event, and either regenerates a dedicated config
file (default: config_websocket_worldcup.yaml) or merges the pairs into an
existing config (--append, and the control bot's /sports).

Leagues: the league is read from `sportsEventId` ("odds:<sport_key>:<id>",
e.g. odds:americanfootball_nfl:..., odds:soccer_epl:...) — `sportsLeague` is
just "Soccer" for every soccer competition, so it can't tell EPL from La Liga.
The World Cup '26 markets predate that and carry "fd-<n>" ids, so they are
still recognised by sportsLeague. Extend SPORT_KEY_TO_LEAGUE / LEAGUES for new
competitions.

Matching is deterministic:
  * Manifold answers carry the team code in `shortText` ("🇲🇽MEX", "TB",
    "LAR"), and Kalshi tickers end in one (KXNFLGAME-26OCT18PITTB-TB).
  * Codes that differ between the two sites are in KALSHI_CODE_ALIASES
    (NFL JAX->JAC, EPL CHE->CFC ...).
  * A code that matches nothing is resolved by NAME, but only inside an event
    already pinned by date and only when exactly one remaining leg fits:
    every token of Kalshi's leg name ("Jacksonville", "Los Angeles R") must be
    in the Manifold answer ("Jacksonville Jaguars", "Los Angeles Rams"). These
    are reported as inferred codes so they can be added to the alias table.
  * The game date is the US/Eastern date of `sportsStartTimestamp` (Kalshi's
    datecode). The exact date wins; ±1 day is tried only when it has nothing —
    MLB plays the same matchup on consecutive days, so a window would be
    ambiguous there.
  * A Manifold "Draw" answer maps to the Kalshi "-TIE" leg. A draw on one side
    without a tie leg on the other is not the same market, and is refused.

Two-way markets (NFL, NBA, NHL, MLB) are sum-to-one with two answers, so
buying YES on one IS buying NO on the other: connecting both legs would only
have the bot trade the same price twice and fight its own fills. Only the team
named first in the question (the away team in "X at Y") is connected. Three-way
soccer markets (home / away / draw) get all three legs.

Usage:
    python make_manifoldsports_configs.py                 # regenerate config_websocket_worldcup.yaml
    python make_manifoldsports_configs.py --dry-run       # print YAML, write nothing
    python make_manifoldsports_configs.py --output my.yaml
    python make_manifoldsports_configs.py --append config_websocket.yaml --league nfl,epl
"""
import argparse
import os
import re
import sys
import time
import unicodedata
from concurrent.futures import ThreadPoolExecutor
from datetime import date, datetime, timedelta, timezone
from typing import Dict, List, Optional, Tuple

import requests
import yaml

from make_configs import _DIGRAPHS, extract_manifold_slug, kalshi_team_name, norm

MANIFOLD_API = "https://api.manifold.markets/v0"
KALSHI_API = "https://api.elections.kalshi.com/trade-api/v2"

MANIFOLD_SPORTS_USER_ID = "NnVY8olowYMYQGr346dfmHXBSpx2"  # @ManifoldSports

DEFAULT_OUTPUT = "config_websocket_worldcup.yaml"
BOT_BLOCK_SOURCE = "config_websocket.yaml"  # copy bot: settings from here on first run

# Short league name -> Kalshi game series. The short names are what --league
# and /sports accept.
LEAGUES = {
    "nfl": "KXNFLGAME",
    "ncaaf": "KXNCAAFGAME",
    "nba": "KXNBAGAME",
    "wnba": "KXWNBAGAME",
    "nhl": "KXNHLGAME",
    "mlb": "KXMLBGAME",
    "epl": "KXEPLGAME",
    "laliga": "KXLALIGAGAME",
    "bundesliga": "KXBUNDESLIGAGAME",
    "seriea": "KXSERIEAGAME",
    "ligue1": "KXLIGUE1GAME",
    "mls": "KXMLSGAME",
    "ucl": "KXUCLGAME",
    "uel": "KXUELGAME",
    "wc": "KXWCGAME",
}

# sportsEventId "odds:<sport_key>:<id>" -> short league name. Observed live:
# americanfootball_nfl, soccer_epl (2026-10-16); the rest are the same
# provider's keys for the leagues Kalshi lists.
SPORT_KEY_TO_LEAGUE = {
    "americanfootball_nfl": "nfl",
    "americanfootball_ncaaf": "ncaaf",
    "basketball_nba": "nba",
    "basketball_wnba": "wnba",
    "icehockey_nhl": "nhl",
    "baseball_mlb": "mlb",
    "soccer_epl": "epl",
    "soccer_spain_la_liga": "laliga",
    "soccer_germany_bundesliga": "bundesliga",
    "soccer_italy_serie_a": "seriea",
    "soccer_france_ligue_one": "ligue1",
    "soccer_usa_mls": "mls",
    "soccer_uefa_champs_league": "ucl",
    "soccer_uefa_europa_league": "uel",
    "soccer_fifa_world_cup": "wc",
}

# Markets whose sportsEventId isn't an odds: id (the World Cup '26 ones are
# "fd-<n>") fall back to sportsLeague.
SPORTS_LEAGUE_TO_LEAGUE = {
    "FIFA World Cup": "wc",
    "Formula 1": "f1",
}

# Racing has no "game": ManifoldSports opens one market per session TYPE, named
# "<year> <Grand Prix>: <type>", and Kalshi has one series per type. Verified
# 2026-10-29 — KXF1RACE-MEX26 and KXF1RACEPODIUM-MEX26 open; pole was KXF1POLE
# for every 2026 race (KXF1POLEPOSITION is 2025's). Kalshi lists a race about a
# week out and its event codes are not derivable (MEX26, UNIS26, SPAGP26), so
# races match by DATE and are cross-checked by Grand Prix name.
RACE_LEAGUES = {
    "f1": {
        "race winner": "KXF1RACE",
        "pole position": "KXF1POLE",
        "podium": "KXF1RACEPODIUM",
    },
}

ALL_LEAGUES = list(LEAGUES) + list(RACE_LEAGUES)

# Kalshi's occurrence_datetime is its estimate of when the session ENDS — about
# 6h after Manifold's sportsStartTimestamp for 2026 races and qualifying. Races
# are a week or more apart, so this window can only ever hold one of them.
RACE_WINDOW = (timedelta(hours=-12), timedelta(hours=36))

# Manifold Grand Prix name -> extra words Kalshi may use for the same race
# (Kalshi: "Mexico Grand Prix" for Manifold's "Mexico City Grand Prix").
GP_ALIASES = {
    "sao paulo": {"brazil", "brazilian", "interlagos"},
    "mexico city": {"mexican"},
    "united states": {"usa", "austin", "cota"},
    "emilia romagna": {"imola"},
}
GP_STOPWORDS = {"grand", "prix", "gp", "city", "the", "de", "las", "main", "race",
                "winner", "qualifying", "session", "q3", "pole", "position", "podium",
                "finishers", "qualify", "in", "of"}
RACE_QUESTION_RE = re.compile(r"^\s*(\d{4})\s+(.+?)\s*:\s*(.+?)\s*$")
NAME_SUFFIXES = {"jr", "sr", "ii", "iii", "iv"}

# Team codes that differ between Manifold and Kalshi, per Kalshi series.
# Verified against live markets: WC 2026-06, NFL/EPL 2026-10-16.
KALSHI_CODE_ALIASES = {
    "KXWCGAME": {"ALG": "DZA", "HAI": "HTI", "IRN": "IRI", "URY": "URU"},
    "KXNFLGAME": {"JAX": "JAC"},
    "KXEPLGAME": {"CHE": "CFC", "LIV": "LFC", "BHA": "BRI"},
}

MON = {1: "JAN", 2: "FEB", 3: "MAR", 4: "APR", 5: "MAY", 6: "JUN",
       7: "JUL", 8: "AUG", 9: "SEP", 10: "OCT", 11: "NOV", 12: "DEC"}

try:
    from zoneinfo import ZoneInfo
    EASTERN = ZoneInfo("America/New_York")
except Exception:  # no tzdata on the host: a fixed offset is off by an hour in winter
    EASTERN = timezone(timedelta(hours=-4))


def parse_leagues(tokens) -> Tuple[Optional[set], List[str]]:
    """'nfl,epl' / ['nfl', 'epl'] -> ({'nfl', 'epl'}, unknown). None = all."""
    if not tokens:
        return None, []
    if isinstance(tokens, str):
        tokens = [tokens]
    wanted, unknown = set(), []
    for tok in tokens:
        for name in str(tok).lower().replace(",", " ").split():
            (wanted.add(name) if name in ALL_LEAGUES else unknown.append(name))
    return wanted, unknown


def series_league(series: str) -> Optional[str]:
    """Short league name for a Kalshi series (game or racing), else None."""
    for league, s in LEAGUES.items():
        if s == series:
            return league
    for league, kinds in RACE_LEAGUES.items():
        if series in kinds.values():
            return league
    return None


def ascii_fold(s: str) -> str:
    """'2026 São Paulo Grand Prix' -> '2026 Sao Paulo Grand Prix'; flags dropped."""
    s = unicodedata.normalize("NFKD", s or "")
    s = "".join(ch for ch in s if not unicodedata.combining(ch))
    s = re.sub(r"[^\x20-\x7E]+", "", s)
    return re.sub(r"\s+", " ", s).strip()


# -----------------------------
# Manifold
# -----------------------------
def fetch_manifold_sports_markets() -> List[dict]:
    """All unresolved, still-open markets created by ManifoldSports (lite)."""
    out: List[dict] = []
    before: Optional[str] = None
    while True:
        params = {"userId": MANIFOLD_SPORTS_USER_ID, "limit": 1000}
        if before:
            params["before"] = before
        r = requests.get(f"{MANIFOLD_API}/markets", params=params, timeout=30)
        r.raise_for_status()
        batch = r.json()
        out.extend(batch)
        if len(batch) < 1000:
            break
        before = batch[-1]["id"]
    now_ms = time.time() * 1000
    return [m for m in out
            if not m.get("isResolved") and m.get("closeTime", 0) > now_ms]


def fetch_manifold_market_full(market_id: str) -> dict:
    r = requests.get(f"{MANIFOLD_API}/market/{market_id}", timeout=30)
    r.raise_for_status()
    return r.json()


def market_league(market: dict) -> Optional[str]:
    """Short league name for a ManifoldSports market, or None if unmapped."""
    parts = (market.get("sportsEventId") or "").split(":")
    if len(parts) >= 3 and parts[0] == "odds":
        return SPORT_KEY_TO_LEAGUE.get(parts[1])
    if parts[0] == "bdl-f1":
        return "f1"
    return SPORTS_LEAGUE_TO_LEAGUE.get(market.get("sportsLeague") or "")


def market_sport_label(market: dict) -> str:
    parts = (market.get("sportsEventId") or "").split(":")
    if len(parts) >= 3 and parts[0] == "odds":
        return parts[1]
    return market.get("sportsLeague") or "?"


def is_draw(answer: dict) -> bool:
    return any((answer.get(f) or "").strip().lower() == "draw"
               for f in ("shortText", "text"))


def answer_team_code(answer: dict) -> Optional[str]:
    """The team code from an answer's shortText ("🇲🇽MEX" -> MEX, "TB" -> TB).

    2-4 capital letters. Text is used only when it is itself a bare code — a
    full name like "Los Angeles Rams" must not read as "LAR" by accident.
    Returns None for the Draw answer (or anything without a code).
    """
    if is_draw(answer):
        return None
    for field in ("shortText", "text"):
        letters = re.sub(r"[^A-Za-z]", "", answer.get(field) or "")
        if 2 <= len(letters) <= 4 and letters.isupper():
            return letters
    return None


# -----------------------------
# Kalshi
# -----------------------------
def fetch_kalshi_series_markets(series_ticker: str) -> List[dict]:
    out: List[dict] = []
    cursor: Optional[str] = None
    while True:
        params = {"series_ticker": series_ticker, "limit": 1000, "status": "open"}
        if cursor:
            params["cursor"] = cursor
        r = requests.get(f"{KALSHI_API}/markets", params=params, timeout=30)
        r.raise_for_status()
        data = r.json()
        out.extend(data.get("markets", []))
        cursor = data.get("cursor")
        if not cursor:
            break
    return out


def index_kalshi_events(markets: List[dict], series: str) -> Dict[str, Dict[str, Dict[str, dict]]]:
    """datecode -> event_ticker -> {leg code: market}.

    Leg codes come from each market's ticker suffix, not from splitting the
    event's matchup chunk: NFL/NHL codes are 2 or 3 letters (PITTB, NJWSH), so
    a fixed-width split cannot find the boundary. MLB event tickers carry a
    start time after the date (26OCT172000TBCLE); only the date is read.
    """
    out: Dict[str, Dict[str, Dict[str, dict]]] = {}
    date_re = re.compile(rf"^{re.escape(series)}-(\d{{2}}[A-Z]{{3}}\d{{2}})")
    for m in markets:
        ticker = m.get("ticker") or ""
        event = m.get("event_ticker") or ticker.rsplit("-", 1)[0]
        dm = date_re.match(event)
        if not dm or not ticker.startswith(event + "-"):
            continue
        code = ticker[len(event) + 1:]
        out.setdefault(dm.group(1), {}).setdefault(event, {})[code] = m
    return out


def eastern_datecode(kickoff_utc: datetime, shift_days: int = 0) -> str:
    d = kickoff_utc.astimezone(EASTERN).date() + timedelta(days=shift_days)
    return f"{d.year % 100:02d}{MON[d.month]}{d.day:02d}"


# -----------------------------
# Matching
# -----------------------------
def name_fits(kalshi_name: str, manifold_text: str) -> bool:
    """Every token of Kalshi's leg name appears in the Manifold answer text.

    Kalshi abbreviates shared cities to one letter ("Los Angeles R",
    "New York J"); such a token must START a Manifold token, which is what
    tells the Rams from the Chargers.
    """
    def fold(t: str) -> str:   # Kalshi "Bodoe" vs Manifold "Bodø" (norm: "bodo")
        for src, dst in _DIGRAPHS:
            t = t.replace(src, dst)
        return t

    k = [fold(t) for t in norm(kalshi_name).split()]
    m = [fold(t) for t in norm(manifold_text).split()]
    if not k:
        return False
    for tok in k:
        if len(tok) == 1:
            if not any(w.startswith(tok) for w in m):
                return False
        elif tok not in m:
            return False
    return True


def assign_teams(team_answers: List[Tuple[dict, Optional[str]]], legs: Dict[str, dict],
                 aliases: Dict[str, str]) -> Optional[Tuple[Dict[str, str], List[str]]]:
    """Map each team answer to a distinct non-TIE leg of one event.

    Returns ({answer_id: kalshi_code}, inferred notes) or None when the event
    is not this matchup. Codes first; then, for answers whose code matched
    nothing, the single remaining leg whose name fits.
    """
    team_legs = {c: m for c, m in legs.items() if c != "TIE"}
    if len(team_legs) != len(team_answers):
        return None
    assigned: Dict[str, str] = {}
    pending = []
    for answer, code in team_answers:
        kcode = aliases.get(code, code) if code else None
        if kcode and kcode in team_legs and kcode not in assigned.values():
            assigned[answer["id"]] = kcode
        else:
            pending.append((answer, code))

    inferred = []
    for answer, code in pending:
        free = [c for c in team_legs if c not in assigned.values()]
        fits = [c for c in free
                if name_fits(kalshi_team_name(team_legs[c]), answer.get("text") or "")]
        if len(fits) != 1:
            return None
        assigned[answer["id"]] = fits[0]
        inferred.append(f"{code or answer.get('text')}->{fits[0]} "
                        f"(by name: {kalshi_team_name(team_legs[fits[0]])!r})")
    return assigned, inferred


def first_named(team_answers: List[Tuple[dict, Optional[str]]], question: str) -> dict:
    """The team named first in the question — the away team in "X at Y"."""
    q = norm(question)

    def pos(answer: dict) -> Tuple[int, int]:
        text = norm(answer.get("text") or "")
        i = q.find(text) if text else -1
        return (i if i >= 0 else len(q), answer.get("index") or 0)

    return min((a for a, _ in team_answers), key=pos)


def fetch_kalshi_series_events(series_ticker: str) -> Dict[str, dict]:
    """Open events of a series by ticker. Racing needs these: the Grand Prix
    name is on the EVENT ("Mexico Grand Prix 2026"), never on its markets."""
    out: Dict[str, dict] = {}
    cursor: Optional[str] = None
    while True:
        params = {"series_ticker": series_ticker, "status": "open", "limit": 200}
        if cursor:
            params["cursor"] = cursor
        r = requests.get(f"{KALSHI_API}/events", params=params, timeout=30)
        r.raise_for_status()
        data = r.json()
        for e in data.get("events", []):
            out[e["event_ticker"]] = e
        cursor = data.get("cursor")
        if not cursor:
            return out


def race_event_time(legs: Dict[str, dict]) -> Optional[datetime]:
    """When Kalshi expects the session to end (occurrence_datetime)."""
    for m in legs.values():
        for field in ("occurrence_datetime", "expected_expiration_time"):
            v = m.get(field)
            if v:
                try:
                    return datetime.fromisoformat(str(v).replace("Z", "+00:00"))
                except ValueError:
                    pass
    return None


def gp_names_agree(manifold_gp: str, event: dict) -> bool:
    """Does Kalshi's event name the same Grand Prix? A cross-check on the date
    match, so a mis-dated Kalshi event can't be wired to the wrong race."""
    gp = norm(manifold_gp)
    ours = (set(gp.split()) - GP_STOPWORDS) | GP_ALIASES.get(gp.replace(" grand prix", "").strip(), set())
    theirs = set(norm(f"{event.get('title', '')} {event.get('sub_title', '')}").split()) - GP_STOPWORDS
    return bool(ours & theirs)


def _name_tokens(name: str) -> List[str]:
    toks = []
    for t in norm(name).split():
        if t in NAME_SUFFIXES:
            continue
        for src, dst in _DIGRAPHS:
            t = t.replace(src, dst)
        toks.append(t)
    return toks


def match_driver(text: str, legs: Dict[str, dict]) -> Optional[str]:
    """Kalshi leg code for a Manifold driver answer, or None.

    Surname must match exactly and be unique among the legs; the given names
    must then share a token ("Kimi Antonelli" vs "Andrea Kimi Antonelli") or,
    failing that, an initial. "Sainz Jr." / "Hulkenberg" vs "Hülkenberg" fold
    away in _name_tokens.
    """
    ours = _name_tokens(text)
    if len(ours) < 2:
        return None
    surname, given = ours[-1], ours[:-1]
    cands = []
    for code, m in legs.items():
        theirs = _name_tokens(kalshi_team_name(m))
        if len(theirs) >= 2 and theirs[-1] == surname:
            cands.append((code, theirs[:-1]))
    if len(cands) != 1:
        return None
    code, their_given = cands[0]
    if set(given) & set(their_given):
        return code
    if {g[0] for g in given} & {g[0] for g in their_given}:
        return code
    return None


def build_race_pairs(selected: List[Tuple[dict, str, datetime]], diag: dict,
                     verbose: bool = True) -> List[dict]:
    """Pairs for racing markets (F1): every driver answer with a Kalshi leg.

    Race winner and pole are one-of-N, podium is N independent yes/nos; both
    sides agree on which (Manifold shouldAnswersSumToOne vs Kalshi
    mutually_exclusive) or the market is refused. Unlike a two-way game, the
    legs of a one-of-N market are distinct prices, so every driver is connected.
    """
    misses, not_listed = diag["misses"], diag["not_listed"]
    unmatched = diag.setdefault("unmatched", [])

    work = []
    for lite, league, start in selected:
        slug = lite.get("slug", "")
        q = RACE_QUESTION_RE.match(ascii_fold(lite.get("question", "")))
        kind = q.group(3).lower() if q else None
        series = RACE_LEAGUES[league].get(kind or "")
        if not series:
            misses.append(f"unmapped {league.upper()} market type {kind!r}: {slug} "
                          f"— add it to RACE_LEAGUES")
            continue
        work.append((lite, start, series, q.group(2)))

    kalshi: Dict[str, Tuple[Dict[str, dict], Dict[str, Dict[str, dict]]]] = {}
    for series in sorted({w[2] for w in work}):
        legs_by_event: Dict[str, Dict[str, dict]] = {}
        markets = fetch_kalshi_series_markets(series)
        for m in markets:
            ticker, event = m.get("ticker") or "", m.get("event_ticker") or ""
            if event and ticker.startswith(event + "-"):
                legs_by_event.setdefault(event, {})[ticker[len(event) + 1:]] = m
        kalshi[series] = (fetch_kalshi_series_events(series) if legs_by_event else {}, legs_by_event)
        if verbose:
            print(f"# Kalshi {series}: {len(markets)} open markets in {len(legs_by_event)} event(s)")

    to_fetch = []
    for lite, start, series, gp in work:
        slug = lite.get("slug", "")
        meta, legs_by_event = kalshi[series]
        lo, hi = start + RACE_WINDOW[0], start + RACE_WINDOW[1]
        hits = [e for e, legs in legs_by_event.items()
                if (t := race_event_time(legs)) is not None and lo <= t <= hi]
        if not hits:
            if start.timestamp() < time.time():
                misses.append(f"no open Kalshi {series} market for {slug} "
                              f"(started {start.astimezone(EASTERN):%b %d %H:%M} ET)")
            else:
                not_listed.append(f"{slug} ({start.astimezone(EASTERN):%b %d})")
            continue
        if len(hits) > 1:
            misses.append(f"ambiguous Kalshi {series} events for {slug}: {sorted(hits)}")
            continue
        event = hits[0]
        if not gp_names_agree(gp, meta.get(event, {})):
            misses.append(f"{slug}: {event} is on the right date but names "
                          f"{(meta.get(event) or {}).get('sub_title')!r}, not {gp!r} — "
                          f"add the name to GP_ALIASES if it is the same race")
            continue
        to_fetch.append((lite, series, event))

    def fetch(item):
        try:
            return fetch_manifold_market_full(item[0]["id"]), None
        except requests.RequestException as e:
            return None, e

    with ThreadPoolExecutor(max_workers=6) as pool:
        fulls = list(pool.map(fetch, to_fetch))

    pairs: List[dict] = []
    for (lite, series, event), (full, err) in zip(to_fetch, fulls):
        slug = lite.get("slug", "")
        if err is not None:
            misses.append(f"could not fetch {slug}: {err}")
            continue
        meta, legs_by_event = kalshi[series]
        legs = legs_by_event[event]
        exclusive = (meta.get(event) or {}).get("mutually_exclusive")
        one_of_n = bool(full.get("shouldAnswersSumToOne"))
        if exclusive is not None and bool(exclusive) != one_of_n:
            misses.append(f"{slug} is {'one-of-N' if one_of_n else 'independent yes/no'} but "
                          f"{event} is {'' if exclusive else 'not '}mutually exclusive — refused")
            continue

        question = ascii_fold(lite.get("question", ""))
        claims: Dict[str, List[dict]] = {}
        missing = []
        for a in full.get("answers", []):
            if a.get("resolution"):
                continue
            text = a.get("text") or ""
            if (a.get("shortText") or "").upper() == "OTHER" or norm(text).startswith("another"):
                continue                     # "Another driver": no Kalshi counterpart
            code = match_driver(text, legs)
            if code is None:
                missing.append(text)
            else:
                claims.setdefault(code, []).append(a)
        for code, answers in claims.items():
            if len(answers) > 1:             # two answers on one leg: neither is safe
                misses.append(f"{slug}: {[x.get('text') for x in answers]} all match {event}-{code}")
                continue
            a = answers[0]
            pairs.append({
                "name": f"{question} - {ascii_fold(a.get('text') or '')}",
                "kalshi_ticker": f"{event}-{code}",
                "manifold_slug": slug,
                "manifold_answer_id": a["id"],
            })
        if missing:
            unmatched.append(f"{slug}: {', '.join(ascii_fold(t) for t in missing)}")
    return pairs


def build_market_pairs(verbose: bool = True, leagues: Optional[set] = None,
                       game_date: Optional[date] = None,
                       diagnostics: Optional[dict] = None) -> List[dict]:
    """Pairs for every open ManifoldSports market Kalshi lists.

    leagues:     short names (ALL_LEAGUES) to keep; None = every mapped league.
    game_date:   keep only games/sessions on this US/Eastern date.
    diagnostics: filled with {"misses": [...], "not_listed": [...],
                 "inferred": [...], "unmatched": [...], "two_way": {slugs},
                 "markets": n, "matched": n} for callers that report themselves
                 (the control bot) rather than read stdout.
    """
    diag = diagnostics if diagnostics is not None else {}
    misses: List[str] = diag.setdefault("misses", [])
    not_listed: List[str] = diag.setdefault("not_listed", [])
    inferred_all: List[str] = diag.setdefault("inferred", [])
    two_way: set = diag.setdefault("two_way", set())
    diag.setdefault("unmatched", [])

    selected = []
    unmapped: Dict[str, int] = {}
    for lite in fetch_manifold_sports_markets():
        league = market_league(lite)
        if not league:
            label = market_sport_label(lite)
            unmapped[label] = unmapped.get(label, 0) + 1
            continue
        if leagues is not None and league not in leagues:
            continue
        start = lite.get("sportsStartTimestamp")
        if not start:
            misses.append(f"no sportsStartTimestamp: {lite.get('slug')}")
            continue
        try:
            kickoff = (datetime.fromtimestamp(start / 1000, timezone.utc)
                       if isinstance(start, (int, float)) else
                       datetime.fromisoformat(str(start).replace("Z", "+00:00")).astimezone(timezone.utc))
        except ValueError:
            misses.append(f"unreadable sportsStartTimestamp {start!r}: {lite.get('slug')}")
            continue
        if game_date and kickoff.astimezone(EASTERN).date() != game_date:
            continue
        selected.append((lite, league, kickoff))
    selected.sort(key=lambda t: (t[1], t[2]))  # league, then kickoff
    for label, n in sorted(unmapped.items()):
        misses.append(f"unmapped league {label!r} ({n} market(s)) — add it to SPORT_KEY_TO_LEAGUE")
    diag["markets"] = len(selected)
    if verbose:
        print(f"# ManifoldSports open markets selected: {len(selected)}")

    races = [t for t in selected if t[1] in RACE_LEAGUES]
    selected_games = [t for t in selected if t[1] in LEAGUES]

    # Each needed Kalshi series once.
    events_by_series: Dict[str, Dict[str, Dict[str, Dict[str, dict]]]] = {}
    for series in sorted({LEAGUES[lg] for _, lg, _ in selected_games}):
        markets = fetch_kalshi_series_markets(series)
        events_by_series[series] = index_kalshi_events(markets, series)
        if verbose:
            print(f"# Kalshi {series}: {len(markets)} open markets")

    # Skip the answer fetch for games Kalshi hasn't listed yet. ManifoldSports
    # creates fixtures ~14 days out; Kalshi lists closer to the game.
    to_fetch = []
    for lite, league, kickoff in selected_games:
        events = events_by_series[LEAGUES[league]]
        if not any(eastern_datecode(kickoff, s) in events for s in (0, -1, 1)):
            if kickoff.timestamp() < time.time():
                # Started or finished and Kalshi's market is no longer open —
                # not something a re-run will fix.
                misses.append(f"no open Kalshi market for {lite.get('slug')} "
                              f"(kicked off {kickoff.astimezone(EASTERN):%b %d %H:%M} ET)")
            else:
                not_listed.append(f"{lite.get('slug')} ({kickoff.astimezone(EASTERN):%b %d})")
            continue
        to_fetch.append((lite, league, kickoff))

    def fetch(item):
        # One market's failure is that market's miss, not the whole run's.
        try:
            return fetch_manifold_market_full(item[0]["id"]), None
        except requests.RequestException as e:
            return None, e

    with ThreadPoolExecutor(max_workers=6) as pool:
        fulls = list(pool.map(fetch, to_fetch))

    pairs: List[dict] = []
    for (lite, league, kickoff), (full, err) in zip(to_fetch, fulls):
        slug = lite.get("slug", "")
        if err is not None:
            misses.append(f"could not fetch {slug}: {err}")
            continue
        series = LEAGUES[league]
        aliases = KALSHI_CODE_ALIASES.get(series, {})
        events = events_by_series[series]

        answers = [a for a in full.get("answers", []) if not a.get("resolution")]
        draw_answers = [a for a in answers if is_draw(a)]
        team_answers = [(a, answer_team_code(a)) for a in answers if not is_draw(a)]
        if len(team_answers) != 2 or len(draw_answers) > 1:
            misses.append(f"expected 2 teams (+ optional Draw): {slug} -> "
                          f"{[a.get('shortText') or a.get('text') for a in answers]}")
            continue

        # Exact Eastern date first, then ±1 day.
        chosen = []
        for shifts in ((0,), (-1, 1)):
            chosen = []
            for s in shifts:
                for event, legs in events.get(eastern_datecode(kickoff, s), {}).items():
                    got = assign_teams(team_answers, legs, aliases)
                    if got:
                        chosen.append((event, legs, got))
            if chosen:
                break
        codes = sorted(c or "?" for _, c in team_answers)
        if not chosen:
            misses.append(f"no Kalshi {series} event for {slug} (codes {codes}, "
                          f"{kickoff.astimezone(EASTERN):%b %d} ET)")
            continue
        if len(chosen) > 1:
            misses.append(f"ambiguous Kalshi events for {slug}: {[c[0] for c in chosen]}")
            continue
        event, legs, (assigned, inferred) = chosen[0]

        if draw_answers and "TIE" not in legs:
            misses.append(f"{slug} has a Draw answer but {event} has no TIE leg — not the same market")
            continue
        if not draw_answers and "TIE" in legs:
            misses.append(f"{event} has a TIE leg but {slug} has no Draw answer — not the same market")
            continue
        for note in inferred:
            inferred_all.append(f"{series} {note} — {slug}")

        question = ascii_fold(lite.get("question", ""))
        if draw_answers:
            entries = [(a, f"{event}-{assigned[a['id']]}", c) for a, c in team_answers]
            entries.append((draw_answers[0], f"{event}-TIE", "Draw"))
        else:
            # Two-way, sum-to-one: one leg carries the whole market, and the
            # other team's leg rides along as its partner (both legs price it).
            pick = first_named(team_answers, question)
            pick_code = next(c for a, c in team_answers if a is pick)
            other = next(a for a, c in team_answers if a is not pick)
            entries = [(pick, f"{event}-{assigned[pick['id']]}", pick_code)]
            partner = f"{event}-{assigned[other['id']]}"
            two_way.add(slug)

        for answer, ticker, label in entries:
            pair = {
                "name": f"{question} - {label or assigned.get(answer['id'])}",
                "kalshi_ticker": ticker,
                "manifold_slug": slug,
                "manifold_answer_id": answer["id"],
            }
            if not draw_answers:
                pair["partner_ticker"] = partner
            pairs.append(pair)

    pairs += build_race_pairs(races, diag, verbose)

    diag["matched"] = len({p["manifold_slug"] for p in pairs})
    if verbose:
        if diag["unmatched"]:
            print("\n# --- ANSWERS WITH NO KALSHI LEG (not connected) ---")
            for n in diag["unmatched"]:
                print("#", n)
        if inferred_all:
            print("\n# --- CODES INFERRED BY NAME (add to KALSHI_CODE_ALIASES) ---")
            for n in inferred_all:
                print("#", n)
        if not_listed:
            print(f"\n# {len(not_listed)} game(s) not on Kalshi yet: {', '.join(not_listed)}")
        if misses:
            print("\n# --- MISSES ---")
            for m in misses:
                print("#", m)
        print(f"\n# Matched {diag['matched']}/{len(selected)} matches "
              f"-> {len(pairs)} market pairs")
    return pairs


def split_connected_two_way(specs: List[dict], existing: List[dict],
                            two_way: Optional[set] = None) -> Tuple[List[dict], List[dict]]:
    """Withhold a two-way game's leg when the config already trades its other leg.

    /sports always picks the first-named team, but a game connected by hand
    (or before this rule) may sit on the other answer. Adding this one too
    would put both legs of one sum-to-one market in the config. Returns
    (kept, withheld). Three-way and one-of-N (F1) markets are untouched.

    two_way: the slugs build_market_pairs marked two-way. Without it, a slug
    with exactly one spec is taken as two-way — wrong for an F1 market where
    only one driver happened to match, hence the explicit set when available.
    """
    def slug(p: dict) -> str:  # hand-connected slugs may carry a ?r= share suffix
        return extract_manifold_slug(str(p.get("manifold_slug") or "")).lower()

    per_slug: Dict[str, int] = {}
    for s in specs:
        per_slug[slug(s)] = per_slug.get(slug(s), 0) + 1
    have: Dict[str, set] = {}
    for p in existing:
        have.setdefault(slug(p), set()).add(p.get("manifold_answer_id"))

    kept, withheld = [], []
    for s in specs:
        other = have.get(slug(s), set()) - {s["manifold_answer_id"]}
        is_two_way = (slug(s) in {x.lower() for x in two_way}) if two_way is not None \
            else per_slug[slug(s)] == 1
        if is_two_way and other:
            withheld.append(s)
        else:
            kept.append(s)
    return kept, withheld


# -----------------------------
# Config output
# -----------------------------
DEFAULT_BOT_BLOCK = {
    "dry_run": False,
    "passive_bet_size": 500,
    "passive_spread_buffer": 0.08,
    "passive_order_duration_min": 60,
    "max_bet_size": 50,
    "min_edge_threshold": 0.02,
    "max_edge_threshold": 0.1,
    "amm_target_fraction": 0.95,
    "edge_scale_start": 0.1,
    "max_kalshi_spread": 0.05,
    "min_time_between_trades": 0,
    "max_staleness_ms": 5000,
    "max_position": 1000,
    "networth": 5000,
    "kelly_fraction_limit": 0.25,
    "min_edge_to_trade": 0.01,
    "request_timeout": 1,
}


def load_bot_block(output_path: str) -> dict:
    """Preserve bot settings of an existing output file; else copy from the
    main config; else use defaults."""
    for path in (output_path, BOT_BLOCK_SOURCE):
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                cfg = yaml.safe_load(f) or {}
            if cfg.get("bot"):
                print(f"# bot: settings taken from {path}")
                return cfg["bot"]
    return dict(DEFAULT_BOT_BLOCK)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output", default=DEFAULT_OUTPUT,
                    help=f"output config file (default: {DEFAULT_OUTPUT})")
    ap.add_argument("--append", metavar="CONFIG",
                    help="merge the pairs into this existing config instead of "
                         "regenerating --output (pairs already there are skipped)")
    ap.add_argument("--league", action="append",
                    help=f"only these leagues, e.g. nfl,epl (known: {', '.join(ALL_LEAGUES)})")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the generated YAML instead of writing the file")
    args = ap.parse_args()

    leagues, unknown = parse_leagues(args.league)
    if unknown:
        ap.error(f"unknown league(s): {', '.join(unknown)}")

    if args.append:
        import make_configs as mc
        diag: dict = {}
        pairs = build_market_pairs(leagues=leagues, diagnostics=diag)
        existing = []
        if os.path.exists(args.append):
            with open(args.append, "r", encoding="utf-8") as f:
                existing = [p for p in ((yaml.safe_load(f) or {}).get("market_pairs") or [])
                            if isinstance(p, dict)]
        pairs, withheld = split_connected_two_way(pairs, existing, diag.get("two_way"))
        for p in withheld:
            print(f"# other leg already connected, skipped: {p['name']}")
        mc.append_market_pairs(pairs, args.append, dry_run=args.dry_run)
        return

    bot_block = load_bot_block(args.output)
    pairs = build_market_pairs(leagues=leagues)
    if not pairs:
        print("# No pairs generated — not writing config.")
        sys.exit(1)

    config = {"bot": bot_block, "market_pairs": pairs}
    header = (
        "# Auto-generated by make_manifoldsports_configs.py — re-run it to refresh.\n"
        f"# Generated {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')}: "
        f"ManifoldSports matches -> Kalshi.\n"
        "# Edit the bot: block freely; it is preserved on regeneration.\n"
    )
    body = yaml.dump(config, default_flow_style=False, sort_keys=False,
                     indent=2, allow_unicode=True)
    if args.dry_run:
        print(header + body)
    else:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(header + body)
        print(f"# Wrote {len(pairs)} market pairs to {args.output}")


if __name__ == "__main__":
    main()
