#!/usr/bin/env python3
import argparse
import os
import re
import json
import sys
import unicodedata
import requests
import yaml
from datetime import date, timedelta
from typing import Dict, List, Tuple, Optional

KALSHI_API_BASE = "https://api.elections.kalshi.com/trade-api/v2"

MON = {
    1: "JAN", 2: "FEB", 3: "MAR", 4: "APR", 5: "MAY", 6: "JUN",
    7: "JUL", 8: "AUG", 9: "SEP", 10: "OCT", 11: "NOV", 12: "DEC",
}

SPORTS = {
    "nba": {
        "series_ticker": "KXNBAGAME",
        "ui_base": "https://kalshi.com/markets/kxnbagame/professional-basketball-game",
        "manifold_url": "nba-basketball-games-live-20242025",
        "parser": "nba_win_at",
        "nick_to_kalshi_participant": {
            "Rockets": ["Houston"],
            "Pistons": ["Detroit"],
            "Magic": ["Orlando"],
            "Grizzlies": ["Memphis"],
            "Clippers": ["Los Angeles C", "LA C", "L.A. C"],
            "Lakers": ["Los Angeles L", "LA L", "L.A. L"],
            "Thunder": ["Oklahoma City", "OKC"],
            "Knicks": ["New York", "NY"],
            "Nuggets": ["Denver"],
            "Raptors": ["Toronto"],
            "Bucks": ["Milwaukee"],
            "Pelicans": ["New Orleans"],
            "Bulls": ["Chicago"],
            "Nets": ["Brooklyn"],
            "Wizards": ["Washington"],
            "Kings": ["Sacramento"],
            "Trail Blazers": ["Portland"],
            "Jazz": ["Utah"],
            "Suns": ["Phoenix"],
            "Spurs": ["San Antonio"],
            "Timberwolves": ["Minnesota"],
            "Mavericks": ["Dallas"],
            "Heat": ["Miami"],
            "Pacers": ["Indiana"],
            "Hawks": ["Atlanta"],
            "76ers": ["Philadelphia"],
            "Hornets": ["Charlotte"],
            "Warriors": ["Golden State", "GSW", "GS"],
            "Cavaliers": ["Cleveland"],
            "Celtics": ["Boston"],
        },
    },

    "premier_league": {
        "series_ticker": "KXEPLGAME",
        "ui_base": "https://kalshi.com/markets/kxeplgame/english-premier-league-game",
        "manifold_url": "english-premier-league-matchweek-21",
        "parser": "epl_win",
        "nick_to_kalshi_participant": {
            "Arsenal": ["ARS", "Arsenal"],
            "Aston Villa": ["AVL", "Aston Villa"],
            "Bournemouth": ["BOU", "Bournemouth"],
            "Brentford": ["BRE", "Brentford"],
            "Brighton": ["BRI", "Brighton"],
            "Burnley": ["BUR", "Burnley"],
            "Chelsea": ["CFC", "Chelsea"],
            "Crystal Palace": ["CRY", "Crystal Palace"],
            "Everton": ["EVE", "Everton"],
            "Fulham": ["FUL", "Fulham"],
            "Leeds United": ["LEE", "Leeds United", "Leeds"],
            "Sunderland": ["SUN", "Sunderland"],
            "Liverpool": ["LFC", "Liverpool"],
            "Manchester City": ["MCI", "Manchester City", "Man City"],
            "Manchester United": ["MUN", "Manchester United", "Man United"],
            "Newcastle United": ["NEW", "Newcastle United", "Newcastle"],
            "Nottingham Forest": ["NFO", "Nottingham Forest", "Nott'm Forest"],
            "Tottenham Hotspur": ["TOT", "Tottenham Hotspur", "Tottenham", "Spurs"],
            "West Ham United": ["WHU", "West Ham United", "West Ham"],
            "Wolverhampton Wanderers": ["WOL", "Wolverhampton Wanderers", "Wolves"],
        },
    },
    "nhl": {
        "series_ticker": "KXNHLGAME",
        "ui_base": "https://kalshi.com/markets/kxnhlgame/nhl-game",
        "manifold_url": "nhl-games-live",
        "parser": "nhl_win_at",
        "nick_to_kalshi_participant": {
            "Stars": ["Dallas"],
            "Red Wings": ["Detroit"],
            "Rangers": ["New York R", "NY Rangers", "New York Rangers", "NYR"],
            "Capitals": ["Washington"],
            "Sabres": ["Buffalo"],
            "Senators": ["Ottawa"],
            "Devils": ["New Jersey", "NJ"],
            "Islanders": ["New York I", "NY Islanders", "New York Islanders", "NYI"],
            "Panthers": ["Florida"],
            "Hurricanes": ["Carolina"],
            "Predators": ["Nashville"],
            "Wild": ["Minnesota"],
            "Flyers": ["Philadelphia"],
            "Blackhawks": ["Chicago"],
            "Mammoth": ["Utah"],
            "Avalanche": ["Colorado"],
            "Flames": ["Calgary"],
            "Oilers": ["Edmonton"],
            "Sharks": ["San Jose", "SJ", "SJ Sharks"],
            "Golden Knights": ["Vegas", "VGK", "Golden Knights"],
            "Kraken": ["Seattle"],
            "Kings": ["Los Angeles", "LA", "L.A."],
            "Canadiens": ["Montreal"],
            "Bruins": ["Boston"],

            # Expanded / patched teams
            "Maple Leafs": ["Toronto"],
            "Jets": ["Winnipeg"],
            "Blues": ["St. Louis", "St Louis"],
            "Lightning": ["Tampa Bay", "Tampa", "TB Lighting"],
            "Ducks": ["Anaheim"],
            "Canucks": ["Vancouver"],
            "Penguins": ["Pittsburgh"],
            "Blue Jackets": ["Columbus"],
        },
    },

    "mlb": {
        "series_ticker": "KXMLBGAME",
        "ui_base": "https://kalshi.com/markets/kxmlbgame/professional-baseball-game",
        "manifold_url": "major-league-baseball-live",
        "parser": "mlb_win_at",
        "nick_to_kalshi_participant": {
            "Diamondbacks": ["Arizona"],
            "D-backs": ["Arizona"],
            "Braves": ["Atlanta"],
            "Orioles": ["Baltimore"],
            "Red Sox": ["Boston"],
            "Cubs": ["Chicago C"],
            "White Sox": ["Chicago WS"],
            "Reds": ["Cincinnati"],
            "Guardians": ["Cleveland"],
            "Rockies": ["Colorado"],
            "Tigers": ["Detroit"],
            "Astros": ["Houston"],
            "Royals": ["Kansas City"],
            "Angels": ["Los Angeles A", "LA A", "L.A. A"],
            "Dodgers": ["Los Angeles D", "LA D", "L.A. D"],
            "Marlins": ["Miami"],
            "Brewers": ["Milwaukee"],
            "Twins": ["Minnesota"],
            "Mets": ["New York M", "NY Mets", "New York Mets", "NYM"],
            "Yankees": ["New York Y", "NY Yankees", "New York Yankees", "NYY"],
            "Athletics": ["A's", "Athletics", "Oakland", "Sacramento"],
            "Phillies": ["Philadelphia"],
            "Pirates": ["Pittsburgh"],
            "Padres": ["San Diego"],
            "Giants": ["San Francisco"],
            "Mariners": ["Seattle"],
            "Cardinals": ["St. Louis", "St Louis"],
            "Rays": ["Tampa Bay"],
            "Rangers": ["Texas"],
            "Blue Jays": ["Toronto"],
            "Nationals": ["Washington"],
        },
    },

    "ncaa_basketball": {
        "series_ticker": "KXNCAAMBGAME",
        "ui_base": "https://kalshi.com/markets/kxncaambgame/mens-college-basketball-mens-game",
        "manifold_url": "",  # Set per-run in __main__
        "parser": "ncaa_vs",
        "nick_to_kalshi_participant": {
            "Ohio State": ["Ohio St.", "Ohio State"],
            "USF": ["South Florida", "USF"],
            "Miami": ["Miami (FL)", "Miami"],
            "Saint Mary's": ["Saint Mary's", "SMC"],
        },
    },

    "tennis_atp": {
        "series_ticker": "KXATPMATCH",
        "ui_base": "https://kalshi.com/markets/kxatpmatch/atp-tennis-match",
        "manifold_url": "2026-atp-tour-mens-tennis-matches",
        "parser": "tennis_vs",
        # Matching is by surname token overlap — Kalshi titles are
        #   "Will <FirstName> <Surname> win the <Surname1> vs <Surname2>: Round Of XX match?"
        # so the nick map is empty and participant_opts_from_nick falls back
        # to [nick] (i.e. the surname extracted by parse_tennis_vs).
        "nick_to_kalshi_participant": {},
    },
    "tennis_wta": {
        "series_ticker": "KXWTAMATCH",
        "ui_base": "https://kalshi.com/markets/kxwtamatch/wta-tennis-match",
        "manifold_url": "2026-wta-tour-womens-tennis-matches",
        "parser": "tennis_vs",
        "nick_to_kalshi_participant": {},
    },

    "nfl": {
        "series_ticker": "KXNFLGAME",
        "ui_base": "https://kalshi.com/markets/kxnflgame/professional-football-game",
        "manifold_url": "week-18-nfl-2025-megamarket-all-gam",
        "parser": "nfl_beat",
        # NFL input uses team names (Cowboys, Commanders, etc.)
        # Kalshi titles commonly use city/region names; include multiple aliases where ambiguous (NY/LA).
        "nick_to_kalshi_participant": {
            "Cowboys": ["Dallas"],
            "Commanders": ["Washington"],
            "Lions": ["Detroit"],
            "Vikings": ["Minnesota"],
            "Broncos": ["Denver"],
            "Chiefs": ["Kansas City"],
            "Texans": ["Houston"],
            "Chargers": ["Los Angeles", "LA", "L.A.", "Los Angeles C"],
            "Buccaneers": ["Tampa Bay", "Tampa"],
            "Dolphins": ["Miami"],
            "Steelers": ["Pittsburgh"],
            "Browns": ["Cleveland"],
            "Cardinals": ["Arizona"],
            "Bengals": ["Cincinnati"],
            "Jaguars": ["Jacksonville"],
            "Colts": ["Indianapolis"],
            "Seahawks": ["Seattle", "SEA"],
            "Panthers": ["Carolina"],
            "Eagles": ["Philadelphia"],
            "Bills": ["Buffalo"],
            "Bears": ["Chicago"],
            "49ers": ["San Francisco, 'SF"],
            "Rams": ["Los Angeles", "LA", "L.A.", "Los Angeles R"],
            "Falcons": ["Atlanta"],
            "Giants": ["New York G", "NY Giants", "New York Giants", "NYG", "New York"],
            "Jets": ["New York J", "NY Jets", "New York Jets", "NYJ", "New York"],
            "Patriots": ["New England"],
            "Packers": ["Green Bay"],
            "Saints": ["New Orleans"],
            "Raiders": ["Las Vegas", "Vegas"],
            "Titans": ["Tennessee"],
            "Ravens": ["Baltimore"],
        },
    },
}

# -----------------------------
# Utilities
# -----------------------------
# Letters NFKD leaves alone because they are base characters, not accented
# forms. Values follow the romanization Kalshi/Manifold actually use.
TRANSLITERATE = {
    "ø": "o", "æ": "ae", "ß": "ss", "ł": "l", "đ": "d", "ð": "d", "þ": "th",
    "ı": "i", "œ": "oe", "'": "", "`": "",
}

# Nordic/German romanizations differ between sources: Manifold writes "Bodø",
# Kalshi writes "Bodoe". Collapsing the digraphs makes both "bodo".
_DIGRAPHS = (("oe", "o"), ("ae", "a"), ("ue", "u"))


def collapse_digraphs(s: str) -> str:
    """A looser key for team names, used only as a matching fallback."""
    for src, dst in _DIGRAPHS:
        s = s.replace(src, dst)
    return s.replace(" ", "")


def norm(s: str) -> str:
    """Normalize text for matching: fold accents to ASCII, drop the rest.

    NFKD splits an accented character into base letter + combining mark and
    expands ligatures (ĳ -> ij), so dropping the combining marks leaves plain
    ASCII. Without this the [^a-z0-9] filter below turns 'é' into a *space*, and
    the two sides of a match silently disagree whenever only one carries the
    accent: Manifold's 'Péter Erdő' became 'p ter erd' while Kalshi's
    'Peter Erdo' stayed 'peter erdo' (verified live, 2026-07-29), and
    'Curaçao' / 'Curacao' likewise. Flag emojis still normalize away as before.

    make_configs_polymarket.py imports this function, so both generators share
    the fix.

    NFKD does not decompose letters that are their own base character rather
    than an accented form — ø, æ, ß, ł, đ, þ, ð — so those are transliterated
    explicitly. Without it "Bodø/Glimt" normalized to "bod glimt" (the ø became
    a space) and could never match Kalshi's "Bodoe/Glimt".
    """
    s = unicodedata.normalize("NFKD", s)
    s = "".join(ch for ch in s if not unicodedata.combining(ch))
    s = s.lower()
    for src, dst in TRANSLITERATE.items():
        if src in s:
            s = s.replace(src, dst)
    s = re.sub(r"[^a-z0-9 ]+", " ", s)
    s = re.sub(r"\s+", " ", s).strip()
    return s

def strip_percent_lines(raw_block: str) -> List[Tuple[str, Optional[str]]]:
    # Remove standalone percentage lines like "72%" or "98.8%" (entire line is just percentage)
    # Keeps lines like "72% Cowboys beat Commanders..." (percentage at start of content)
    # Also handles format with answer IDs: "72% | abc123 | Cowboys beat Commanders..."
    # Returns list of (line_text, answer_id_or_None) tuples
    lines = [ln.strip() for ln in raw_block.splitlines() if ln.strip()]
    cleaned: List[Tuple[str, Optional[str]]] = []

    for ln in lines:
        # Skip standalone percentage lines
        if re.fullmatch(r"\d+(?:\.\d+)?%", ln):
            continue

        # Extract answer ID from pattern: "72% | abc123 | Rest of line"
        if re.match(r'^\d+(?:\.\d+)?%\s*\|\s*[A-Za-z0-9]+\s*\|\s*', ln):
            match = re.match(r'^(\d+(?:\.\d+)?%)\s*\|\s*([A-Za-z0-9]+)\s*\|\s*(.+)$', ln)
            if match:
                answer_id = match.group(2)
                text = match.group(3)
                cleaned.append((text, answer_id))
                continue

        cleaned.append((ln, None))

    return cleaned

def datecode_from_mmdd(month: int, day: int, year_two_digits: str) -> str:
    return f"{year_two_digits}{MON[month]}{day:02d}"

def datecode_from_line_mmdd(line: str, year_two_digits: str) -> str:
    m = re.search(r"\b(\d{1,2})/(\d{1,2})\b", line)
    if not m:
        raise ValueError(f"Could not parse date (MM/DD) from: {line}")
    return datecode_from_mmdd(int(m.group(1)), int(m.group(2)), year_two_digits)

def kalshi_get_all_markets(series_ticker: str, limit: int = 1000) -> List[dict]:
    out: List[dict] = []
    cursor: Optional[str] = None
    while True:
        params = {"series_ticker": series_ticker, "limit": limit}
        if cursor:
            params["cursor"] = cursor
        r = requests.get(f"{KALSHI_API_BASE}/markets", params=params, timeout=30)
        r.raise_for_status()
        data = r.json()
        out.extend(data.get("markets", []))
        cursor = data.get("cursor")
        if not cursor:
            break
    return out

def build_event_index(markets: List[dict]) -> Dict[str, List[dict]]:
    idx: Dict[str, List[dict]] = {}
    for m in markets:
        ev = m.get("event_ticker")
        if ev:
            idx.setdefault(ev, []).append(m)
    return idx

def kalshi_slug_from_ticker(ticker: str) -> str:
    # Drop trailing outcome suffix
    parts = ticker.split("-")
    if len(parts) < 3:
        raise ValueError(f"Unexpected ticker format: {ticker}")
    return "-".join(parts[:-1]).lower()

def extract_suffix_code(ticker: str) -> str:
    parts = ticker.split("-")
    if len(parts) < 2:
        raise ValueError(f"Unexpected ticker format: {ticker}")
    return parts[-1]

def unique_event_codes(event_markets: List[dict]) -> List[str]:
    codes: List[str] = []
    for m in event_markets:
        t = m.get("ticker")
        if isinstance(t, str) and "-" in t:
            c = extract_suffix_code(t)
            if c not in codes:
                codes.append(c)
    
    # Sort to put TIE last if present (for 3-outcome markets like EPL)
    if 'TIE' in codes:
        codes.remove('TIE')
        codes.append('TIE')
    
    return codes

def parse_title_participants(title: str) -> Optional[Tuple[str, str]]:
    # Common: "Dallas vs Detroit Winner?"
    t = title.strip()
    t = re.sub(r"\bWinner\?\s*$", "", t, flags=re.IGNORECASE).strip()
    parts = re.split(r"\s+vs\s+", t, flags=re.IGNORECASE)
    if len(parts) == 2:
        return parts[0].strip(), parts[1].strip()

    # Sometimes: "Dallas at Washington" or similar
    parts = re.split(r"\s+at\s+", t, flags=re.IGNORECASE)
    if len(parts) == 2:
        return parts[0].strip(), parts[1].strip()

    return None

def extract_datecode_from_ticker(ticker: str, series: str) -> Optional[str]:
    """Extract the datecode (e.g. '26MAR19') from a full Kalshi ticker like 'KXNCAAMBGAME-26MAR19TCUOSU-TCU'."""
    m = re.search(rf"^{re.escape(series)}-(\d{{2}}[A-Z]{{3}}\d{{2}})", ticker)
    if not m:
        return None
    return m.group(1)

def ticker_matchup_chunk(ticker: str, series: str, datecode: str) -> Optional[str]:
    # Between datecode and trailing "-CODE", no length assumptions.
    m = re.search(rf"^{re.escape(series)}-{re.escape(datecode)}(.+?)-", ticker)
    if not m:
        return None
    return m.group(1)

def left_right_codes_from_matchup_chunk(
    event_markets: List[dict],
    series: str,
    datecode: str,
    codes: List[str],
) -> Optional[Tuple[str, str]]:
    if len(codes) < 2:
        return None
    
    # Filter out TIE code for 3-outcome markets (EPL)
    team_codes = [c for c in codes if c != 'TIE']
    if len(team_codes) < 2:
        return None
    
    a, b = team_codes[0], team_codes[1]
    for m in event_markets:
        t = m.get("ticker")
        if not isinstance(t, str):
            continue
        chunk = ticker_matchup_chunk(t, series, datecode)
        if not chunk:
            continue
        ia, ib = chunk.find(a), chunk.find(b)
        if ia != -1 and ib != -1 and ia != ib:
            return (a, b) if ia < ib else (b, a)
    return None

def participant_matches(participant: str, opts: List[str]) -> bool:
    pn = norm(participant)
    for o in opts:
        on = norm(o)
        if on == pn or (on and (on in pn or pn in on)):
            return True
    return False

# -----------------------------
# Input parsers
# -----------------------------
def parse_nba_win_at(line: str, year_two_digits: str) -> Tuple[str, str, str]:
    # "... 12/23 ... (HOU) Rockets WIN at (LAC) Clippers"
    # Also handles: "38% Thursday 12/25, 10:30 PM: (MIN) Timberwolves WIN at (DEN) Nuggets"
    # and the weekly megamarkets' "Celtics beat Pistons (10/20)" (see parse_beat_line).
    m = re.search(r"\)\s*(.+?)\s+WIN\s+at\s+\([^)]+\)\s*(.+?)\s*$", line, flags=re.IGNORECASE)
    if not m:
        return parse_beat_line(line, year_two_digits, "NBA")
    datecode = datecode_from_line_mmdd(line, year_two_digits)
    away_nick = m.group(1).strip()
    home_nick = m.group(2).strip()
    return datecode, away_nick, home_nick

def parse_nhl_win_at(line: str, year_two_digits: str) -> Tuple[str, str, str]:
    # "... 12/23 ... DAL Stars WIN at DET Red Wings"
    # Also handles: "80% Tuesday 12/23, 10:00 PM: SEA Kraken WIN at LA Kings"
    # and the weekly megamarkets' "Ducks beat Rangers (10/19)" (see parse_beat_line).
    m = re.search(r":\s*[A-Z]{2,4}\s+(.+?)\s+WIN\s+at\s+[A-Z]{2,4}\s+(.+?)\s*$", line, flags=re.IGNORECASE)
    if not m:
        return parse_beat_line(line, year_two_digits, "NHL")
    datecode = datecode_from_line_mmdd(line, year_two_digits)
    away_nick = m.group(1).strip()
    home_nick = m.group(2).strip()
    return datecode, away_nick, home_nick

def parse_mlb_win_at(line: str, year_two_digits: str) -> Tuple[str, str, str]:
    # "Wednesday 4/22, 1:10 PM: HOU Astros WIN at CLE Guardians"
    # "Saturday 9/12, 1:10 PM: COL Rockies at DET Tigers"   <- no WIN, 9/12/26 on
    # Structure mirrors NHL: "<ABBR> <Nick> [WIN] at <ABBR> <Nick>" after a colon.
    # The WIN is OPTIONAL. The creator's generator started omitting it mid-season
    # and every answer for the day failed to parse at once. It was only ever
    # decoration: " at " is the separator and the ORDER carries the meaning — YES
    # is the first-named (away) team. Checked against Kalshi's away leg on the
    # first no-WIN slate: 7 of the 8 traded games agreed within 2c, while reading
    # it as the home team would have been off by 0.24 on average.
    # BOTH abbreviations are optional: the Athletics have no city prefix, so they
    # read "Athletics at SEA Mariners" away and "... at Athletics" home.
    # (?-i:) keeps the abbr uppercase-only despite the IGNORECASE on " at " —
    # under IGNORECASE, [A-Z]{2,4} also matches lowercase, so "BOS Red Sox" would
    # let the optional group swallow "Red" and leave the nick as "Sox".
    datecode = datecode_from_line_mmdd(line, year_two_digits)
    # ":\s+" (colon-SPACE), not ":\s*": with the leading abbr now optional there is
    # nothing left to stop the match anchoring on the colon inside the clock time,
    # and "9:40 PM: Athletics at ..." would yield the nick "40 PM: Athletics".
    m = re.search(r":\s+(?-i:[A-Z]{2,4}\s+)?(.+?)\s+at\s+(?-i:[A-Z]{2,4}\s+)?(.+?)\s*$", line, flags=re.IGNORECASE)
    if not m:
        raise ValueError(f"Could not parse MLB teams from: {line}")
    # The away group is lazy and stops at " at ", so it still carries whatever WIN
    # was attached: spaced ("Rays WIN"), glued ("RaysWIN") or glued behind a stray
    # letter ("AstrosmWIN") — all three occur in this market, and the last two never
    # parsed under the old \s+WIN\s+ form at all. Strip it case-SENSITIVELY so a
    # nickname that merely ends in the letters "win" survives untouched. The stray
    # letter is pinned to "m" rather than [a-z]: no MLB nickname ends in m, so it
    # cannot eat a real one, whereas [a-z]? turns "RaysWIN" into "Ray".
    away_nick = re.sub(r"\s*m?WIN$", "", m.group(1).strip()).strip()
    home_nick = m.group(2).strip()
    if not away_nick or not home_nick:
        raise ValueError(f"Could not parse MLB teams from: {line}")
    return datecode, away_nick, home_nick

def parse_beat_line(line: str, year_two_digits: str, label: str = "NFL") -> Tuple[str, str, str]:
    """'<A> beat <B> (M/D)' -> (datecode, A, B). A is the YES side.

    The weekly megamarkets use this for every league (NFL, NBA and NHL, e.g.
    "(Week 1) NBA 2026-27 Mega-Market") with bare
    nicknames, which the per-sport nick_to_kalshi_participant maps resolve to
    Kalshi's city names. Before the NBA/NHL parsers fell back to this, /nba and
    /nhl matched nothing on those markets and /fixtures needed a rewrite per team.
    """
    m = re.search(r"^\s*(?:\d+(?:\.\d+)?%)?\s*(.+?)\s+beat\s+(.+?)\s*\((\d{1,2})/(\d{1,2})\)\s*", line, flags=re.IGNORECASE)
    if not m:
        raise ValueError(f"Could not parse {label} line: {line}")
    team_a = m.group(1).strip()
    team_b = m.group(2).strip()
    month = int(m.group(3))
    day = int(m.group(4))
    datecode = datecode_from_mmdd(month, day, year_two_digits)
    return datecode, team_a, team_b


def parse_nfl_beat(line: str, year_two_digits: str) -> Tuple[str, str, str]:
    # "Cowboys beat Commanders (12/25) 1:00 PM (Netflix)"
    # Also handles: "72% Cowboys beat Commanders (12/25) 1:00 PM (Netflix)"
    return parse_beat_line(line, year_two_digits, "NFL")

def parse_epl_win(line: str, year_two_digits: str) -> Tuple[str, str, str]:
    # Support multiple formats from Manifold:
    # Format 1: "Crystal Palace beat Fulham (Thu Jan 01, 17:30)"
    # Format 2: "50% Crystal Palace beat Fulham"
    # Format 3: Just team names with various separators
    
    # First try the explicit date format
    m = re.search(r"^\s*(?:\d+(?:\.\d+)?%)?\s*(.+?)\s+beat\s+(.+?)\s*\([^)]*([A-Z][a-z]{2})\s+(\d{1,2})[,\s]+", line, flags=re.IGNORECASE)
    if m:
        team_a = m.group(1).strip()
        team_b = m.group(2).strip()
        month_abbr = m.group(3).upper()
        day = int(m.group(4))
        
        # Convert month abbreviation to month number
        month_map = {v: k for k, v in MON.items()}
        if month_abbr not in month_map:
            raise ValueError(f"Unknown month abbreviation: {month_abbr}")
        month = month_map[month_abbr]
        
        datecode = datecode_from_mmdd(month, day, year_two_digits)
        return datecode, team_a, team_b
    
    # Format 2: Simple "Team1 beat Team2" (extract date from context or use default)
    m = re.search(r"^\s*(?:\d+(?:\.\d+)?%)?\s*(.+?)\s+beat\s+(.+?)(?:\s*\(|\s*$)", line, flags=re.IGNORECASE)
    if m:
        team_a = m.group(1).strip()
        team_b = m.group(2).strip()
        # Look for any date in the line
        date_m = re.search(r"(\d{1,2})/(\d{1,2})", line)
        if date_m:
            month = int(date_m.group(1))
            day = int(date_m.group(2))
            datecode = datecode_from_mmdd(month, day, year_two_digits)
            return datecode, team_a, team_b
        else:
            raise ValueError(f"Could not find date in EPL line: {line}")
    
    raise ValueError(f"Could not parse EPL line: {line}")

def parse_ncaa_vs(line: str) -> Tuple[str, str]:
    """Parse 'TeamA vs TeamB' or 'TeamA vs. TeamB' format (no date in text).
    Returns (team_a, team_b)."""
    m = re.split(r'\s+vs\.?\s+', line.strip(), flags=re.IGNORECASE)
    if len(m) == 2:
        return m[0].strip(), m[1].strip()
    raise ValueError(f"Could not parse NCAA teams from: {line}")

def parse_tennis_vs(line: str) -> Tuple[str, str]:
    """Parse a Manifold tennis answer line. Examples:
        '🇺🇦 Y. Starodubtseva vs 🇦🇺 A. Tomljanovic [Australian Open – Round 1]'
        '🇨🇦 F. Auger-Aliassime (7) vs 🇵🇹 N. Borges [Australian Open – Round 1]'
        '🇷🇺 D. Medvedev (11) vs 🇳🇱 J. de Jong [Australian Open – Round 1]'

    Returns (player_a_surname, player_b_surname). Surname here is "everything
    after the leading initial(s)" so multi-word names like 'de Jong' or
    'Camila Osorio Serrano' come through intact — token-overlap matching in
    pick_game_market handles the rest.
    """
    # Drop trailing tournament/round bracket
    s = re.sub(r"\s*\[[^\]]*\]\s*$", "", line).strip()
    # Split on ' vs ' (also tolerates 'vs.')
    parts = re.split(r"\s+vs\.?\s+", s, flags=re.IGNORECASE)
    if len(parts) != 2:
        raise ValueError(f"Could not split tennis line on 'vs': {line}")

    def clean_player(p: str) -> str:
        # Strip trailing seed marker like "(7)" or "(15)"
        p = re.sub(r"\s*\(\d+\)\s*$", "", p).strip()
        # Drop leading non-letter chars (flag emojis, stray whitespace, etc.)
        m = re.search(r"[A-Za-z].*", p)
        if not m:
            return ""
        p = m.group(0).strip()
        # Drop the leading initial(s): keep everything after the LAST period.
        # 'Y. Starodubtseva' -> 'Starodubtseva'
        # 'M. Camila Osorio Serrano' -> 'Camila Osorio Serrano'
        # 'Auger-Aliassime' (no period) -> unchanged
        if '.' in p:
            p = p.rsplit('.', 1)[1].strip()
        return p

    a = clean_player(parts[0])
    b = clean_player(parts[1])
    if not a or not b:
        raise ValueError(f"Could not extract player names from: {line}")
    return a, b

# -----------------------------
# Match by title tokens
# -----------------------------
# Kalshi changed the game-market title format around 2026-08-22. Both shapes are
# handled here because old events keep the old titles until they settle:
#
#   legacy (one title names the whole matchup, every leg shares it)
#     KXNFLGAME-...WASDAL-DAL  title "Washington vs Dallas Winner?"
#   current (one leg per outcome, each title names exactly ONE participant)
#     KXNFLGAME-...WASDAL-DAL  title "Dallas wins"   yes_sub_title "Dallas"
#     KXNFLGAME-...WASDAL-WAS  title "Washington wins" yes_sub_title "Washington"
#     KXEPLGAME-...NEWHUL-TIE  title "Tie is the result" yes_sub_title "Tie"
#
# Under the current format `yes_sub_title` is the clean participant name, so it
# is the matching key; `title` is only the legacy fallback.

# Kalshi qualifies soccer sub-titles with the period ("Reg Time: Arsenal").
SUBTITLE_QUALIFIER_RE = re.compile(r"^[^:]{0,20}:\s*")

TIE_NAMES = {"tie", "draw"}


def kalshi_team_name(market: dict) -> str:
    """The team a Kalshi leg pays out on, minus any 'Reg Time:' qualifier."""
    sub = market.get("yes_sub_title") or ""
    return SUBTITLE_QUALIFIER_RE.sub("", sub).strip()


def group_kalshi_games(markets: List[dict]) -> Dict[str, List[dict]]:
    """Group a series' markets into games, keyed by event_ticker."""
    games: Dict[str, List[dict]] = {}
    for m in markets:
        key = m.get("event_ticker") or (m.get("ticker") or "").rsplit("-", 1)[0]
        if key:
            games.setdefault(key, []).append(m)
    return games


def participant_opts_from_nick(nick: str, nick_map: Dict[str, List[str]]) -> List[str]:
    return nick_map.get(nick, [nick])

def participant_score(text: str, opts: List[str]) -> float:
    """How well `text` names the participant described by `opts`. 0.0 = not at all.

    Scored as `coverage + overlap` over whole tokens, where coverage is the
    fraction of the *option's* tokens present. Coverage is what separates teams
    that share a city: against "Los Angeles R", the Rams option "Los Angeles R"
    covers 3/3 (score 4.0) while the Chargers' best option "Los Angeles" covers
    2/2 but of a shorter name (3.0) — so the longer, fully-covered option wins.
    A plain token-intersection count cannot make that distinction, and the same
    tie exists for New York G/J/M/Y. The `+ overlap` term breaks ties between
    equally-covered options in favour of the more specific one.

    Partial coverage still scores (below any full match) so a sloppy alias like
    the NFL map's "San Francisco, 'SF" still matches Kalshi's "San Francisco".
    """
    tt = set(norm(text).split())
    if not tt:
        return 0.0
    best = 0.0
    for o in opts:
        ot = set(norm(o).split())
        if not ot:
            continue
        overlap = len(tt & ot)
        if not overlap:
            continue
        best = max(best, overlap / len(ot) + overlap)
    return best


# Floor for participant_score. 2.0 = one option fully covered (the weakest
# honest match: a one-token option matching exactly). Everything below is a
# partial match on a shared token, which is exactly how same-city teams collide:
# "Manchester City" vs Kalshi's "Hull City" scores 1.5, and the Cubs' option
# "Chicago C" against Kalshi's "Chicago WS" also 1.5. Both must be rejected, and
# both are the kind of pair that would otherwise generate a config that trades
# the wrong team.
MIN_PARTICIPANT_SCORE = 2.0


def assign_legs_to_participants(
    legs: List[dict], a_opts: List[str], b_opts: List[str],
) -> Optional[Tuple[float, dict, dict]]:
    """Best (score, leg_for_A, leg_for_B) assignment within one Kalshi event.

    Current-format only: each leg names one participant, so A and B must land on
    two DIFFERENT legs. That requirement is what the old `sa > 0 and sb > 0`
    title test used to provide — dropping it (the emergency `or` fix) makes any
    single-team title a match, so a team playing a different opponent that day
    scores just as well. Requiring both participants inside one event restores
    the guard at the level where it now belongs.
    """
    # yes_sub_title ONLY — never fall back to `title`. Under the legacy format
    # every leg of an event shares one title naming BOTH participants, so a title
    # fallback scores both legs identically for both participants and the
    # assignment below picks an arbitrary (often reversed) ordering. An event
    # without per-leg sub-titles is not in the current format: return None and
    # let the caller's legacy path handle it.
    cands = []
    names = set()
    for m in legs:
        name = kalshi_team_name(m)
        if not name or norm(name) in TIE_NAMES:
            continue
        if not isinstance(m.get("ticker"), str):
            continue
        names.add(norm(name))
        cands.append((m, participant_score(name, a_opts), participant_score(name, b_opts)))
    if len(names) < 2:
        return None

    best = None
    for i, (leg_a, score_a, _) in enumerate(cands):
        if score_a < MIN_PARTICIPANT_SCORE:
            continue
        for j, (leg_b, _, score_b) in enumerate(cands):
            if i == j or score_b < MIN_PARTICIPANT_SCORE:
                continue
            total = score_a + score_b
            if best is None or total > best[0]:
                best = (total, leg_a, leg_b)
    return best

def title_score(title: str, a_opts: List[str], b_opts: List[str]) -> int:
    """LEGACY title format only: one title names both participants
    ("Washington vs Dallas Winner?"), so both must appear. Current-format
    titles name one participant and always score 0 here — they are matched by
    `assign_legs_to_participants` instead."""
    tt = set(norm(title).split())
    best = 0
    for a in a_opts:
        for b in b_opts:
            at = set(norm(a).split())
            bt = set(norm(b).split())
            sa = len(tt & at)
            sb = len(tt & bt)
            if sa > 0 and sb > 0:
                best = max(best, sa + sb)
    return best

def pick_game_market(day_markets: List[dict], a_opts: List[str], b_opts: List[str]) -> Optional[dict]:
    """The Kalshi market representing this matchup — the leg paying out on
    participant A under the current format, the shared-title market under the
    legacy one. The caller only reads `event_ticker` and `ticker` off it (plus
    `title` for diagnostics), so either is a valid representative."""
    # Current format: score whole EVENTS, requiring both participants on
    # distinct legs, and return A's leg.
    best = None
    for legs in group_kalshi_games(day_markets).values():
        picked = assign_legs_to_participants(legs, a_opts, b_opts)
        if picked and (best is None or picked[0] > best[0]):
            best = picked
    if best is not None:
        return best[1]

    # Legacy format: the whole matchup lives in one title.
    fallback = None
    best_s = 0
    for m in day_markets:
        title = m.get("title") or ""
        s = title_score(title, a_opts, b_opts)
        if s > best_s:
            best_s = s
            fallback = m
    return fallback if best_s > 0 else None

# -----------------------------
# Resolve codes in requested order
# -----------------------------
def resolve_codes_in_input_order(
    series: str,
    datecode: str,
    event_markets: List[dict],
    matched_title: str,
    a_opts: List[str],
    b_opts: List[str],
) -> Optional[Tuple[str, str]]:
    """
    Returns (code_for_A, code_for_B) where A/B correspond to the input line ordering.
    Uses:
      - event suffix codes (Kalshi-derived)
      - matchup chunk order to get left/right codes
      - title participants to decide whether A is left or right
    """
    codes = unique_event_codes(event_markets)
    if len(codes) < 2:
        return None

    # Current format: each leg names one participant, so the leg IS the answer —
    # read the suffix codes straight off the two assigned tickers. Preferred over
    # the matchup-chunk path below because it never has to guess an ordering:
    # ticker chunks like "NYGNYJ" or "LACLAR" contain both codes as substrings of
    # each other's neighbourhood, and the title no longer says who is on the left.
    picked = assign_legs_to_participants(event_markets, a_opts, b_opts)
    if picked is not None:
        _score, leg_a, leg_b = picked
        code_a = extract_suffix_code(leg_a["ticker"])
        code_b = extract_suffix_code(leg_b["ticker"])
        # Distinct legs can still share a suffix only if the event is malformed;
        # refuse rather than emit a config that bets both sides of one leg.
        if code_a != code_b and code_a in codes and code_b in codes:
            return code_a, code_b
        return None

    # Legacy format: derive left/right from the ticker chunk, then use the
    # shared "<A> vs <B>" title to decide which side the input's A is on.
    lr = left_right_codes_from_matchup_chunk(event_markets, series, datecode, codes)
    if not lr:
        return None
    left_code, right_code = lr

    parts = parse_title_participants(matched_title)
    if not parts:
        return None
    left_part, right_part = parts

    a_is_left = participant_matches(left_part, a_opts)
    a_is_right = participant_matches(right_part, a_opts)
    b_is_left = participant_matches(left_part, b_opts)
    b_is_right = participant_matches(right_part, b_opts)

    if a_is_left and b_is_right:
        return left_code, right_code
    if a_is_right and b_is_left:
        return right_code, left_code

    # If one side is unambiguous, use it
    if a_is_left and not a_is_right:
        return left_code, right_code
    if a_is_right and not a_is_left:
        return right_code, left_code

    return None

# -----------------------------
# Manifold slug extraction
# -----------------------------
def _strip_url_noise(s: str) -> str:
    """Drop surrounding <>/whitespace, then any ?query or #fragment."""
    s = (s or "").strip().strip("<>").strip()
    return re.split(r"[?#]", s, maxsplit=1)[0].rstrip("/")


def extract_manifold_slug(url: str) -> str:
    """The slug from a Manifold URL (or a bare slug).

    Manifold's share button copies `.../slug?r=<referrer>`. The query used to be
    kept as part of the slug, and nothing noticed: /v0/slug/<slug>?r=... still
    resolves (the API ignores the query), so the pairs connected and traded
    under a slug like `week-6-nfl-...-game?r=Um95THVv` — and then /drop with the
    clean URL matched nothing. Everything after ? or # is dropped now.
    """
    # URL format: https://manifold.markets/USERNAME/slug-here
    return _strip_url_noise(url).split('/')[-1]

def extract_kalshi_event_ticker(url_or_ticker: str) -> str:
    """Extract the event ticker from a Kalshi URL or raw ticker string.

    Accepts formats:
      - Full URL: https://kalshi.com/markets/kxranklistsongspotusa/.../kxranklistsongspotusa-26apr01
      - Just the ticker: KXRANKLISTSONGSPOTUSA-26APR01
    Returns uppercased event ticker.
    """
    s = _strip_url_noise(url_or_ticker)
    # If it looks like a URL, grab the last path segment
    if '/' in s:
        s = s.split('/')[-1]
    return s.upper()

def kalshi_get_event_markets(event_ticker: str, limit: int = 200) -> List[dict]:
    """Fetch all markets under a Kalshi event ticker."""
    out: List[dict] = []
    cursor: Optional[str] = None
    while True:
        params: dict = {"event_ticker": event_ticker, "limit": limit}
        if cursor:
            params["cursor"] = cursor
        r = requests.get(f"{KALSHI_API_BASE}/markets", params=params, timeout=30)
        r.raise_for_status()
        data = r.json()
        out.extend(data.get("markets", []))
        cursor = data.get("cursor")
        if not cursor:
            break
    return out

def _tokens_contiguous_in(small: List[str], big: List[str]) -> bool:
    """True when `small` appears as a contiguous run of whole tokens in `big`."""
    if not small or len(small) > len(big):
        return False
    for i in range(len(big) - len(small) + 1):
        if big[i:i + len(small)] == small:
            return True
    return False


ALIASES_FILE = "kalshi_aliases.yaml"


def load_aliases(path: Optional[str] = None) -> Tuple[Dict[str, str], Dict[str, str]]:
    """Load (rewrites, pins) from the alias file. Keys are normalized.

    rewrites: Manifold answer text -> text to match with instead. Durable across
              events ("kimi" -> "Andrea Kimi Antonelli" is right at every race).
    pins:     Manifold answer text -> an exact Kalshi ticker. Event-specific, so
              use these only when a rewrite cannot express the fix.

    `path` defaults to the module-level ALIASES_FILE at *call* time, so
    reassigning that constant (tests, alternate deployments) actually takes
    effect — a `path=ALIASES_FILE` default would bind the original value.
    """
    path = path or ALIASES_FILE
    if not os.path.exists(path):
        return {}, {}
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    rewrites = {norm(k): str(v) for k, v in (data.get("rewrites") or {}).items()}
    pins = {norm(k): str(v) for k, v in (data.get("pins") or {}).items()}
    if rewrites or pins:
        print(f"# Loaded {len(rewrites)} rewrites, {len(pins)} pins from {path}")
    return rewrites, pins


def add_alias(manifold_text: str, value: str, kind: str = "rewrites",
              path: Optional[str] = None) -> None:
    """Add a rewrite or pin to the alias file, preserving the rest of it.

    Rewrites through yaml.safe_dump, so the explanatory comments in a
    hand-written alias file are lost on the first programmatic write — the
    header below is re-added so the file stays self-documenting.
    """
    if kind not in ("rewrites", "pins"):
        raise ValueError(f"kind must be rewrites or pins, got {kind!r}")

    path = path or ALIASES_FILE  # resolved at call time — see load_aliases
    data = {}
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
    data.setdefault("rewrites", {})
    data.setdefault("pins", {})
    # Store under the normalized key so lookups hit regardless of how the text
    # was typed; load_aliases() normalizes again, which is idempotent.
    data[kind][norm(manifold_text)] = value

    header = (
        "# Manual overrides for Kalshi <-> Manifold name matching.\n"
        "# rewrites: Manifold answer text -> text to match with instead (durable).\n"
        "# pins:     Manifold answer text -> exact Kalshi ticker (event-specific).\n"
        "# Keys are normalized (lowercased, accents folded, punctuation dropped).\n"
        "# Written by control_bot.py /fix and /rewrite; safe to edit by hand.\n"
    )
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(header)
        yaml.safe_dump(data, f, default_flow_style=False, sort_keys=True,
                       allow_unicode=True)
    os.replace(tmp, path)


class MatchResult:
    """Outcome of matching one Manifold answer against a Kalshi event's markets.

    `ties` holds the other markets that scored identically. A tie is never safe
    to auto-accept: the old code returned whichever tied market came first in
    API order, so a bare 'Kimi' silently resolved to Andrea Kimi Antonelli or
    Kimi Raikkonen depending on list order, with no signal that a coin was
    flipped.
    """
    __slots__ = ("market", "score", "method", "ties", "runner_up")

    def __init__(self, market=None, score=0.0, method="none", ties=None, runner_up=None):
        self.market = market
        self.score = score
        self.method = method
        self.ties = ties or []
        self.runner_up = runner_up

    @property
    def ok(self) -> bool:
        return self.market is not None and not self.ties

    def describe(self, manifold_text: str) -> str:
        if self.market is None:
            # A 0.00 "best" candidate is just the first market in API order —
            # showing it as a near-miss is noise.
            if self.runner_up is not None and self.score > 0:
                return (f"no match (closest {self.score:.2f}: "
                        f"{self.runner_up.get('yes_sub_title')})")
            return "no match"
        sub = self.market.get("yes_sub_title")
        base = f"{sub} [{self.method} {self.score:.2f}]"
        if self.ties:
            others = ", ".join(str(m.get("yes_sub_title")) for m in self.ties)
            return f"{base} AMBIGUOUS — ties with: {others}"
        return base


def match_kalshi_option_detailed(
    manifold_text: str,
    kalshi_markets: List[dict],
    rewrites: Dict[str, str] = None,
    pins: Dict[str, str] = None,
) -> MatchResult:
    """Match a Manifold answer against Kalshi markets by yes_sub_title.

    Order: pin (exact ticker) -> rewrite -> exact normalized -> token coverage.
    Ties at the top score are reported rather than silently resolved.
    """
    query_raw = norm(manifold_text)
    pins = pins or {}
    rewrites = rewrites or {}

    if query_raw in pins:
        want = pins[query_raw]
        for m in kalshi_markets:
            if m.get("ticker") == want:
                return MatchResult(m, 1.0, "pinned")
        print(f"#   ⚠ pin for {manifold_text!r} -> {want} not in this event's markets")

    query = norm(rewrites[query_raw]) if query_raw in rewrites else query_raw
    method_prefix = "rewritten+" if query != query_raw else ""
    query_tokens = set(query.split())

    scored: List[Tuple[float, dict]] = []
    for m in kalshi_markets:
        sub = m.get("yes_sub_title") or ""
        if not sub:
            continue
        sub_norm = norm(sub)
        sub_tokens = set(sub_norm.split())

        if query == sub_norm:
            return MatchResult(m, 1.0, method_prefix + "exact")

        # Coverage of the shorter name's tokens by the longer one.
        overlap = len(query_tokens & sub_tokens)
        denom = min(len(query_tokens), len(sub_tokens)) or 1
        coverage = overlap / denom

        # Containment (handles multi-word names embedded verbatim), on TOKEN
        # boundaries. A raw substring test matches across word boundaries: the
        # Manifold answer "N/a" normalizes to "n a", which is a substring of
        # "fridoli|n a|mbongo" and scored 0.95 against that market (observed
        # live, 2026-07-29 — it collided with the real Ambongo answer).
        q_seq, s_seq = query.split(), sub_norm.split()
        if _tokens_contiguous_in(q_seq, s_seq) or _tokens_contiguous_in(s_seq, q_seq):
            coverage = max(coverage, 0.95)

        scored.append((coverage, m))

    if not scored:
        return MatchResult()

    scored.sort(key=lambda t: -t[0])
    best_score, best_market = scored[0]

    if best_score < 0.8:
        return MatchResult(None, best_score, "none", runner_up=best_market)

    ties = [m for score, m in scored[1:] if abs(score - best_score) < 1e-9]
    return MatchResult(best_market, best_score, method_prefix + "coverage", ties=ties)


def match_kalshi_option(manifold_text: str, kalshi_markets: List[dict]) -> Optional[dict]:
    """Backwards-compatible wrapper: best match or None, ignoring ambiguity."""
    return match_kalshi_option_detailed(manifold_text, kalshi_markets).market

def generate_generic_configs(
    kalshi_url_or_ticker: str,
    manifold_slug: str,
    raw_manifold_inputs: str,
    diagnostics: Optional[dict] = None,
) -> List[dict]:
    """Generate config entries for arbitrary multi-choice Kalshi events.

    This is a generalized version that works for any Kalshi event (not just sports).
    It matches Manifold answer text against Kalshi yes_sub_title.

    Args:
        kalshi_url_or_ticker: Kalshi event URL or event ticker
            e.g. "https://kalshi.com/markets/kxranklistsongspotusa/.../kxranklistsongspotusa-26apr01"
            or   "KXRANKLISTSONGSPOTUSA-26APR01"
        manifold_slug: Manifold market slug
            e.g. "spotify-usa-1-song-in-march-2026"
        raw_manifold_inputs: Manifold answer lines, one per line.
            Supports formats:
              "89% | ncsOEzdqAg | Zara Larsson"  (with inline answer ID — preferred)
              "Zara Larsson"                      (plain text — needs Manifold API for answer ID)

    Returns list of config dicts with name, kalshi_ticker, manifold_slug, manifold_answer_id.
    """
    event_ticker = extract_kalshi_event_ticker(kalshi_url_or_ticker)
    print(f"# Fetching Kalshi markets for event: {event_ticker}")

    kalshi_markets = kalshi_get_event_markets(event_ticker)
    # Filter to active markets only
    active_markets = [m for m in kalshi_markets if m.get("status") != "finalized"]
    print(f"# Found {len(kalshi_markets)} total markets, {len(active_markets)} active")

    lines = strip_percent_lines(raw_manifold_inputs)
    if not lines:
        print("# WARNING: No input lines found")
        return []

    # If any lines lack inline answer IDs, fetch from Manifold API
    has_inline_ids = all(aid is not None for _, aid in lines)
    manifold_answers: List[dict] = []
    if not has_inline_ids:
        print(f"# Fetching Manifold answers for slug: {manifold_slug}")
        manifold_answers = fetch_manifold_answers(manifold_slug)
        if not manifold_answers:
            print(f"# WARNING: No answers found for Manifold market '{manifold_slug}'.")

    rewrites, pins = load_aliases()

    job_specs: List[dict] = []
    misses: List[str] = []
    ambiguous: List[str] = []
    # Structured mirror of the printed report, for callers that need to act on
    # the unresolved entries (control_bot.py's /fix numbering).
    if diagnostics is not None:
        diagnostics.setdefault("matched", [])
        diagnostics.setdefault("ambiguous", [])
        diagnostics.setdefault("misses", [])
        diagnostics["options"] = [m.get("yes_sub_title", "") for m in active_markets]

    for line_text, inline_answer_id in lines:
        # Match against Kalshi markets
        result = match_kalshi_option_detailed(line_text, active_markets, rewrites, pins)
        if result.market is None:
            misses.append(
                f"No Kalshi match for: {line_text}\n"
                f"#   {result.describe(line_text)}\n"
                f"#   Available options: {[m.get('yes_sub_title', '') for m in active_markets[:10]]}..."
            )
            if diagnostics is not None:
                diagnostics["misses"].append({
                    "text": line_text,
                    "detail": result.describe(line_text),
                })
            continue
        if result.ties:
            # Never guess between equal scores — one of them trades the wrong
            # market. Add a rewrite or pin to kalshi_aliases.yaml to break it.
            ambiguous.append(f"{line_text} -> {result.describe(line_text)}")
            if diagnostics is not None:
                diagnostics["ambiguous"].append({
                    "text": line_text,
                    "detail": result.describe(line_text),
                    "candidates": [result.market.get("ticker")] +
                                  [m.get("ticker") for m in result.ties],
                })
            continue

        matched = result.market
        kalshi_ticker = matched["ticker"]
        option_name = matched.get("yes_sub_title") or line_text
        if result.method != "exact":
            print(f"#   {line_text} -> {result.describe(line_text)}")

        # Resolve Manifold answer ID
        answer_id = inline_answer_id
        if not answer_id:
            # Try matching by name against Manifold answers
            query_norm = norm(line_text)
            for ans in manifold_answers:
                ans_text_norm = norm(ans.get("text", ""))
                if query_norm == ans_text_norm or query_norm in ans_text_norm or ans_text_norm in query_norm:
                    answer_id = ans.get("id")
                    break
        if not answer_id:
            misses.append(f"No Manifold answer ID for: {line_text} (matched Kalshi: {kalshi_ticker})")
            continue

        job_specs.append({
            "name": option_name,
            "kalshi_ticker": kalshi_ticker,
            "manifold_slug": manifold_slug,
            "manifold_answer_id": answer_id,
        })
        if diagnostics is not None:
            diagnostics["matched"].append({
                "text": line_text,
                "detail": result.describe(line_text),
                "ticker": kalshi_ticker,
            })

    if misses:
        print("\n# --- MISSES ---")
        for m in misses:
            print("#", m)

    if ambiguous:
        print("\n# --- AMBIGUOUS (withheld — resolve in kalshi_aliases.yaml) ---")
        for a in ambiguous:
            print("#", a)
        print(f"# Add a `rewrites:` entry (durable) or `pins:` entry (this event only) "
              f"to {ALIASES_FILE}, then re-run.")

    print(f"\n# Matched {len(job_specs)}/{len(lines)} options successfully"
          + (f" ({len(ambiguous)} ambiguous, {len(misses)} missed)" if (ambiguous or misses) else ""))
    return job_specs

def fetch_manifold_answers(slug: str) -> List[dict]:
    """Fetch answers from a Manifold multi-choice market"""
    response = requests.get(f"https://api.manifold.markets/v0/slug/{slug}", timeout=30)
    response.raise_for_status()
    market = response.json()
    return market.get('answers', [])

def fetch_manifold_question(slug: str) -> Optional[str]:
    """The market's question text — needed to recover the subject team of an
    opponent-only fixture market ("Which games will Georgia win?")."""
    try:
        r = requests.get(f"https://api.manifold.markets/v0/slug/{slug}", timeout=30)
        r.raise_for_status()
        return r.json().get("question")
    except Exception as e:
        print(f"# WARNING: could not fetch question for {slug}: {e}")
        return None

def datecode_to_mmdd(datecode: str) -> str:
    """Convert datecode like '26MAR05' to 'M/D' format like '3/5' for matching Manifold answer text"""
    # datecode format: YYMONDD e.g. 26MAR05
    month_abbr = datecode[2:5]
    day = int(datecode[5:])
    month_map = {v: k for k, v in MON.items()}
    month = month_map.get(month_abbr)
    if month is None:
        return ""
    return f"{month}/{day}"

def match_manifold_answer(answers: List[dict], a_nick: str, b_nick: str, datecode: str) -> Optional[str]:
    """Find the Manifold answer ID that matches both team nicknames and date"""
    a_lower = a_nick.lower()
    b_lower = b_nick.lower()
    date_str = datecode_to_mmdd(datecode)

    for answer in answers:
        text = answer.get('text', '').lower()
        if a_lower in text and b_lower in text and date_str in text:
            return answer.get('id')

    return None

# -----------------------------
# Main generator
# -----------------------------
def generate_job_specs(raw_block: str, sport: str, year_two_digits: str = "25",
                       game_date: str = None, manifold_slug_override: str = None) -> List[dict]:
    """Generate config specs for a sport.

    Args:
        game_date: "MM/DD" string for sports where the date is NOT in the input
                   text (e.g. NCAA). Converted to a datecode and used for all lines.
        manifold_slug_override: Override the manifold slug from SPORTS config.
    """
    if sport not in SPORTS:
        raise ValueError(f"Unsupported sport '{sport}'. Choose from: {list(SPORTS.keys())}")

    cfg = SPORTS[sport]
    series = cfg["series_ticker"]
    ui_base = cfg["ui_base"]
    manifold_url = cfg["manifold_url"]
    manifold_slug = extract_manifold_slug(manifold_slug_override or manifold_url)
    nick_map = cfg["nick_to_kalshi_participant"]
    parser = cfg["parser"]

    # Pre-compute a fixed datecode when game_date is provided (e.g. NCAA)
    fixed_datecode = None
    if game_date:
        gm = re.match(r'^(\d{1,2})/(\d{1,2})$', game_date)
        if not gm:
            raise ValueError(f"game_date must be MM/DD format, got: {game_date}")
        fixed_datecode = datecode_from_mmdd(int(gm.group(1)), int(gm.group(2)), year_two_digits)

    lines = strip_percent_lines(raw_block)

    # Fetch Manifold answers for matching answer IDs (only needed if input lacks them)
    has_inline_ids = any(aid is not None for _, aid in lines)
    manifold_answers: List[dict] = []
    if not has_inline_ids:
        manifold_answers = fetch_manifold_answers(manifold_slug)
        if not manifold_answers:
            print(f"# WARNING: No answers found for Manifold market '{manifold_slug}'.")

    markets = kalshi_get_all_markets(series, limit=1000)
    event_idx = build_event_index(markets)

    job_specs: List[dict] = []
    misses: List[str] = []

    for idx, (line, inline_answer_id) in enumerate(lines, 1):
        try:
            if parser == "nba_win_at":
                datecode, a_nick, b_nick = parse_nba_win_at(line, year_two_digits)
                # For NBA/NHL this is AWAY, HOME
                label_a, label_b = "away", "home"
            elif parser == "nhl_win_at":
                datecode, a_nick, b_nick = parse_nhl_win_at(line, year_two_digits)
                label_a, label_b = "away", "home"
            elif parser == "mlb_win_at":
                datecode, a_nick, b_nick = parse_mlb_win_at(line, year_two_digits)
                label_a, label_b = "away", "home"
            elif parser == "nfl_beat":
                datecode, a_nick, b_nick = parse_nfl_beat(line, year_two_digits)
                # For NFL this is TeamA, TeamB in input order
                label_a, label_b = "teamA", "teamB"
            elif parser == "epl_win":
                datecode, a_nick, b_nick = parse_epl_win(line, year_two_digits)
                label_a, label_b = "teamA", "teamB"
            elif parser == "ncaa_vs":
                a_nick, b_nick = parse_ncaa_vs(line)
                datecode = fixed_datecode  # May be None — handled below
                label_a, label_b = "teamA", "teamB"
            elif parser == "tennis_vs":
                a_nick, b_nick = parse_tennis_vs(line)
                datecode = fixed_datecode  # Usually None for tennis
                label_a, label_b = "playerA", "playerB"
            else:
                raise ValueError(f"Unknown parser: {parser}")

        except ValueError as e:
            misses.append(str(e))
            continue

        a_opts = participant_opts_from_nick(a_nick, nick_map)
        b_opts = participant_opts_from_nick(b_nick, nick_map)

        if datecode:
            day_markets = [
                m for m in markets
                if isinstance(m.get("ticker"), str) and f"-{datecode}" in m["ticker"]
                and isinstance(m.get("title"), str)
            ]
        else:
            # No date filter — search all markets (e.g. NCAA tournament spanning multiple days)
            day_markets = [
                m for m in markets
                if isinstance(m.get("ticker"), str) and isinstance(m.get("title"), str)
            ]
            # For tennis, the same two players can play across different
            # tournaments, so finalized matches from earlier in the year would
            # otherwise compete for the title-token match. Restrict to markets
            # that are still open / awaiting settlement.
            if parser == "tennis_vs":
                day_markets = [
                    m for m in day_markets
                    if m.get("status") in ("active", "initialized")
                ]
        if not day_markets:
            misses.append(f"No {series} markets with datecode {datecode or 'ANY'} for: {line}")
            continue

        rep = pick_game_market(day_markets, a_opts, b_opts)
        if not rep:
            sample_titles = [m.get("title", "") for m in day_markets]
            misses.append(
                f"No title match for {a_nick} vs {b_nick} on {datecode}: {line}\n"
                f"# sample titles that day: {sample_titles}"
            )
            continue

        ev = rep.get("event_ticker")
        if not ev or ev not in event_idx:
            misses.append(f"Missing/unknown event_ticker for matched market {rep.get('ticker')}: {line}")
            continue

        # If no fixed datecode, extract it from the matched market's ticker
        effective_datecode = datecode
        if not effective_datecode:
            effective_datecode = extract_datecode_from_ticker(rep.get("ticker", ""), series)
            if not effective_datecode:
                misses.append(f"Could not extract datecode from ticker {rep.get('ticker')}: {line}")
                continue

        ev_markets = event_idx[ev]
        codes = resolve_codes_in_input_order(
            series=series,
            datecode=effective_datecode,
            event_markets=ev_markets,
            matched_title=rep.get("title", ""),
            a_opts=a_opts,
            b_opts=b_opts,
        )
        if not codes:
            misses.append(
                f"Could not map codes for {a_nick} vs {b_nick} ({line})\n"
                f"# matched title: {rep.get('title')}\n"
                f"# event codes: {unique_event_codes(ev_markets)}"
            )
            continue

        code_a, code_b = codes

        # Build the full ticker for team A (first team in the matchup)
        base_ticker = f"{series}-{effective_datecode}"
        # Get matchup chunk from one of the event markets
        matchup_chunk = None
        for m in ev_markets:
            t = m.get("ticker")
            if isinstance(t, str):
                chunk = ticker_matchup_chunk(t, series, effective_datecode)
                if chunk:
                    matchup_chunk = chunk
                    break
        
        if not matchup_chunk:
            misses.append(f"Could not extract matchup chunk for {line}")
            continue

        kalshi_ticker = f"{base_ticker}{matchup_chunk}-{code_a}"

        # Use inline answer ID if provided, otherwise match via API
        answer_id = inline_answer_id
        if not answer_id:
            if parser == "tennis_vs":
                # Tennis lines have no date; surnames alone disambiguate.
                a_lc, b_lc = a_nick.lower(), b_nick.lower()
                for ans in manifold_answers:
                    text_lc = (ans.get("text") or "").lower()
                    if a_lc in text_lc and b_lc in text_lc:
                        answer_id = ans.get("id")
                        break
            else:
                answer_id = match_manifold_answer(manifold_answers, a_nick, b_nick, effective_datecode)
        if not answer_id:
            misses.append(
                f"Could not match Manifold answer for {a_nick} vs {b_nick}: {line}"
            )
            continue

        # Create YAML config entry
        game_name = f"{sport.upper()} game {idx}"
        job_specs.append({
            "name": game_name,
            "kalshi_ticker": kalshi_ticker,
            "manifold_slug": manifold_slug,
            "manifold_answer_id": answer_id,
        })

    if misses:
        print("\n# --- MISSES ---")
        for m in misses:
            print("#", m)

    return job_specs

# -----------------------------
# Output YAML
# -----------------------------
def output_yaml(specs: List[dict], sport: str):
    """Output specs in YAML format"""
    print(f"\n# {sport.upper()} Config")
    print("market_pairs:")
    yaml_str = yaml.dump(specs, default_flow_style=False, sort_keys=False, indent=2)
    # Indent the entire block by 2 spaces for proper YAML nesting
    for line in yaml_str.split('\n'):
        if line.strip():
            print(f"  {line}")

# -----------------------------
# Legacy RAW-block flow (kept for the paste-into-source workflow)
# -----------------------------
def cmd_legacy(args=None):
    """The original hand-edited flow: paste answers into the RAW_* strings below.

    Prefer the `sport` / `generic` subcommands — they fetch the answers from the
    Manifold API instead, so nothing has to be pasted into this file.
    """
    # USAGE: Copy question text directly from Manifold Markets into RAW_* strings below
    # 
    # The parsers handle multiple formats:
    # 1. With standalone percentage lines (Manifold default export):
    #    72%
    #    Cowboys beat Commanders (12/25) 1:00 PM (Netflix)
    #
    # 2. With inline percentages (copy from browser):
    #    72% Cowboys beat Commanders (12/25) 1:00 PM (Netflix)
    #
    # 3. Without percentages (manual cleanup):
    #    Cowboys beat Commanders (12/25) 1:00 PM (Netflix)
    #
    # All three formats work! Just paste directly from Manifold.
    
    RAW_NFL = r"""
"""
    specs = generate_job_specs(RAW_NFL, sport="nfl", year_two_digits="26")
    output_yaml(specs, "nfl")

    RAW_NBA = r"""

"""
    specs = generate_job_specs(RAW_NBA, sport="nba", year_two_digits="26")
    output_yaml(specs, "nba")

    RAW_NHL = r"""

"""
    specs = generate_job_specs(RAW_NHL, sport="nhl", year_two_digits="26")
    output_yaml(specs, "nhl")

    RAW_EPL = r"""
"""
    specs = generate_job_specs(RAW_EPL, sport="premier_league", year_two_digits="26")
    output_yaml(specs, "premier_league")

    # MLB
    # Input format (from the major-league-baseball-live market):
    #   "99% Wednesday 4/22, 1:10 PM: HOU Astros WIN at CLE Guardians"
    RAW_MLB = r"""
50% | ZOOql2EqdP | Wednesday 7/29, 12:10 PM: PHI Phillies WIN at MIA Marlins
50% | 29zggIqqhR | Wednesday 7/29, 12:35 PM: ARZ Diamondbacks WIN at PIT Pirates
50% | UI6nt26CU2 | Wednesday 7/29, 1:05 PM: TOR Blue Jays WIN at WSH Nationals
50% | uIcShAIPS8 | Wednesday 7/29, 1:10 PM: BAL Orioles WIN at DET Tigers
50% | NcI9Udz2ly | Wednesday 7/29, 3:45 PM: MIL Brewers WIN at SF Giants
50% | Q5lyAhuIZQ | Wednesday 7/29, 4:10:PM: COL Rockies WIN at SD Padres
46% | 2qnAZh2IqI | Wednesday 7/29, 6:40 PM: TEX Rangers  WIN at TB Rays
50% | gSNqSR8htO | Wednesday 7/29, 7:10 PM: CLE Guardians  WIN at CIN Reds
33% | nOp5Ey62gN | Wednesday 7/29, 7:40 PM: KC Royals WIN at MIN Twins
53% | L98yyhlZLA | Wednesday 7/29, 7:40 PM: NYY Yankees WIN at CWS White Sox
53% | z0N8npnEgS | Wednesday 7/29, 7:45 PM: CHC Cubs WIN at STL Cardinals
53% | Ul5uL2RzN8 | Wednesday 7/29, 9:38 PM: HOU Astros WIN at LAA Angels
50% | C2Z56QhqCR | Wednesday 7/29, 9:40 PM: BOS Red Sox WIN at Athletics
40% | ZZq8gz5Lnl | Wednesday 7/29, 10:10 PM: SEA Mariners WIN at LAD Dodgers
"""
    specs = generate_job_specs(RAW_MLB, sport="mlb", year_two_digits="26")
    output_yaml(specs, "mlb")

    # ATP Tennis (men's)
    # Input format (from the 2026-atp-tour-mens-tennis-matches market):
    #   "50% | ANSWER_ID | 🇫🇷 Q. Halys vs 🇨🇱 A. Tabilo [Australian Open – Round 1]"
    # The bracketed tournament tag and flag emojis are stripped automatically.
    # No date is needed — finalized matches are filtered out, and the active
    # match between the two named players is chosen by surname token overlap.
    RAW_ATP = r"""
"""
    specs = generate_job_specs(RAW_ATP, sport="tennis_atp", year_two_digits="26")
    output_yaml(specs, "tennis_atp")

    # WTA Tennis (women's) — identical input format
    RAW_WTA = r"""
"""
    specs = generate_job_specs(RAW_WTA, sport="tennis_wta", year_two_digits="26")
    output_yaml(specs, "tennis_wta")

    # NCAA Basketball
    # Input format: "37% | ANSWER_ID | TeamA vs TeamB"
    # game_date is optional — if omitted, matches across all dates (useful for tournament).
    # Requires manifold_slug_override since NCAA has many different Manifold markets.
    RAW_NCAA = r"""

"""
    specs = generate_job_specs(
        RAW_NCAA, sport="ncaa_basketball", year_two_digits="26",
        manifold_slug_override="march-madness-ncaa-mens-basketball",
    )
    output_yaml(specs, "ncaa_basketball")

    # ---- Generic multi-choice Kalshi markets ----
    # For non-sports markets (e.g., Spotify #1 song, awards, politics)
    # Provide: Kalshi event URL + Manifold inputs (with inline answer IDs preferred)
    #
    # Example: Spotify #1 Song market
    # Kalshi: https://kalshi.com/markets/kxranklistsongspotusa/.../kxranklistsongspotusa-26apr01
    # Manifold: https://manifold.markets/USER/spotify-usa-1-song-in-march-2026
    RAW_GENERIC = r"""
"""
    KALSHI_EVENT_URL = "https://kalshi.com/markets/kxf1race/f1-race/kxf1race-hungp26"
    MANIFOLD_SLUG = "who-will-win-the-2026-f1-belgian-gr"

    specs = generate_generic_configs(
        kalshi_url_or_ticker=KALSHI_EVENT_URL,
        manifold_slug=MANIFOLD_SLUG,
        raw_manifold_inputs=RAW_GENERIC,
    )
    output_yaml(specs, "generic")


# =============================================================================
# Generic sports-fixture connector
#
# Handles the "<TeamA> beat <TeamB> (<when>)" markets that cover a whole
# matchday in one multi-choice market, where each answer resolves YES if the
# FIRST-named team wins. Unlike generate_job_specs() this needs no SPORTS entry
# and no nickname table — you pass the Kalshi series ticker directly, so a new
# competition works the day it appears.
#
#   Manifold: "Arsenal Beat Kairat Almaty (Jan 28th)"            -> YES if Arsenal win
#   Manifold: "Burnley beat Tottenham Hotspur (Sat Jan 24, 15:00)"
#   Kalshi:   KXUCLGAME-26AUG11BOGUSG-BOG  title "Bodoe/Glimt vs Union Gilloise Winner?"
#             yes_sub_title "Reg Time: Bodoe/Glimt"
# =============================================================================

# Winner-first fixture verbs. Every one of these names the WINNER on the left,
# which is the whole basis of the fixture path — the pair is wired to the Kalshi
# leg for the first team. Surveyed live 2026-09-05 across 112 open matchday
# markets: "beat"/"beats" (EPL matchweek) and "defeats" (college football week
# markets, 41 fixtures in a single market) are the two that actually occur.
FIXTURE_RE = re.compile(
    r"^(.+?)\s+(?:beats?|defeats?|def\.)\s+(.+?)(?:\s*\(([^)]*)\))?\s*$",
    re.IGNORECASE)

# "vs. Western Kentucky" / "at Arkansas" — the answer names only the OPPONENT and
# the subject team lives in the market question ("Which Games will Georgia
# Bulldogs Football win during the 2026 Season?"). Needs `subject_team`.
OPPONENT_ONLY_RE = re.compile(
    r"^(?:vs\.?|v\.|at|@)\s+(.+?)(?:\s*\(([^)]*)\))?\s*$", re.IGNORECASE)

# "Patriots vs. Seahawks" — names a GAME, not a winner. Never a fixture: see
# parse_fixture_line.
AMBIGUOUS_VS_RE = re.compile(
    r"^(.+?)\s+(?:vs\.?|v\.)\s+(.+?)(?:\s*\(([^)]*)\))?\s*$", re.IGNORECASE)

# Pulls the subject team out of a "which games will X win" style question.
SUBJECT_TEAM_RE = re.compile(
    r"which\s+(?:of\s+the\s+)?games?\s+will\s+(.+?)\s+win\b", re.IGNORECASE)
# Trailing sport words that are in the question but never in a Kalshi team name.
SUBJECT_TRIM_RE = re.compile(
    r"\s+(?:football|basketball|soccer|baseball|hockey|mens|womens|team)$", re.IGNORECASE)


def infer_subject_team(question: str) -> Optional[str]:
    """'Which Games will Georgia Bulldogs Football win in 2026?' -> 'Georgia Bulldogs'."""
    m = SUBJECT_TEAM_RE.search(question or "")
    if not m:
        return None
    name = m.group(1).strip()
    prev = None
    while name != prev:                      # "Georgia Bulldogs Football Team"
        prev = name
        name = SUBJECT_TRIM_RE.sub("", name).strip()
    return name or None


# A TV-guide tail: a kickoff time and broadcaster tacked on after the date, as in
# "Patriots beat Seahawks (09/09) 8:20 PM (NBC/Peacock)". FIXTURE_RE treats the
# LAST parenthetical as the date, so without stripping this first, team B comes
# out as "Seahawks (09/09) 8:20 PM" and the date as "NBC/Peacock" — a parse that
# succeeds and is wrong, which then fails downstream as an unexplained "no Kalshi
# game for ...". Stripping only a time-then-parenthetical at the very END leaves
# a parenthesised team name ("Miami (FL)") alone.
TV_GUIDE_TAIL_RE = re.compile(
    r"\s*\d{1,2}:\d{2}\s*(?:AM|PM)\s*\([^)]*\)\s*$", re.IGNORECASE)


def parse_fixture_line(text: str, subject_team: Optional[str] = None
                       ) -> Optional[Tuple[str, str, Optional[str]]]:
    """'<A> beat <B> (when)' -> (A, B, when), winner first. None if it doesn't fit.

    Also accepts '<A> defeats <B>', and — when `subject_team` is supplied —
    opponent-only answers like 'vs. Western Kentucky' / 'at Arkansas', where the
    subject team comes from the market question instead of the answer.

    A bare '<A> vs <B>' is deliberately REFUSED. "vs" names a fixture, not a
    result: Manifold's "Which one of week 1's NFL games will have the rarest
    score?" lists "Patriots vs. Seahawks", "49ers vs. Rams" — answers that
    identify a game and say nothing about who wins. Reading the left side as the
    winner there would wire every pair to an arbitrary team and trade it. If a
    market really does mean "A beats B", the fix is a rewrite, not a looser regex.
    """
    text = TV_GUIDE_TAIL_RE.sub("", text.strip()).strip()

    m = FIXTURE_RE.match(text)
    if m:
        a, b = m.group(1).strip(), m.group(2).strip()
        if a and b:
            return a, b, (m.group(3) or "").strip() or None

    if subject_team:
        m = OPPONENT_ONLY_RE.match(text)
        if m:
            opponent = m.group(1).strip()
            if opponent:
                return subject_team, opponent, (m.group(2) or "").strip() or None

    return None


def fixture_parse_hint(text: str, subject_team: Optional[str] = None) -> str:
    """Why a line was not a fixture — the useful half of a miss report."""
    if AMBIGUOUS_VS_RE.match(text.strip()):
        return ("'<A> vs <B>' names a game, not a winner — refused on purpose "
                "(see parse_fixture_line)")
    if not subject_team and OPPONENT_ONLY_RE.match(text.strip()):
        return ("opponent-only answer; pass subject_team= (or use a question of "
                "the form 'Which games will <TEAM> win')")
    return "unparseable"


def team_score(query: str, candidate: str) -> float:
    """0..1 similarity between two team names."""
    q, c = norm(query), norm(candidate)
    if not q or not c:
        return 0.0
    if q == c:
        return 1.0
    if collapse_digraphs(q) == collapse_digraphs(c):
        return 0.98
    q_toks, c_toks = q.split(), c.split()
    overlap = len(set(q_toks) & set(c_toks))
    # Denominator is the LONGER name. Dividing by the shorter one scored
    # "Michigan" against "Western Michigan" as 1.0 — identical to the exact
    # match — so a Michigan fixture tied across every game featuring either
    # team and was withheld as ambiguous. Leftover tokens on the candidate now
    # cost something, while the contiguous-run bonus below still lets a genuine
    # short form ("Newcastle" for "Newcastle United") clear the 0.8 gate.
    coverage = overlap / (max(len(q_toks), len(c_toks)) or 1)
    if _tokens_contiguous_in(q_toks, c_toks) or _tokens_contiguous_in(c_toks, q_toks):
        coverage = max(coverage, 0.95)
    return coverage


def fixture_date_tokens(when: Optional[str]) -> Optional[str]:
    """'Sat Jan 24, 15:00' / 'Jan 28th' -> a 'JAN24' style datecode fragment."""
    if not when:
        return None
    months = set(MON.values())
    # (?!\d) rather than \b: "Jan 28th" has no word boundary between 8 and t.
    # Scan all candidates so a leading day-of-week ("Sat Jan 24") is skipped
    # rather than treated as the month.
    for m in re.finditer(r"([A-Za-z]{3})[a-z]*\.?\s+(\d{1,2})(?!\d)", when):
        mon = m.group(1).upper()
        if mon in months:
            return f"{mon}{int(m.group(2)):02d}"
    return None


def sport_key_for_series(series_ticker: str) -> Optional[str]:
    """The SPORTS entry that owns a Kalshi series, if any."""
    st = (series_ticker or "").strip().upper()
    for key, cfg in SPORTS.items():
        if cfg.get("series_ticker", "").upper() == st and cfg.get("nick_to_kalshi_participant"):
            return key
    return None


def nickname_hint(series_ticker: str) -> str:
    """Extra miss text when the fixture path is simply the wrong tool.

    The fixture path matches the answer's team names DIRECTLY against Kalshi's —
    that is what lets it work on a brand-new competition with no config. So it
    cannot connect a market that writes team NICKNAMES ("Patriots beat Seahawks")
    while Kalshi writes cities ("New England", "Seattle"): the two never share a
    token. The `sport` path exists for exactly those leagues and carries the
    nickname table, so point at it rather than leaving a bare "no game found".
    """
    key = sport_key_for_series(series_ticker)
    if not key:
        return ""
    return (f"\n#   NOTE: {series_ticker} is covered by `sport {key}`, which has the "
            f"nickname table\n"
            f"#   (Patriots -> New England). If these answers use nicknames rather than "
            f"the names\n"
            f"#   Kalshi uses, run: make_configs.py sport {key} --manifold-slug <slug> --all")


def generate_fixture_configs(
    manifold_slug: str,
    series_ticker: str,
    answers: Optional[List[dict]] = None,
    diagnostics: Optional[dict] = None,
    subject_team: Optional[str] = None,
    markets: Optional[List[dict]] = None,
) -> List[dict]:
    """Connect a matchday market to a Kalshi game series.

    Each Manifold answer names the winner first, so the pair is wired to the
    Kalshi leg for that team — never the opponent's leg and never the Tie leg.
    """
    series_ticker = series_ticker.strip().upper()
    if "/" in series_ticker:  # tolerate a pasted Kalshi URL
        series_ticker = series_ticker.rstrip("/").split("/")[-1].upper()

    if markets is None:
        print(f"# Fetching Kalshi markets for series: {series_ticker}")
        all_markets = kalshi_get_all_markets(series_ticker, limit=1000)
    else:
        all_markets = markets      # caller already has them (batch use, tests)
    active = [m for m in all_markets if m.get("status") not in ("finalized", "settled")]
    print(f"# {len(all_markets)} markets, {len(active)} active")
    if not active:
        print(f"# WARNING: no active markets in {series_ticker} — nothing to match")

    if answers is None:
        answers = [a for a in fetch_manifold_answers(manifold_slug)
                   if a.get("resolution") is None]
    print(f"# {len(answers)} unresolved Manifold answers")

    # Opponent-only markets ("Which games will Georgia win?" -> "vs. Vanderbilt")
    # put the subject team in the QUESTION, not the answers. Infer it, and say so
    # — a wrong subject silently wires every pair to the wrong team.
    if subject_team is None and any(
            OPPONENT_ONLY_RE.match((a.get("text") or "").strip()) for a in answers):
        question = fetch_manifold_question(manifold_slug)
        subject_team = infer_subject_team(question or "")
        if subject_team:
            print(f"# Opponent-only answers detected; subject team inferred from the "
                  f"question: {subject_team!r}")
        else:
            print(f"# WARNING: answers look opponent-only ('vs. X' / 'at X') but no "
                  f"subject team could be inferred from {question!r}. "
                  f"Pass subject_team= explicitly.")

    games = group_kalshi_games(active)
    rewrites, _pins = load_aliases()

    if diagnostics is not None:
        diagnostics.setdefault("matched", [])
        diagnostics.setdefault("ambiguous", [])
        diagnostics.setdefault("misses", [])

    specs: List[dict] = []
    misses: List[str] = []
    ambiguous: List[str] = []

    def resolved(name: str) -> str:
        return rewrites.get(norm(name), name)

    for answer in answers:
        text = re.sub(r"\s+", " ", (answer.get("text") or "")).strip()
        parsed = parse_fixture_line(text, subject_team)
        if not parsed:
            hint = fixture_parse_hint(text, subject_team)
            misses.append(f"Not a winner-first fixture ({hint}): {text}")
            if diagnostics is not None:
                diagnostics["misses"].append({"text": text, "detail": hint})
            continue

        team_a, team_b, when = parsed
        a_name, b_name = resolved(team_a), resolved(team_b)
        want_date = fixture_date_tokens(when)

        # Score each game on how well it contains BOTH teams.
        scored = []
        for key, legs in games.items():
            names = [kalshi_team_name(m) for m in legs]
            names = [n for n in names if norm(n) not in TIE_NAMES]
            if not names:
                continue
            # A and B must match two DIFFERENT legs. Taking each side's best leg
            # independently lets both land on the same one: in Monmouth vs
            # Western Michigan, "Michigan" and "Western Michigan" both score
            # against the Western Michigan leg, so an unrelated game looked like
            # a perfect match for "Michigan defeats Western Michigan".
            scores = [(team_score(a_name, n), team_score(b_name, n)) for n in names]
            score = 0.0
            for i, (sa, _) in enumerate(scores):
                if sa < 0.8:
                    continue
                for j, (_, sb) in enumerate(scores):
                    if i != j and sb >= 0.8:
                        score = max(score, (sa + sb) / 2)
            if score <= 0.0:
                continue
            if want_date and want_date not in key.upper():
                score -= 0.05   # prefer the matching date, don't require it
            scored.append((score, key, legs))

        if not scored:
            options = sorted({kalshi_team_name(m) for m in active
                              if norm(kalshi_team_name(m)) not in TIE_NAMES})
            misses.append(f"No Kalshi game for: {text}\n"
                          f"#   looked for '{a_name}' vs '{b_name}' among {options[:12]}"
                          + nickname_hint(series_ticker))
            if diagnostics is not None:
                diagnostics["misses"].append({
                    "text": text,
                    "detail": f"no game matching '{a_name}' vs '{b_name}'"})
            continue

        scored.sort(key=lambda t: -t[0])
        top = scored[0][0]
        tied = [s for s in scored if abs(s[0] - top) < 1e-9]
        if len(tied) > 1:
            ambiguous.append(f"{text} -> {len(tied)} games match: "
                             f"{[k for _, k, _ in tied]}")
            if diagnostics is not None:
                diagnostics["ambiguous"].append({
                    "text": text, "detail": f"{len(tied)} games match",
                    "candidates": [k for _, k, _ in tied]})
            continue

        _score, _key, legs = scored[0]
        # Within the game, take the leg paying out on the FIRST-named team.
        leg_scores = [(team_score(a_name, kalshi_team_name(m)), m) for m in legs
                      if norm(kalshi_team_name(m)) not in TIE_NAMES]
        leg_scores.sort(key=lambda t: -t[0])
        if not leg_scores or leg_scores[0][0] < 0.8:
            misses.append(f"Found the game but no leg for '{a_name}': {text}")
            if diagnostics is not None:
                diagnostics["misses"].append({
                    "text": text, "detail": f"no leg for '{a_name}'"})
            continue
        if len(leg_scores) > 1 and abs(leg_scores[0][0] - leg_scores[1][0]) < 1e-9:
            ambiguous.append(f"{text} -> both legs score equally "
                             f"({leg_scores[0][1]['ticker']} / {leg_scores[1][1]['ticker']})")
            if diagnostics is not None:
                diagnostics["ambiguous"].append({
                    "text": text, "detail": "both legs score equally",
                    "candidates": [leg_scores[0][1]["ticker"], leg_scores[1][1]["ticker"]]})
            continue

        winner_leg = leg_scores[0][1]
        specs.append({
            "name": f"{team_a} beat {team_b}",
            "kalshi_ticker": winner_leg["ticker"],
            "manifold_slug": manifold_slug,
            "manifold_answer_id": answer.get("id"),
        })
        if diagnostics is not None:
            diagnostics["matched"].append({
                "text": text, "ticker": winner_leg["ticker"],
                "detail": f"{kalshi_team_name(winner_leg)} leg of {winner_leg.get('title')}"})

    if misses:
        print("\n# --- MISSES ---")
        for m in misses:
            print("#", m)
    if ambiguous:
        print("\n# --- AMBIGUOUS (withheld) ---")
        for a in ambiguous:
            print("#", a)
        print(f"# Break ties with a rewrites: entry in {ALIASES_FILE}.")

    print(f"\n# Matched {len(specs)}/{len(answers)} fixtures")
    return specs


# =============================================================================
# Answer-block builder — replaces the find_manifold_answers.py copy-paste step
# =============================================================================

# Parsers whose Manifold answer text carries the game date (so --date filters
# the answer list). ncaa_vs / tennis_vs have no date in the text.
DATED_PARSERS = {"nba_win_at", "nhl_win_at", "mlb_win_at", "nfl_beat", "epl_win"}


def resolve_date_arg(s: str) -> date:
    """Parse a --date value into a date.

    Accepts: today | tomorrow | yesterday | M/D | MM/DD | M/D/YY | YYYY-MM-DD
    Bare M/D assumes the current year.
    """
    s = (s or "").strip().lower()
    today = date.today()
    if s in ("today", "t", ""):
        return today
    if s == "tomorrow":
        return today + timedelta(days=1)
    if s == "yesterday":
        return today - timedelta(days=1)

    m = re.fullmatch(r"(\d{4})-(\d{1,2})-(\d{1,2})", s)
    if m:
        return date(int(m.group(1)), int(m.group(2)), int(m.group(3)))

    m = re.fullmatch(r"(\d{1,2})/(\d{1,2})(?:/(\d{2}|\d{4}))?", s)
    if m:
        year = today.year
        if m.group(3):
            year = int(m.group(3))
            if year < 100:
                year += 2000
        return date(year, int(m.group(1)), int(m.group(2)))

    raise ValueError(
        f"Could not parse --date {s!r}. Use today/tomorrow, M/D, or YYYY-MM-DD."
    )


def date_text_pattern(d: date) -> "re.Pattern":
    """Regex matching how `d` appears in Manifold answer text ('7/29' or '07/29').

    Digit guards on both sides so 7/29 does not match 17/29 or 7/291.
    """
    months = {str(d.month)}
    if d.month < 10:
        months.add(f"0{d.month}")
    days = {str(d.day)}
    if d.day < 10:
        days.add(f"0{d.day}")
    alts = "|".join(rf"{mm}/{dd}" for mm in sorted(months) for dd in sorted(days))
    return re.compile(rf"(?<!\d)(?:{alts})(?!\d)")


def answer_prob(answer: dict) -> float:
    """Answer probability — the API field is `probability`; `prob` on some shapes."""
    for key in ("prob", "probability"):
        val = answer.get(key)
        if val is not None:
            return float(val)
    return 0.0


def filter_answers(
    answers: List[dict],
    date_filter: date = None,
    contains: str = None,
    include_resolved: bool = False,
    min_prob: Optional[float] = None,
    max_prob: Optional[float] = None,
) -> Tuple[List[dict], Dict[str, int]]:
    """Apply the standard answer filters. Returns (kept, counts_dropped_by_reason).

    min_prob/max_prob exist because a matchday market carries many answers at
    ~0% that are not worth a WebSocket subscription or a position poll.
    """
    kept: List[dict] = []
    dropped = {"resolved": 0, "date": 0, "contains": 0, "probability": 0}
    pattern = date_text_pattern(date_filter) if date_filter else None
    contains_norm = norm(contains) if contains else None

    for a in answers:
        if not include_resolved and a.get("resolution") is not None:
            dropped["resolved"] += 1
            continue
        text = re.sub(r"\s+", " ", (a.get("text") or "")).strip()
        if not text:
            continue
        if pattern and not pattern.search(text):
            dropped["date"] += 1
            continue
        if contains_norm and contains_norm not in norm(text):
            dropped["contains"] += 1
            continue
        prob = answer_prob(a)
        if (min_prob is not None and prob < min_prob) or \
           (max_prob is not None and prob > max_prob):
            dropped["probability"] += 1
            continue
        kept.append(a)

    return kept, {k: v for k, v in dropped.items() if v}


def describe_drops(dropped: Dict[str, int]) -> str:
    labels = {"resolved": "resolved", "date": "other dates",
              "contains": "filtered by --contains", "probability": "outside prob range"}
    return ", ".join(f"{n} {labels.get(k, k)}" for k, n in dropped.items())


def manifold_answers_block(
    slug: str,
    date_filter: date = None,
    include_resolved: bool = False,
    contains: str = None,
    min_prob: Optional[float] = None,
    max_prob: Optional[float] = None,
) -> str:
    """Build a `NN% | answerId | text` block straight from the Manifold API.

    This is the exact format strip_percent_lines()/the parsers already accept, so
    it drops into generate_job_specs()/generate_generic_configs() unchanged — no
    clipboard round-trip through find_manifold_answers.py.

    Resolved answers are dropped by default: a season-long market like
    major-league-baseball-live carries ~1500 answers of which only the current
    slate is unresolved.
    """
    answers = fetch_manifold_answers(slug)
    if not answers:
        print(f"# WARNING: no answers found for Manifold market '{slug}'")
        return ""

    kept, dropped = filter_answers(answers, date_filter=date_filter,
                                   contains=contains, include_resolved=include_resolved,
                                   min_prob=min_prob, max_prob=max_prob)
    # Answer text routinely carries trailing/double spaces ("Rangers  WIN at").
    # Collapsed outside the f-string: a backslash inside an f-string expression
    # is a SyntaxError before Python 3.12, and the bot host runs 3.11.
    rows = []
    for a in kept:
        text = re.sub(r"\s+", " ", (a.get("text") or "")).strip()
        rows.append(f"{int(round(answer_prob(a) * 100))}% | {a.get('id')} | {text}")

    summary = f"# {len(rows)} answers from '{slug}'"
    if date_filter:
        summary += f" matching {date_filter.month}/{date_filter.day}"
    if dropped:
        summary += f" (skipped {describe_drops(dropped)})"
    print(summary)
    return "\n".join(rows)


# =============================================================================
# Kalshi lookup + manual pairs + per-pair overrides
# =============================================================================

def lookup_kalshi(ticker_or_url: str, search: str = None,
                  include_finalized: bool = False) -> List[dict]:
    """List a Kalshi event's or series' markets — the Kalshi side of
    find_manifold_answers.py.

    Accepts an event ticker (KXNEWPOPE-70), a series ticker (KXUCLGAME), or a
    URL. Tries the event endpoint first, since an event ticker passed to the
    series endpoint returns nothing rather than erroring.
    """
    ident = extract_kalshi_event_ticker(ticker_or_url)
    markets = kalshi_get_event_markets(ident)
    source = "event"
    if not markets:
        markets = kalshi_get_all_markets(ident, limit=1000)
        source = "series"
    if not include_finalized:
        markets = [m for m in markets if m.get("status") not in ("finalized", "settled")]
    if search:
        q = norm(search)
        markets = [m for m in markets
                   if q in norm(f"{m.get('yes_sub_title', '')} {m.get('title', '')} "
                                f"{m.get('ticker', '')}")]
    print(f"# {len(markets)} markets for {ident} (as {source})")
    return markets


def format_kalshi_markets(markets: List[dict], limit: int = 40) -> str:
    """Human-readable listing: ticker, payout name, price, status."""
    lines = []
    for m in markets[:limit]:
        # Kalshi's integer-cent yes_bid/yes_ask are null on every market since
        # ~2026-09; the live values are decimal-dollar STRINGS in the _dollars
        # twins. Reading the old names printed "no quote" for everything.
        def _cents(dollars_key, cents_key):
            v = m.get(dollars_key)
            if v is not None:
                try:
                    return round(float(v) * 100)
                except (TypeError, ValueError):
                    pass
            return m.get(cents_key)

        bid = _cents("yes_bid_dollars", "yes_bid")
        ask = _cents("yes_ask_dollars", "yes_ask")
        price = f"{bid}/{ask}c" if bid is not None and ask is not None else "no quote"
        name = kalshi_team_name(m) or m.get("yes_sub_title") or m.get("title") or "?"
        lines.append(f"{m.get('ticker')}\n    {name}  [{price}, {m.get('status')}]")
    if len(markets) > limit:
        lines.append(f"... and {len(markets) - limit} more (use a search term)")
    return "\n".join(lines) if lines else "(no markets)"


def build_manual_pair(manifold_slug_or_url: str, answer_id: Optional[str],
                      kalshi_ticker: str, name: str = None) -> Tuple[Optional[dict], List[str]]:
    """Validate and build one pair by hand. Returns (spec_or_None, problems).

    Both legs are verified against their APIs before the pair is built: a typo'd
    answer id or ticker would otherwise produce a pair that silently never
    trades, which is far harder to notice than an error here.
    """
    problems: List[str] = []
    slug = extract_manifold_slug(manifold_slug_or_url)

    try:
        r = requests.get(f"https://api.manifold.markets/v0/slug/{slug}", timeout=30)
        market = r.json() if r.ok else None
        if not r.ok:
            problems.append(f"Manifold market '{slug}' not found (HTTP {r.status_code})")
            market = None
    except (requests.RequestException, ValueError) as e:
        problems.append(f"Manifold lookup failed: {e}")
        market = None

    answer_text = None
    if market is not None:
        answers = market.get("answers") or []
        if answer_id:
            match = next((a for a in answers if a.get("id") == answer_id), None)
            if match is None:
                sample = [a.get("id") for a in answers[:5] if a.get("resolution") is None]
                problems.append(f"Answer '{answer_id}' is not on '{slug}'"
                                + (f" (e.g. {sample})" if sample else ""))
            else:
                answer_text = re.sub(r"\s+", " ", match.get("text") or "").strip()
                if match.get("resolution") is not None:
                    problems.append(f"Answer '{answer_id}' is already resolved "
                                    f"({match.get('resolution')})")
        elif answers:
            problems.append(f"'{slug}' is multi-choice — an answer id is required")

    ticker = kalshi_ticker.strip().upper()
    try:
        r = requests.get(f"{KALSHI_API_BASE}/markets/{ticker}", timeout=30)
        km = (r.json() or {}).get("market") if r.ok else None
        if not r.ok or not km:
            problems.append(f"Kalshi market '{ticker}' not found (HTTP {r.status_code})")
        elif km.get("status") in ("finalized", "settled"):
            problems.append(f"Kalshi market '{ticker}' is {km.get('status')} — already over")
    except (requests.RequestException, ValueError) as e:
        problems.append(f"Kalshi lookup failed: {e}")
        km = None

    if problems:
        return None, problems

    spec = {
        "name": name or answer_text or (km.get("yes_sub_title") if km else ticker) or ticker,
        "kalshi_ticker": ticker,
        "manifold_slug": slug,
    }
    if answer_id:
        spec["manifold_answer_id"] = answer_id
    return spec, []


# Flag name -> engine key. The engine reads PAIR_OVERRIDE_KEYS off each pair
# (bot_core.PairSettings resolves them once at startup).
OVERRIDE_ALIASES = {
    "max-position": "max_position", "max-bet": "max_bet_size",
    "min-edge": "min_edge_threshold", "max-edge": "max_edge_threshold",
    "clearing-edge": "clearing_edge_threshold", "max-spread": "max_kalshi_spread",
    "cooldown": "min_time_between_trades", "extreme-mult": "extreme_bet_multiplier",
    "edge-scale-start": "edge_scale_start", "size-edge-power": "size_edge_power",
    "reverse": "reverse",
    "amm": "enable_amm", "enable-amm": "enable_amm",
    "amm-fraction": "amm_target_fraction", "liquidity": "liquidity_override",
    "settle-slam": "settle_slam", "tie-resolves": "tie_resolves",
    "settle-bet": "settle_bet_size", "settle-bet-size": "settle_bet_size",
    "partner-trust-ms": "partner_trust_ms", "partner-trust": "partner_trust_ms",
    "partner-max-lead": "partner_max_lead",
    "underdog-threshold": "underdog_threshold", "underdog-min-edge": "underdog_min_edge",
}
VALID_OVERRIDES = {
    "reverse", "max_bet_size", "min_edge_threshold", "max_edge_threshold",
    "max_position", "min_time_between_trades", "edge_scale_start",
    "size_edge_power", "max_kalshi_spread", "max_polymarket_spread",
    "clearing_edge_threshold", "extreme_bet_multiplier",
    "enable_amm", "amm_target_fraction", "p_override", "liquidity_override",
    "settle_slam", "tie_resolves", "settle_bet_size",
    "limit_offset", "min_edge_floor", "adding_edge_tolerance", "min_clearing_room",
    "min_bet_size", "cap_clear_at_position", "partner_trust_ms", "partner_max_lead",
    "underdog_threshold", "underdog_min_edge",
}

# Overrides that take a yes/no rather than a number. Callers accept these as a
# bare flag (--amm) or with an explicit value (--amm false); everything else in
# VALID_OVERRIDES is parsed as int/float. Keep in sync with the engine:
# bot_core.PairSettings coerces enable_amm and reverse with bool().
BOOL_OVERRIDES = {"reverse", "enable_amm", "settle_slam", "cap_clear_at_position"}

# Overrides drawn from a fixed vocabulary — neither a number nor a yes/no, so
# they need their own parse path (int("no") is the failure this prevents).
CHOICE_OVERRIDES = {"tie_resolves": {"no", "half", "ignore"}}


BOOL_TRUE = {"1", "true", "yes", "on", "y", "t"}
BOOL_FALSE = {"0", "false", "no", "off", "n", "f"}


def parse_bool_override(value: Optional[str]) -> Optional[bool]:
    """A bare flag means True; an explicit value must be recognisable.

    Returns None for anything else so the caller can REPORT it. Silently reading
    an unrecognised value as False is how `--amm 50` (a value the flag never
    wanted) turns into a pair that trades with the opposite sizing to the one
    that was asked for, with nothing in the output to say so.
    """
    if value is None:
        return True
    v = str(value).strip().lower()
    if v in BOOL_TRUE:
        return True
    if v in BOOL_FALSE:
        return False
    return None


def resolve_override(flag: str) -> Optional[str]:
    """Map a CLI/Telegram flag to an engine override key, or None if unknown."""
    key = OVERRIDE_ALIASES.get(flag.strip().lstrip("-").lower())
    if key:
        return key
    key = flag.strip().lstrip("-").lower().replace("-", "_")
    return key if key in VALID_OVERRIDES else None


def apply_overrides(specs: List[dict], overrides: Dict[str, object]) -> List[dict]:
    """Attach per-pair overrides to every spec."""
    for spec in specs:
        spec.update(overrides)
    return specs


# =============================================================================
# Config writing — append to an existing config, preserving everything above
# =============================================================================

def pair_key(pair: dict) -> Tuple[str, str]:
    """Identity of a market pair: the source leg + the Manifold leg."""
    source = (
        pair.get("kalshi_ticker")
        or pair.get("polymarket_token_id")
        or ""
    )
    return (str(source), str(pair.get("manifold_answer_id") or ""))


def _yaml_scalar(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        # PyYAML (YAML 1.1) only reads a float with a '.' in the mantissa:
        # str(0.00001) is "1e-05", which loads back as the STRING '1e-05'.
        if value != value:
            return ".nan"
        if value in (float("inf"), float("-inf")):
            return ".inf" if value > 0 else "-.inf"
        text = repr(value)
        if "e" in text:
            mantissa, exp = text.split("e")
            if "." not in mantissa:
                mantissa += ".0"
            text = f"{mantissa}e{exp}"
        return text
    if isinstance(value, int):
        return str(value)
    escaped = str(value).replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


KEY_ORDER = ["name", "kalshi_ticker", "partner_ticker", "polymarket_token_id",
             "manifold_slug", "manifold_answer_id", "notes"]


def render_pair_entries(specs: List[dict], indent: int = 2) -> str:
    """Render pairs as a YAML list body matching the hand-written config style.

    `indent` is the column of each item's `-`. Appending must match the block
    it joins: PyYAML's dump writes items at column 0 (`- name:`), and an
    indented item after those is not valid YAML.
    """
    out: List[str] = []
    item, cont = " " * indent + "- ", " " * (indent + 2)
    for spec in specs:
        keys = [k for k in KEY_ORDER if k in spec]
        keys += [k for k in spec if k not in KEY_ORDER]
        first = True
        for k in keys:
            prefix = item if first else cont
            out.append(f"{prefix}{k}: {_yaml_scalar(spec[k])}")
            first = False
    return "\n".join(out) + "\n"


def report_conflicts(specs: List[dict]) -> set:
    """Flag pairs that collide, returning their indices in `specs`.

    Two Manifold answers mapping to one Kalshi ticker (or vice versa) means a
    match went wrong. The common cause is a genuine ambiguity: a doubleheader
    (same teams, same date, two games) matches date+teams identically, so the
    Kalshi side picks one game for both Manifold answers — one of the two pairs
    is then wired to the wrong game and would trade its prices. These are
    withheld from the config rather than written, since a wrong pair trades.
    """
    by_ticker: Dict[str, List[int]] = {}
    by_answer: Dict[str, List[int]] = {}
    for i, spec in enumerate(specs):
        ticker = spec.get("kalshi_ticker") or spec.get("polymarket_token_id") or ""
        by_ticker.setdefault(ticker, []).append(i)
        by_answer.setdefault(spec.get("manifold_answer_id") or "", []).append(i)

    conflicted: set = set()
    for label, groups in (("source leg", by_ticker), ("Manifold answer", by_answer)):
        for value, idxs in groups.items():
            if len(idxs) > 1:
                names = [specs[i].get("name", "?") for i in idxs]
                print(f"# ⚠ CONFLICT: {len(idxs)} pairs share {label} {value}: {names}")
                conflicted.update(idxs)
    return conflicted


def _find_insertion_point(lines: List[str]) -> Optional[int]:
    """Index to insert new entries at: end of the existing `market_pairs:` block.

    Returns None when the file has no market_pairs: key. Only the block's own
    lines advance the point, so a column-0 comment after the block (usually a
    heading for whatever follows) stays below the insertion.

    The block's own lines are the indented ones AND column-0 `- ` items: YAML
    lets a mapping's list sit at the key's own column, and that is what
    yaml.dump writes (config_ab_*.yaml, make_manifoldsports_configs.py). Before
    this, the first `- name:` read as the end of the block — so append, /drop,
    the reaper and --overwrite all produced YAML that no longer parsed.
    """
    start = None
    for i, line in enumerate(lines):
        if re.match(r"^market_pairs\s*:", line):
            start = i
            break
    if start is None:
        return None

    last_content = start
    for k in range(start + 1, len(lines)):
        line = lines[k]
        if not line.strip():
            continue
        if line.startswith((" ", "\t")) or re.match(r"^-(\s|$)", line):
            last_content = k
            continue
        if line.lstrip().startswith("#"):
            continue
        break  # a new top-level key ends the block
    return last_content + 1


def _pairs_item_indent(lines: List[str]) -> int:
    """Column of the existing market_pairs items' `-` (2 if there are none)."""
    start = next((i for i, ln in enumerate(lines) if re.match(r"^market_pairs\s*:", ln)), None)
    end = _find_insertion_point(lines)
    if start is None or end is None:
        return 2
    for ln in lines[start + 1:end]:
        m = re.match(r"^( *)-(\s|$)", ln)
        if m:
            return len(m.group(1))
    return 2


def _open_empty_pairs_block(lines: List[str]) -> List[str]:
    """Turn an inline `market_pairs: []` into a bare `market_pairs:` header.

    remove_market_pairs writes the explicit empty list on purpose (a bare key
    parses as None, and passive_market_maker.py does len() on it). But appending
    indented entries UNDER `market_pairs: []` is not valid YAML — the file then
    fails to parse, and nothing notices until the next run refuses to touch it.
    So "drop every pair, then add new ones" silently corrupted the config.
    Reopening the block here keeps both halves of that cycle working.
    """
    for i, line in enumerate(lines):
        m = re.match(r"^(market_pairs\s*:)\s*\[\s*\]\s*$", line.rstrip("\n"))
        if m:
            out = list(lines)
            out[i] = m.group(1) + ("\n" if line.endswith("\n") else "")
            return out
        if re.match(r"^market_pairs\s*:", line):
            break
    return lines


def plan_pair_write(specs: List[dict], existing: List[dict], overwrite: bool = False
                    ) -> Tuple[List[dict], Dict[int, dict], List[str], List[str]]:
    """Sort `specs` against the pairs already in a config.

    Returns (new, updates, unchanged, skipped):
      new        specs whose pair_key() is not in the config yet
      updates    {index into existing: merged pair} — overwrite only
      unchanged  names of present pairs the merge would not change
      skipped    names of present pairs left alone (no overwrite)

    With overwrite the existing pair is MERGED with the spec: keys the spec
    carries win, keys it doesn't are kept. So `--min-edge 0.0001 --overwrite`
    changes min_edge_threshold and nothing else — a settle_slam set by an
    earlier connect survives. Shared by append_market_pairs and the control
    bot's preview, so what /confirm writes is what the preview said.
    """
    index = {pair_key(p): i for i, p in enumerate(existing)}
    seen = set()
    new, updates, unchanged, skipped = [], {}, [], []
    for spec in specs:
        key = pair_key(spec)
        name = spec.get("name", key[0])
        if key in index:
            if not overwrite:
                skipped.append(name)
                continue
            i = index[key]
            merged = dict(updates.get(i, existing[i]))
            merged.update(spec)
            if merged == existing[i]:
                unchanged.append(name)
            else:
                updates[i] = merged
            continue
        if key in seen:
            continue
        seen.add(key)
        new.append(spec)
    return new, updates, unchanged, skipped


def describe_updates(existing: List[dict], updates: Dict[int, dict]) -> List[str]:
    """'min_edge_threshold: 0.02 -> 0.0001 (24 pairs)' per distinct change."""
    counts: Dict[Tuple[str, str, str], int] = {}
    for i, merged in updates.items():
        old = existing[i]
        for k, v in merged.items():
            if k not in old or old[k] != v:
                was = repr(old[k]) if k in old else "(unset)"
                key = (k, was, repr(v))
                counts[key] = counts.get(key, 0) + 1
    return [f"{k}: {was} -> {now} ({n} pair{'s' if n != 1 else ''})"
            for (k, was, now), n in sorted(counts.items())]


def _update_pairs_in_place(text: str, raw_pairs: list, existing: List[dict],
                           updates: Dict[int, dict]) -> Optional[str]:
    """Apply overwrite `updates` by editing ONLY the changed keys' lines.

    A changed key's line is rewritten where it stands; a new key is added as the
    pair's last line. Nothing else moves, so comments in the block survive — a
    full re-render (what /drop does) silently threw away commented-out pairs
    and notes, and the .bak holding them is replaced by the very next write.

    Returns None when the layout isn't one this can edit with certainty (item
    count doesn't line up with the parse, a key present in the parse can't be
    found as a line, or the result doesn't parse back to exactly the expected
    pairs); the caller then re-renders the block instead.
    """
    lines = text.splitlines(keepends=True)
    start = next((i for i, ln in enumerate(lines) if re.match(r"^market_pairs\s*:", ln)), None)
    end = _find_insertion_point(lines)
    if start is None or end is None:
        return None
    indent = _pairs_item_indent(lines)
    item_re = re.compile(rf"^ {{{indent}}}-(\s|$)")
    starts = [k for k in range(start + 1, end) if item_re.match(lines[k])]
    if len(starts) != len(raw_pairs):
        return None
    rows = [starts[i] for i, p in enumerate(raw_pairs) if isinstance(p, dict)]
    if len(rows) != len(existing):
        return None
    limits = dict(zip(starts, starts[1:] + [end]))

    for i in sorted(updates, reverse=True):          # bottom-up: indices above stay valid
        first = rows[i]
        last, cont = first, None
        for k in range(first + 1, limits[first]):
            ln = lines[k]
            if ln.strip() and not ln.lstrip().startswith("#"):
                depth = len(ln) - len(ln.lstrip(" "))
                if depth > indent:
                    last = k
                    cont = depth if cont is None else cont
        cont_pad = " " * (cont if cont is not None else indent + 2)

        old, new = existing[i], updates[i]
        inserts = []
        for key, value in new.items():
            if key in old and old[key] == value:
                continue
            pat = re.compile(rf"^(\s*-\s+|\s+){re.escape(str(key))}\s*:")
            hit = next((k for k in range(first, last + 1) if pat.match(lines[k])), None)
            if hit is not None:
                lead = pat.match(lines[hit]).group(1)
                lines[hit] = f"{lead}{key}: {_yaml_scalar(value)}\n"
            elif key in old:
                return None
            else:
                inserts.append(f"{cont_pad}{key}: {_yaml_scalar(value)}\n")
        if inserts:
            if not lines[last].endswith("\n"):
                lines[last] += "\n"
            lines[last + 1:last + 1] = inserts

    out = "".join(lines)
    try:
        parsed = yaml.safe_load(out) or {}
    except yaml.YAMLError:
        return None
    got = [p for p in (parsed.get("market_pairs") or []) if isinstance(p, dict)]
    if got != [updates.get(i, p) for i, p in enumerate(existing)]:
        return None
    return out


def append_market_pairs(specs: List[dict], path: str, dry_run: bool = False,
                        overwrite: bool = False) -> int:
    """Append pairs to `path`, skipping ones already present. Returns the
    number of pairs written (added + updated).

    Append-only and byte-preserving above the insertion point: the hand-written
    `bot:` block and its comments are never rewritten. A .bak is written first.

    overwrite=True updates pairs that are already present instead of skipping
    them (see plan_pair_write for the merge rule). Only the changed keys' lines
    are edited (_update_pairs_in_place), so comments in the block survive; a
    layout that editor won't touch falls back to re-rendering the block, as
    remove_market_pairs does, and says that comments were lost.
    """
    if not specs:
        print("# Nothing to write — no pairs generated.")
        return 0

    existing_pairs: List[dict] = []
    text = ""
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        try:
            cfg = yaml.safe_load(text) or {}
        except yaml.YAMLError as e:
            print(f"# ERROR: {path} is not valid YAML ({e}) — refusing to write.")
            return 0
        existing_pairs = [p for p in (cfg.get("market_pairs") or []) if isinstance(p, dict)]

    new_specs, updates, unchanged, duplicates = plan_pair_write(
        specs, existing_pairs, overwrite=overwrite)

    if duplicates:
        print(f"# {len(duplicates)} already in {path}, skipped: {duplicates}")
    if unchanged:
        print(f"# {len(unchanged)} already in {path} with these settings — unchanged.")
    if not new_specs and not updates:
        print(f"# No new pairs to add — {path} unchanged.")
        return 0

    dropped_comments: List[str] = []
    work = text
    if updates:
        edited = _update_pairs_in_place(text, cfg.get("market_pairs") or [],
                                        existing_pairs, updates)
        if edited is None:
            # A layout the line editor won't touch: re-render the whole block.
            merged = [updates.get(i, p) for i, p in enumerate(existing_pairs)] + new_specs
            new_text, dropped_comments = _replace_pairs_block(text, merged)
            block = render_pair_entries(new_specs) if new_specs else ""
        work = edited
    if work is not None:
        lines = work.splitlines(keepends=True)
        lines = _open_empty_pairs_block(lines)
        insert_at = _find_insertion_point(lines)
        block = render_pair_entries(new_specs, indent=_pairs_item_indent(lines)) if new_specs else ""

        if not new_specs:
            new_text = work
        elif insert_at is None:
            prefix = work
            if prefix and not prefix.endswith("\n"):
                prefix += "\n"
            new_text = prefix + "\nmarket_pairs:\n" + block
        else:
            if insert_at > 0 and not lines[insert_at - 1].endswith("\n"):
                lines[insert_at - 1] += "\n"
            new_text = "".join(lines[:insert_at]) + block + "".join(lines[insert_at:])

    if dry_run:
        if updates:
            print(f"\n# --- would update {len(updates)} pairs in {path} ---")
            for line in describe_updates(existing_pairs, updates):
                print(f"#   {line}")
        if new_specs:
            print(f"\n# --- would add {len(new_specs)} pairs to {path} ---")
            print(block, end="")
        return len(new_specs) + len(updates)

    backed_up = bool(text)
    if backed_up:
        with open(path + ".bak", "w", encoding="utf-8") as f:
            f.write(text)
    with open(path, "w", encoding="utf-8") as f:
        f.write(new_text)
    note = f"backup at {path}.bak" if backed_up else "new file"
    if updates:
        print(f"# ✓ Updated {len(updates)} existing pairs in {path} (overwrite):")
        for line in describe_updates(existing_pairs, updates):
            print(f"#   {line}")
        if dropped_comments:
            print(f"# note: {len(dropped_comments)} comment line(s) inside market_pairs "
                  f"were dropped by the rewrite (kept in {path}.bak)")
    if new_specs:
        print(f"# ✓ Added {len(new_specs)} pairs to {path} "
              f"({len(existing_pairs) + len(new_specs)} total; {note})")
    elif updates:
        print(f"# {path}: {len(existing_pairs)} pairs total; {note}")
    return len(new_specs) + len(updates)


def set_bot_param(path: str, key: str, value: str) -> Tuple[bool, str]:
    """Set one key in the `bot:` block, preserving comments and layout.

    Line-based rather than a YAML round-trip, because yaml.safe_dump would strip
    every explanatory comment in that block — which is most of its value.
    Returns (changed, message).
    """
    if not os.path.exists(path):
        return False, f"{path} does not exist."
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    try:
        yaml.safe_load(text)
    except yaml.YAMLError as e:
        return False, f"{path} is not valid YAML ({e}) — refusing to edit."

    lines = text.splitlines(keepends=True)
    start = next((i for i, ln in enumerate(lines) if re.match(r"^bot\s*:", ln)), None)
    if start is None:
        return False, f"No `bot:` block in {path}."

    end = len(lines)
    for i in range(start + 1, len(lines)):
        ln = lines[i]
        if ln.strip() and not ln.startswith((" ", "\t")) and not ln.lstrip().startswith("#"):
            end = i
            break

    # Round-trip the value through YAML so `true`/`0.02`/`abc` keep their types.
    try:
        parsed = yaml.safe_load(value)
    except yaml.YAMLError:
        parsed = value
    rendered = yaml.safe_dump(parsed, default_flow_style=True).strip().rstrip("...").strip()

    pattern = re.compile(rf"^(\s+){re.escape(key)}\s*:\s*([^#\n]*)(#.*)?$")
    for i in range(start + 1, end):
        m = pattern.match(lines[i].rstrip("\n"))
        if not m:
            continue
        indent, old, comment = m.group(1), (m.group(2) or "").strip(), m.group(3) or ""
        sep = "  " if comment else ""
        lines[i] = f"{indent}{key}: {rendered}{sep}{comment}\n"
        new_text = "".join(lines)
        try:
            yaml.safe_load(new_text)
        except yaml.YAMLError as e:
            return False, f"Edit would break the YAML ({e}) — not written."
        with open(path + ".bak", "w", encoding="utf-8") as f:
            f.write(text)
        with open(path, "w", encoding="utf-8") as f:
            f.write(new_text)
        return True, f"{key}: {old} → {rendered}"

    lines.insert(start + 1, f"  {key}: {rendered}\n")
    new_text = "".join(lines)
    try:
        yaml.safe_load(new_text)
    except yaml.YAMLError as e:
        return False, f"Edit would break the YAML ({e}) — not written."
    with open(path + ".bak", "w", encoding="utf-8") as f:
        f.write(text)
    with open(path, "w", encoding="utf-8") as f:
        f.write(new_text)
    return True, f"{key}: (new) → {rendered}"


def restore_backup(path: str) -> Tuple[bool, str]:
    """Swap a config with its .bak, keeping the current version as the new .bak."""
    bak = path + ".bak"
    if not os.path.exists(bak):
        return False, f"No backup at {bak}."
    with open(bak, "r", encoding="utf-8") as f:
        backup_text = f.read()
    try:
        cfg = yaml.safe_load(backup_text) or {}
    except yaml.YAMLError as e:
        return False, f"Backup is not valid YAML ({e}) — refusing to restore."
    current = ""
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            current = f.read()
    with open(path, "w", encoding="utf-8") as f:
        f.write(backup_text)
    with open(bak, "w", encoding="utf-8") as f:
        f.write(current)   # so /undo undoes itself
    n = len([p for p in (cfg.get("market_pairs") or []) if isinstance(p, dict)])
    return True, f"Restored {path} from backup ({n} pairs). Run again to redo."


def select_pairs(pairs: List[dict], target: str) -> Tuple[List[dict], str]:
    """Pairs matching `target`, plus a human description of HOW it matched.

    A wrongly-connected market is almost never one pair — connecting a
    multi-choice market wires up every option at once, so discarding it means
    removing 20-50 entries that share a Manifold slug or a Kalshi event. This
    resolves the whole group from one identifier.

    Accepted, in priority order (first kind that matches wins, so a value that
    could read as two things is never silently split across both):

      manifold slug     exact, or a full Manifold URL
      kalshi ticker     exact leg (KXPRESNOMD-28-KH) or the event that owns it
                        (KXPRESNOMD-28 takes every -KH/-GN/... leg under it), or
                        a Kalshi URL
      polymarket token  exact

    Matching is deliberately exact-or-prefix-on-a-boundary. No fuzzy name
    matching: this feeds a destructive edit, and "close enough" is how you drop
    the market you meant to keep.
    """
    t = (target or "").strip()
    if not t:
        return [], "empty target"

    # Both sides normalized: configs written before extract_manifold_slug
    # dropped ?query strings still hold slugs like `...-game?r=Um95THVv`.
    slug = extract_manifold_slug(t).lower()
    by_slug = [p for p in pairs
               if extract_manifold_slug(str(p.get("manifold_slug") or "")).lower() == slug]
    if by_slug:
        return by_slug, f"manifold_slug == {slug}"

    ticker = extract_kalshi_event_ticker(t) if "/" in t else t.upper()
    by_ticker = [p for p in pairs if (p.get("kalshi_ticker") or "").upper() == ticker]
    if by_ticker:
        return by_ticker, f"kalshi_ticker == {ticker}"

    # Event prefix: only on a "-" boundary, so KXNBA-27 cannot eat KXNBA-2700.
    by_event = [p for p in pairs
                if (p.get("kalshi_ticker") or "").upper().startswith(ticker + "-")]
    if by_event:
        return by_event, f"kalshi event {ticker} ({len(by_event)} legs)"

    by_token = [p for p in pairs if str(p.get("polymarket_token_id") or "") == t]
    if by_token:
        return by_token, f"polymarket_token_id == {t}"

    return [], f"nothing matched {t!r}"


def market_summary(pairs: List[dict], limit: int = 12) -> str:
    """The Manifold markets a config holds, with pair counts — shown when a
    /drop target matches nothing, so the right identifier is one copy away."""
    counts: Dict[str, int] = {}
    for p in pairs:
        slug = str(p.get("manifold_slug") or "?")
        counts[slug] = counts.get(slug, 0) + 1
    rows = [f"  {slug} ({n})" for slug, n in
            sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:limit]]
    if len(counts) > limit:
        rows.append(f"  ... and {len(counts) - limit} more")
    return "\n".join(rows)


def drop_pairs(target: str, path: str, dry_run: bool = False) -> List[dict]:
    """Remove every pair matching `target` from the config at `path`."""
    pairs = []
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                cfg = yaml.safe_load(f) or {}
            pairs = [p for p in (cfg.get("market_pairs") or []) if isinstance(p, dict)]
        except yaml.YAMLError as e:
            print(f"# ERROR: {path} is not valid YAML ({e}) — refusing to touch it.")
            return []

    matched, how = select_pairs(pairs, target)
    if not matched:
        print(f"# {how} in {path} ({len(pairs)} pair(s) present) — nothing to drop.")
        if pairs:
            print("# markets in this config:\n" + "\n".join(
                "#" + row for row in market_summary(pairs).splitlines()))
        return []

    print(f"# Matched {len(matched)}/{len(pairs)} pair(s) by {how}")
    return remove_market_pairs({pair_key(p) for p in matched}, path, dry_run=dry_run)


def _replace_pairs_block(text: str, pairs: List[dict]) -> Tuple[str, List[str]]:
    """`text` with its market_pairs block re-rendered from `pairs`.

    Returns (new_text, comment lines that were inside the block and are lost).
    Everything outside the block is kept byte-for-byte.
    """
    lines = text.splitlines(keepends=True)
    start = next((i for i, ln in enumerate(lines) if re.match(r"^market_pairs\s*:", ln)), None)
    if start is None:
        prefix = text if (not text or text.endswith("\n")) else text + "\n"
        body = render_pair_entries(pairs) if pairs else ""
        return prefix + ("\nmarket_pairs:\n" + body if pairs else "\nmarket_pairs: []\n"), []

    end = _find_insertion_point(lines)
    dropped_comments = [
        ln.strip() for ln in lines[start + 1:end] if ln.lstrip().startswith("#")
    ]
    if pairs:
        header, body = lines[start], render_pair_entries(pairs)
        if re.match(r"^market_pairs\s*:\s*\[\s*\]", header):
            header = "market_pairs:\n"   # reopen an explicit empty list
        elif not header.endswith("\n"):
            header += "\n"
    else:
        # Must be an explicit empty list. A bare `market_pairs:` key parses as
        # None, and passive_market_maker.py does len(config.get('market_pairs',
        # [])) — which raises TypeError on None rather than reading as empty.
        # Reaping the last pair is the normal end-of-day state, so this matters.
        header, body = "market_pairs: []\n", ""
    return "".join(lines[:start]) + header + body + "".join(lines[end:]), dropped_comments


def remove_market_pairs(keys: set, path: str, dry_run: bool = False) -> List[dict]:
    """Remove pairs whose pair_key() is in `keys`. Returns the removed pairs.

    Unlike append_market_pairs this must rewrite the market_pairs block, so
    comments *inside* that block are lost (they survive in the .bak). Everything
    above `market_pairs:` — the hand-tuned bot: block and its comments — is still
    preserved byte-for-byte.
    """
    if not os.path.exists(path):
        print(f"# {path} does not exist — nothing to remove.")
        return []

    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    try:
        cfg = yaml.safe_load(text) or {}
    except yaml.YAMLError as e:
        print(f"# ERROR: {path} is not valid YAML ({e}) — refusing to write.")
        return []

    pairs = [p for p in (cfg.get("market_pairs") or []) if isinstance(p, dict)]
    keep = [p for p in pairs if pair_key(p) not in keys]
    removed = [p for p in pairs if pair_key(p) in keys]
    if not removed:
        return []

    if not any(re.match(r"^market_pairs\s*:", ln) for ln in text.splitlines()):
        print(f"# No market_pairs: block in {path} — nothing to remove.")
        return []
    new_text, dropped_comments = _replace_pairs_block(text, keep)

    if dry_run:
        print(f"# --- would remove {len(removed)} pairs from {path} "
              f"({len(keep)} would remain) ---")
        for p in removed:
            print(f"#   - {p.get('name', pair_key(p)[0])}")
        return removed

    with open(path + ".bak", "w", encoding="utf-8") as f:
        f.write(text)
    with open(path, "w", encoding="utf-8") as f:
        f.write(new_text)
    if dropped_comments:
        print(f"# note: {len(dropped_comments)} comment line(s) inside market_pairs "
              f"were dropped by the rewrite (kept in {path}.bak)")
    print(f"# ✓ Removed {len(removed)} pairs from {path} ({len(keep)} remain; "
          f"backup at {path}.bak)")
    return removed


# =============================================================================
# CLI
# =============================================================================

def _emit(specs: List[dict], label: str, args) -> int:
    """Report conflicts, then write to --write or print the YAML block."""
    if not specs:
        print(f"# No pairs generated for {label}.")
        return 1

    conflicted = report_conflicts(specs)
    if conflicted and not args.allow_conflicts:
        specs = [s for i, s in enumerate(specs) if i not in conflicted]
        print(f"# ⚠ Withholding {len(conflicted)} conflicting pairs. Resolve them by hand, "
              f"or pass --allow-conflicts to write them anyway.")
        if not specs:
            print("# Every pair conflicted — nothing to write.")
            return 1

    if args.write:
        append_market_pairs(specs, args.write, dry_run=args.dry_run)
    else:
        output_yaml(specs, label)
        print("\n# (add --write config_websocket.yaml to merge these in)")
    return 0


def cmd_answers(args) -> int:
    """Print the answer block for a slug — the find_manifold_answers.py replacement."""
    d = resolve_date_arg(args.date) if args.date else None
    block = manifold_answers_block(
        args.slug, date_filter=d, include_resolved=args.include_resolved,
        contains=args.contains,
    )
    if not block:
        return 1
    print(block)
    return 0


def cmd_sport(args) -> int:
    cfg = SPORTS[args.sport]
    slug = extract_manifold_slug(args.manifold_slug or cfg["manifold_url"])
    dated = cfg["parser"] in DATED_PARSERS

    # --date filters the Manifold answer list (text side). For sports with no
    # date in the answer text, it doubles as the Kalshi-side game date.
    d = None
    if args.date:
        d = resolve_date_arg(args.date)
    elif dated and not args.all:
        d = date.today()
        print(f"# No --date given; defaulting to today ({d.month}/{d.day}). "
              f"Use --all to take every unresolved answer.")

    if args.stdin:
        raw = sys.stdin.read()
        print("# Reading answer lines from stdin")
    else:
        raw = manifold_answers_block(
            slug, date_filter=d if dated else None,
            include_resolved=args.include_resolved, contains=args.contains,
        )
    if not raw.strip():
        print("# No answer lines to match — nothing to do.")
        return 1

    game_date = args.game_date
    if not game_date and not dated and d:
        game_date = f"{d.month}/{d.day}"

    year = args.year or (d or date.today()).strftime("%y")

    specs = generate_job_specs(
        raw, sport=args.sport, year_two_digits=year,
        game_date=game_date, manifold_slug_override=slug,
    )
    return _emit(specs, args.sport, args)


def cmd_generic(args) -> int:
    slug = extract_manifold_slug(args.manifold)

    if args.stdin:
        raw = sys.stdin.read()
        print("# Reading answer lines from stdin")
    else:
        d = resolve_date_arg(args.date) if args.date else None
        raw = manifold_answers_block(
            slug, date_filter=d, include_resolved=args.include_resolved,
            contains=args.contains,
        )
    if not raw.strip():
        print("# No answer lines to match — nothing to do.")
        return 1

    specs = generate_generic_configs(
        kalshi_url_or_ticker=args.kalshi,
        manifold_slug=slug,
        raw_manifold_inputs=raw,
    )
    return _emit(specs, "generic", args)


def cmd_drop(args) -> int:
    removed = drop_pairs(args.target, args.config, dry_run=args.dry_run)
    if not removed:
        return 1
    if args.dry_run:
        return 0                      # remove_market_pairs already listed them
    for pair in removed:
        src = pair.get("kalshi_ticker") or pair.get("polymarket_token_id") or "?"
        print(f"#   - {pair.get('name', '?')}  [{src}]")
    print("# The trader reads its config only at startup — restart it to stop "
          "trading these pairs.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="make_configs.py",
        description="Generate Kalshi->Manifold market pairs and merge them into a bot config.",
        epilog=(
            "examples:\n"
            "  make_configs.py sport mlb --write config_websocket.yaml\n"
            "  make_configs.py sport nba --date 7/30 --dry-run --write config_websocket.yaml\n"
            "  make_configs.py generic --kalshi <kalshi-event-url> --manifold <manifold-url>\n"
            "  make_configs.py drop 2028-democratic-nominee --config config_websocket.yaml\n"
            "  make_configs.py answers major-league-baseball-live --date today\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = ap.add_subparsers(dest="command")

    def add_common(p, with_write=True):
        p.add_argument("--date", help="today|tomorrow|M/D|YYYY-MM-DD (filters Manifold answers)")
        p.add_argument("--contains", help="only answers whose text contains this")
        p.add_argument("--include-resolved", action="store_true",
                       help="keep already-resolved answers (default: drop them)")
        if with_write:
            p.add_argument("--write", metavar="CONFIG",
                           help="append pairs to this config (default: print YAML only)")
            p.add_argument("--dry-run", action="store_true",
                           help="with --write, show what would be added without writing")
            p.add_argument("--stdin", action="store_true",
                           help="read answer lines from stdin instead of the Manifold API")
            p.add_argument("--allow-conflicts", action="store_true",
                           help="write pairs that collide on a ticker/answer (default: withhold)")

    p_sport = sub.add_parser("sport", help="league games (nba/nfl/nhl/mlb/epl/ncaa/tennis)")
    p_sport.add_argument("sport", choices=sorted(SPORTS.keys()))
    p_sport.add_argument("--all", action="store_true",
                         help="every unresolved answer, not just today's (dated sports)")
    p_sport.add_argument("--game-date", metavar="M/D",
                         help="Kalshi-side game date for sports with no date in the answer text")
    p_sport.add_argument("--manifold-slug", help="override the built-in Manifold slug")
    p_sport.add_argument("--year", metavar="YY", help="2-digit year for Kalshi datecodes")
    add_common(p_sport)
    p_sport.set_defaults(func=cmd_sport)

    p_generic = sub.add_parser("generic", help="any multi-choice Kalshi event (F1, Fed, awards)")
    p_generic.add_argument("--kalshi", required=True, help="Kalshi event URL or ticker")
    p_generic.add_argument("--manifold", required=True, help="Manifold market URL or slug")
    add_common(p_generic)
    p_generic.set_defaults(func=cmd_generic)

    p_answers = sub.add_parser("answers", help="print a slug's answer block (id + text)")
    p_answers.add_argument("slug", help="Manifold market slug")
    add_common(p_answers, with_write=False)
    p_answers.set_defaults(func=cmd_answers)

    p_drop = sub.add_parser("drop", help="remove every pair for a market from a config")
    p_drop.add_argument("target",
                        help="manifold slug, Kalshi ticker or event, polymarket token, or a URL")
    p_drop.add_argument("--config", default="config_websocket.yaml",
                        help="config to edit (default: config_websocket.yaml)")
    p_drop.add_argument("--dry-run", action="store_true",
                        help="show what would be removed without writing")
    p_drop.set_defaults(func=cmd_drop)

    p_legacy = sub.add_parser("legacy", help="run the hand-edited RAW_* blocks in this file")
    p_legacy.set_defaults(func=lambda a: cmd_legacy() or 0)

    return ap


def main(argv=None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "command", None):
        ap.print_help()
        return 1
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())