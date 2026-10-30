#!/usr/bin/env python3
# Cloud page data for the GX: the leaderboard registry (a copy of the Vercel leaderboards page's BOARDS, the way that
# page keeps its own), one POST to the get-leaderboard-bundle edge function (contract: round 3 cloud_contract.md; source
# Cloud Workspace/supabase/functions/get-leaderboard-bundle), and the bundle reshaped into what the plugin draws
# (/Cloud/Json). Values arrive formatted as the web page formats them (leaderboards.js formatValue,
# readDisplayValue, unitFor); a value in F also carries the number, so the page can show it in the GX's unit.

import calendar, gzip, json, math, ssl, time, urllib.error, urllib.request

CLOUD_BASE = 'https://qnbekuaoweuteylitzvo.supabase.co/functions/v1/'
# Supabase's public anon key, the one the firmware (2_functions.ino) and the web pages already carry
CLOUD_ANON_KEY = ('eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InFuYmVrdWFvd2V1dGV5bGl0enZvIiwicm9s'
                  'ZSI6ImFub24iLCJpYXQiOjE3NTE5NzY1MzUsImV4cCI6MjA2NzU1MjUzNX0.k2S_kzkdAyN1Azs_7enxLun9LouB1bA_q7Sw8x1Cp0o')
BUNDLE_FN = 'get-leaderboard-bundle'
ROWS_LIMIT = 10      # rows per board, as the web page fetches
SPEED_LIMIT = 500
TOP_ROWS = 5         # shown per board, then the caller's own row after a gap when it is not among them

W_TO_KW = 1000.0
# (section, ((board key, title, info, ((tab label, column, unit), ...)), ...)), in the web page's order
SECTIONS = (
    ('Sailing', (
        ('undersail', 'Under Sail', '', (('Total Days', 'sailing_days_alltime', 'days'),
                                         ('Total Miles', 'sailing_dist_alltime', 'nm'),
                                         ('Streak', 'longest_consecutive_days_under_sail', 'days'))),
        ('vmg', 'Peak Upwind VMG', 'Your best speed made good straight into the true wind, recorded with the engine off: '
                                   'the part of your GPS speed over ground that points directly upwind.',
         (('', 'best_upwind_vmg_alltime', 'kts'),)),
    )),
    ('Distance', (
        ('distance', 'Distance', '', (('Best Day', 'max_24hr_distance', 'nm'), ('Lifetime', 'total_dist_alltime', 'nm'),
                                      ('Nonstop', 'longest_single_trip_nm_alltime', 'nm'))),
        ('longpassage', 'Longest Passage', 'The most consecutive days fully offshore, every GPS fix more than half a mile '
                                           'from land.', (('', 'longest_streak_days', 'days'),)),
        ('farland', 'Farthest From Land', 'The farthest your boat has been from the nearest coastline, measured straight '
                                          'line. A record of how far offshore, not of distance traveled.',
         (('', 'farthest_from_land_nm', 'nm'),)),
    )),
    ('Exploration', (
        ('countries', 'Countries Visited', "Countries whose coast you've come within 12 nm of (the territorial-sea "
                                           "limit). Your home country counts.", (('', 'countries_visited', ''),)),
        ('latitude', 'Latitudes', '', (('North', 'farthest_lat_north', '°N'), ('South', 'farthest_lat_south', '°S'),
                                       ('Max Range', 'latitude_max_range', '°'))),
        ('dayscruising', 'Cruising Days', 'Days on which every GPS fix was more than half a mile from land. A quick out '
                                          "and back from port doesn't count.", (('', 'days_at_sea_alltime', 'days'),)),
    )),
    ('Energy', (
        ('solar', 'Solar', '', (('Max kW', 'solar_power_max_alltime_w', 'kW'), ('Best Day', 'max_daily_solar_kwh', 'kWh'),
                                ('Lifetime', 'solar_kwh_alltime', 'kWh'))),
        ('alternator', 'Alternator', '', (('Max A', 'alt_curr_max_alltime', 'A'),
                                          ('Max kW', 'alt_power_max_alltime_w', 'kW'),
                                          ('Best Day', 'max_daily_alt_kwh', 'kWh'),
                                          ('Lifetime', 'alt_charged_energy_alltime', 'kWh'))),
    )),
    ('Engine', (
        ('enginehours', 'Engine Hours', '', (('', 'eng_hrs', 'hrs'),)),
        ('enginerpm', 'Engine Speed', 'Engine RPM records. High Avg and Low Avg are averaged only while the engine runs.',
         (('Max', 'rpm_max_alltime', 'RPM'), ('High Avg', 'rpm_avg_max_alltime', 'RPM'),
          ('Low Avg', 'rpm_avg_min_alltime', 'RPM'))),
        ('nmpergallon', 'Best nM per Gallon', 'Engine On counts only miles made under power. Total counts every mile '
                                              'against fuel burned, so sailboats score high here.',
         (('Engine On', 'nm_per_gallon_engine_on', 'mpg'), ('Total', 'best_nm_per_gallon', 'mpg'))),
    )),
    ('Weather Extremes', (
        ('gale', 'Longest Gale', '', (('', 'longest_gale_duration_hours_alltime', 'hr'),)),
        ('boardtemp', 'Board Temperature', '', (('High', 'board_temp_max_alltime', '°F'),
                                                ('Low', 'board_temp_min_alltime', '°F'))),
        ('pressure', 'Barometric Pressure', '', (('High', 'baro_pressure_max_alltime', 'mbar'),
                                                 ('Low', 'baro_pressure_min_alltime', 'mbar'))),
    )),
    ('Anchoring', (
        ('deepanchor', 'Deepest Anchorage', '', (('', 'deepest_anchorage_ft', 'ft'),)),
        ('consecanchor', 'Consecutive Days at Anchor', '', (('', 'consecutive_days_at_anchor', 'days'),)),
    )),
)
# The standings chips' board names (leaderboards.js BOARD_NAMES)
BOARD_NAMES = {'undersail': 'Under sail', 'vmg': 'Upwind VMG', 'distance': 'Distance', 'longpassage': 'Longest passage',
               'farland': 'Farthest from land', 'countries': 'Countries', 'latitude': 'Latitude',
               'dayscruising': 'Cruising days', 'solar': 'Solar', 'alternator': 'Alternator', 'enginehours': 'Engine hours',
               'enginerpm': 'Engine speed', 'nmpergallon': 'nM per gallon', 'gale': 'Longest gale',
               'boardtemp': 'Board temp', 'pressure': 'Pressure', 'deepanchor': 'Deepest anchorage',
               'consecanchor': 'Days at anchor'}


def columns():
    return [c for _s, boards in SECTIONS for _k, _t, _i, tabs in boards for _l, c, _u in tabs]


def display_value(column, unit, raw):
    # leaderboards.js readDisplayValue + transform: engine and alternator hours are stored in seconds, kW boards in W
    if raw is None or isinstance(raw, bool) or not isinstance(raw, (int, float)) or not math.isfinite(raw):
        return None
    if column in ('eng_hrs', 'alt_hrs'):
        return raw / 3600.0
    if unit == 'kW':
        return raw / W_TO_KW
    return float(raw)


def unit_for(v, unit):
    # a latitude value carries its hemisphere by sign, on either board
    if unit in ('°N', '°S') and v is not None:
        return '°S' if v < 0 else '°N'
    return unit


def _whole(v):
    return '{:,}'.format(int(math.floor(v + 0.5)))


def format_value(v, unit):
    # leaderboards.js formatValue, with the unit appended (no space before a degree sign)
    if v is None:
        return '---'
    if unit in ('°N', '°S'):
        s = '%.1f' % abs(v)
    elif unit == 'kW':
        s = '%.2f' % v
    elif unit in ('RPM', 'A', 'days', 'hrs', '', 'ft', 'nm', 'kWh'):
        s = _whole(v)
    elif unit in ('kts', '°', 'kWh/day', 'hr', '%', 'mpg', 'nm/hr', 'nm/kWh', '°F', 'mbar'):
        s = '%.1f' % v
    else:
        s = '%.2f' % v
    u = unit_for(v, unit)
    return s + ('' if not u else u if u.startswith('°') else ' ' + u)


def _details(p):
    out = []
    if p.get('boat_type') and p.get('boat_length_ft'):
        out.append('%s · %s ft' % (str(p['boat_type']).capitalize(), p['boat_length_ft']))
    mm = ' '.join(str(x) for x in (p.get('boat_year'), p.get('boat_make_model')) if x)
    if p.get('boat_make_model') and mm:
        out.append(mm)
    return '\n'.join(out)


def view(bundle, now=None):
    # The edge function's answer -> the page's model. Raises ValueError when the answer is not a bundle.
    if not isinstance(bundle, dict) or not isinstance(bundle.get('boards'), dict):
        raise ValueError('not a leaderboard bundle')
    profiles = bundle.get('profiles') if isinstance(bundle.get('profiles'), dict) else {}

    def row(rank, e, column, unit):
        p = profiles.get(str(e.get('p'))) if isinstance(e, dict) else None
        if not isinstance(p, dict) or not p.get('username'):
            return None
        v = display_value(column, unit, e.get('v'))
        r = {'r': rank, 'n': str(p['username']), 'you': int(bool(e.get('you'))), 't': format_value(v, unit),
             'd': _details(p), 'v': v}
        if unit == '°F':
            r['f'] = 1
        return r

    sections, standings = [], []
    for title, boards in SECTIONS:
        sec = {'title': title, 'boards': []}
        for key, btitle, info, tabs in boards:
            b = {'key': key, 'title': btitle, 'info': info, 'tabs': []}
            for label, column, unit in tabs:
                d = bundle['boards'].get(column) if isinstance(bundle['boards'].get(column), dict) else {}
                # top rows are numbered by position (listHtml); a column that failed ({error}) shows as no data
                top = [x for x in (row(i + 1, e, column, unit) for i, e in enumerate(d.get('top') or [])) if x]
                rows = top[:TOP_ROWS]
                mine = next((x for x in top if x['you']), None)
                gap = False
                if mine is None or mine['r'] > TOP_ROWS:
                    # the caller's own row under its board rank (ties share one, so it can differ from position)
                    rank, v = d.get('rank'), display_value(column, unit, d.get('value'))
                    you = bundle.get('you') if isinstance(bundle.get('you'), dict) else None
                    if isinstance(rank, int) and rank > 0 and v is not None and you and you.get('username'):
                        mine = {'r': rank, 'n': str(you['username']), 'you': 1, 't': format_value(v, unit),
                                'd': _details(you), 'v': v}
                        if unit == '°F':
                            mine['f'] = 1
                    if mine is not None:
                        rows.append(mine)
                        gap = True
                peak = max([abs(x['v']) for x in rows if x['v'] is not None] or [0]) or 1
                for x in rows:
                    x['w'] = round(abs(x['v']) / peak, 3) if x['v'] is not None else 0
                b['tabs'].append({'label': label, 'unit': unit, 'rows': rows, 'gap': int(gap)})
                if mine is not None and mine['v'] is not None:
                    standings.append({'rank': mine['r'], 'label': BOARD_NAMES[key] + (' · ' + label if label else ''),
                                      't': mine['t'], 'v': mine['v'], 'f': mine.get('f', 0)})
            sec['boards'].append(b)
        sections.append(sec)
    standings.sort(key=lambda s: s['rank'])

    speed = []
    for e in bundle.get('speed') or []:
        p = profiles.get(str(e.get('p'))) if isinstance(e, dict) else None
        v = e.get('v') if isinstance(e, dict) else None
        if not isinstance(p, dict) or not isinstance(v, (int, float)) or not v:
            continue
        try:
            length = float(p.get('boat_length_ft'))
        except (TypeError, ValueError):
            continue
        if not p.get('boat_type') or not length:
            continue
        speed.append({'n': str(p.get('username') or ''), 'len': length, 'v': float(v), 'type': str(p['boat_type']).lower(),
                      'boat': ' '.join(str(x) for x in (p.get('boat_year'), p.get('boat_make_model')) if x),
                      'you': int(bool(e.get('you')))})

    fleet = bundle.get('fleet') if isinstance(bundle.get('fleet'), dict) else {}
    pulse = []
    if fleet.get('totalDevices'):
        pulse.append('%s boats' % _whole(fleet['totalDevices']))
    if isinstance(fleet.get('totalDistance'), (int, float)):
        pulse.append('%s nm logged' % _whole(fleet['totalDistance']))
    energy = (fleet.get('altEnergy') or 0) + (fleet.get('solarEnergy') or 0)
    if energy > 0:
        pulse.append('%s kWh generated' % _whole(energy / 1000.0))
    you = bundle.get('you') if isinstance(bundle.get('you'), dict) else None
    return {'asOf': _epoch(bundle.get('as_of'), now), 'you': {'n': str(you.get('username') or ''), 'd': _details(you)}
            if you else None, 'pulse': ' · '.join(pulse), 'standings': standings[:6], 'sections': sections,
            'speed': speed, 'fleet': fleet}


def _epoch(v, now):
    if isinstance(v, (int, float)) and v > 0:
        return int(v / 1000 if v > 1e11 else v)
    if isinstance(v, str):
        try:
            return calendar.timegm(time.strptime(v[:19], '%Y-%m-%dT%H:%M:%S'))   # ISO-8601 UTC
        except ValueError:
            pass
    return int(now if now is not None else time.time())


def opener():
    # The cloud's own opener: default certificate checks, no proxy (unlike the LAN opener for the regulator)
    return urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                       urllib.request.HTTPSHandler(context=ssl.create_default_context()))


def fetch(token, base=None, timeout=15.0, open_=None):
    # One POST for every board, the speed scatter and the fleet stats. Raises urllib.error.HTTPError (an answer),
    # OSError/URLError (no internet), ValueError (an unreadable answer).
    body = json.dumps({'token': token, 'columns': columns(), 'limit': ROWS_LIMIT,
                       'speed_limit': SPEED_LIMIT}).encode()
    req = urllib.request.Request((base or CLOUD_BASE) + BUNDLE_FN, data=body, method='POST', headers={
        'Content-Type': 'application/json', 'Accept-Encoding': 'gzip', 'apikey': CLOUD_ANON_KEY,
        'Authorization': 'Bearer ' + CLOUD_ANON_KEY})
    with (open_ or opener()).open(req, timeout=timeout) as r:
        raw = r.read()
        if (r.headers.get('Content-Encoding') or '').lower() == 'gzip':
            raw = gzip.decompress(raw)
        return json.loads(raw)


def http_error_body(e):
    # (code from the body or '', message): 401 token_not_found means the regulator's token is gone from the cloud
    try:
        j = json.loads(e.read())
    except Exception:
        j = None
    if isinstance(j, dict):
        return str(j.get('code') or ''), str(j.get('error') or j.get('message') or '')[:200]
    return '', ''


# ---- Demo mode: a canned bundle in the edge function's shape. The same fleet feeds the web app's demo Cloud Features.
# Every board comes from one model per boat (where it cruises, how long it has logged, its engine, alternator, array
# and hull), so a boat's records agree with each other: its passages, latitudes, countries and distances tell one
# story, speeds follow length and hull type, kW follows amps and system voltage. Small per-value jitter keeps the
# numbers from looking computed.

# name, type, LOA ft, make/model, year, years logged, days at sea per year, share of sea days under sail, nm per sailing
# day, engine hours per year, farthest N, farthest S (signed), countries, longest offshore passage
# days, longest run under sail days, longest nonstop nm, farthest from land nm, longest gale h (None: never), lowest
# and highest barometer mbar, board temperature high and low F, deepest anchorage ft, most consecutive days at anchor,
# solar array W, alternator peak A, system volts, max / cruise / idle RPM, cruise gal/h, cruise kn, best-day sun hours,
# sustained top speed as a share of hull speed
_DEMO_FLEET_BOATS = (
    ('Meridian', 'monohull', 44, 'Hallberg-Rassy 43', 2008, 2.3, 104, 0.74, 103, 236, 42.3, -23.1, 19, 26, 23, 3012,
     1178, 31, 978, 1034, 127, 52, 112, 41, 820, 168, 12, 3480, 2310, 830, 0.82, 6.6, 6.1, 1.21),
    ('Blue Heron', 'monohull', 38, 'Sabre 38', 1989, 1.4, 21, 0.68, 41, 97, 44.6, 40.9, 2, 2, 1, 137, 38, 4, 996, 1033,
     109, 44, 37, 4, 280, 92, 12, 3260, 2190, 850, 0.71, 6.2, 5.2, 1.07),
    ('Wanderer', 'catamaran', 45, 'Leopard 45', 2019, 1.9, 96, 0.61, 103, 312, 26.8, 12.1, 9, 6, 5, 948, 214, 13, 993,
     1031, 131, 71, 63, 47, 1960, 205, 24, 3410, 2480, 880, 1.34, 7.1, 6.7, 1.58),
    ('DemoBoat', 'monohull', 40, 'Pacific Seacraft 40', 2004, 1.7, 34, 0.71, 58, 141, 45.1, 32.3, 3, 5, 4, 668, 287, 9,
     991, 1037, 118, 39, 58, 6, 555, 104, 12, 3180, 2250, 820, 0.68, 6.4, 5.6, 1.13),
    ('Sea Smoke', 'trawler', 42, 'Nordhavn 43', 2006, 2.1, 61, 0.0, 0, 588, 58.4, 47.6, 2, 4, 0, 517, 46, 7, 987, 1036,
     122, 37, 74, 11, 960, 238, 24, 2480, 1720, 760, 2.9, 7.6, 5.0, 1.04),
    ('Halcyon', 'monohull', 36, 'Tartan 3700', 2002, 1.2, 26, 0.77, 44, 74, 45.9, 41.6, 2, 2, 2, 168, 22, None, 1001,
     1034, 104, 47, 33, 3, 340, 88, 12, 3320, 2240, 870, 0.62, 6.3, 5.0, 1.09),
    ('Kestrel', 'catamaran', 40, 'Lagoon 400', 2014, 1.6, 74, 0.52, 81, 284, 28.9, 21.7, 3, 3, 2, 206, 61, 6, 997, 1029,
     129, 68, 29, 38, 1420, 158, 12, 3370, 2380, 860, 1.12, 6.8, 6.4, 1.47),
    ('Northern Light', 'monohull', 47, 'Valiant 47', 1983, 2.4, 63, 0.66, 96, 207, 78.2, 41.4, 7, 9, 7, 1183, 338, 22,
     974, 1039, 112, 33, 104, 9, 610, 142, 12, 3290, 2210, 810, 0.88, 6.5, 4.4, 1.16),
    ('Osprey', 'powerboat', 34, 'Back Cove 34', 2017, 1.3, 24, 0.0, 0, 163, 44.4, 41.2, 2, 1, 0, 143, 31, None, 1004,
     1033, 123, 51, 46, 2, 0, 196, 12, 3920, 3140, 760, 17.4, 23.5, 4.9, 2.86),
    ('Tern', 'monohull', 33, 'Cape Dory 33', 1981, 0.9, 19, 0.82, 36, 58, 39.6, 36.9, 1, 2, 2, 109, 18, None, 1003,
     1032, 101, 52, 24, 5, 210, 61, 12, 3460, 2330, 900, 0.54, 5.6, 5.3, 1.03),
    ('Moonshadow', 'monohull', 52, 'Amel 54', 2011, 2.2, 112, 0.69, 112, 294, 12.4, -54.9, 27, 22, 19, 2647, 962, 38,
     969, 1031, 124, 36, 131, 29, 1240, 214, 24, 3060, 2160, 790, 1.09, 7.2, 5.9, 1.19),
    ('Avocet', 'trawler', 37, 'Grand Banks 36', 1998, 1.8, 72, 0.0, 0, 431, 41.7, 24.6, 2, 2, 0, 184, 49, 5, 994, 1032,
     126, 54, 22, 17, 640, 132, 12, 2560, 1820, 740, 2.4, 7.4, 6.0, 1.05),
    ('Second Wind', 'monohull', 30, 'Catalina 30', 1986, 1.1, 14, 0.79, 27, 38, 43.2, 41.8, 1, 1, 1, 64, 9, None, 1006,
     1030, 99, 55, 19, 2, 0, 52, 12, 3510, 2440, 920, 0.42, 5.5, 5.1, 1.02),
    ('Sundowner', 'powerboat', 28, 'Cutwater C-28', 2020, 0.8, 18, 0.0, 0, 112, 48.9, 47.1, 2, 1, 0, 96, 12, None, 1008,
     1029, 119, 49, 41, 2, 180, 118, 12, 3830, 3010, 720, 12.6, 21.2, 4.6, 2.74),
    ('Larkspur', 'catamaran', 47, 'Fountaine Pajot Saona 47', 2021, 1.3, 104, 0.64, 112, 266, 38.9, 13.2, 6, 8, 7, 1391,
     418, 17, 985, 1035, 133, 66, 67, 33, 2140, 229, 24, 3360, 2410, 850, 1.38, 7.3, 6.5, 1.71),
    ('Brightwork', 'monohull', 41, 'Swan 411', 1977, 1.5, 47, 0.83, 84, 109, 47.8, 36.6, 4, 4, 4, 694, 214, 14, 988,
     1036, 108, 46, 48, 7, 360, 86, 12, 3540, 2290, 860, 0.71, 6.4, 5.4, 1.27),
)
# boards a boat has no data on are left out of them, as the cloud leaves no-data rows out: sailing boards for boats
# that never sail, solar for boats without an array, the gale board for boats that never logged one
_DEMO_LOW = ('farthest_lat_south', 'rpm_avg_min_alltime', 'board_temp_min_alltime', 'baro_pressure_min_alltime')
_DEMO_YOU = 'DemoBoat'
_DEMO_OTHERS = 1.68   # fleet totals against these 16 public boats: the 31 private ones log about a third as much each


def _hash01(*xs):
    h = 2166136261
    for c in ''.join(str(x) for x in xs).encode():
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h / 4294967296.0


def _jit(v, key, spread=0.03):
    # a value's own small, repeatable wobble, so derived numbers do not look computed
    return v * (1 + spread * (2 * _hash01(key) - 1))


def _demo_boat(b):
    (name, kind, loa, make, year, yrs, sea_yr, sail_frac, sail_nm, eng_yr, lat_n, lat_s, countries, streak,
     sail_streak, nonstop, far_land, gale, baro_lo, baro_hi, temp_hi, temp_lo, anchor_ft, anchor_days, solar_w, alt_a,
     volts, rpm_max, rpm_cruise, rpm_idle, gph, cruise_kn, sun_h, top) = b
    sail = kind in ('monohull', 'catamaran')
    hull = 1.34 * math.sqrt(loa * 0.9)
    sea = sea_yr * yrs
    sail_days = sea * sail_frac
    sail_dist = sail_days * sail_nm
    eng_h = eng_yr * yrs
    # under power: most engine hours move the boat at cruise; a sailboat's include charging at anchor and harbour work
    total = sail_dist + eng_h * cruise_kn * (0.88 if not sail else 0.61)
    charge_v = 14.1 if volts == 12 else 28.2
    fuel_gal = eng_h * gph * 0.93
    k = lambda col: name + col
    v = {
        'sailing_days_alltime': round(sail_days) if sail else None,
        'sailing_dist_alltime': round(_jit(sail_dist, k('sd')), 1) if sail else None,
        'longest_consecutive_days_under_sail': sail_streak if sail else None,
        'best_upwind_vmg_alltime': round(_jit(hull * (0.58 if kind == 'catamaran' else 0.69) * (top / 1.15) ** 0.5,
                                             k('vmg'), 0.04), 2) if sail else None,
        'max_24hr_distance': round(_jit(24 * (hull * (0.86 if kind == 'catamaran' else 0.74) if sail
                                              else cruise_kn * (0.39 if kind == 'powerboat' else 0.96)), k('24'), 0.05), 1),
        'total_dist_alltime': round(total, 1),
        'longest_single_trip_nm_alltime': nonstop,
        'longest_streak_days': streak,
        'farthest_from_land_nm': far_land,
        'countries_visited': countries,
        'farthest_lat_north': lat_n,
        'farthest_lat_south': lat_s,
        'latitude_max_range': round(lat_n - lat_s, 1),
        'days_at_sea_alltime': round(sea),
        'solar_power_max_alltime_w': round(_jit(solar_w * 0.84, k('sp'), 0.05)) if solar_w else None,
        'max_daily_solar_kwh': round(_jit(solar_w / 1000 * sun_h * 0.81, k('sd2'), 0.05), 2) if solar_w else None,
        'solar_kwh_alltime': round(_jit(solar_w / 1000 * sun_h * 0.43 * (sea_yr * 1.7 + 40) * yrs, k('sl'), 0.06), 1)
        if solar_w else None,
        'alt_curr_max_alltime': round(_jit(alt_a, k('am'), 0.02), 1),
        'alt_power_max_alltime_w': round(_jit(alt_a * charge_v, k('aw'), 0.02)),
        # best day: full output for a few hours of bulk; lifetime: the average once the bank is up is mostly loads
        'max_daily_alt_kwh': round(_jit(alt_a * charge_v / 1000 * {'trawler': 3.1, 'catamaran': 2.6, 'monohull': 2.4,
                                                                    'powerboat': 1.9}[kind], k('ad'), 0.07), 2),
        'alt_charged_energy_alltime': round(_jit(eng_h * alt_a * (0.19 if sail else 0.14) * charge_v / 1000, k('al'),
                                                 0.05), 1),
        'eng_hrs': round(eng_h * 3600 * _jit(1, k('eh'), 0.02)),
        'rpm_max_alltime': rpm_max,
        'rpm_avg_max_alltime': rpm_cruise,
        'rpm_avg_min_alltime': rpm_idle,
        'nm_per_gallon_engine_on': round(_jit(cruise_kn / gph, k('mpg'), 0.04), 2),
        'best_nm_per_gallon': round(total / fuel_gal, 2),
        'longest_gale_duration_hours_alltime': gale,
        'board_temp_max_alltime': temp_hi + round(_hash01(k('th')) * 0.9, 1),
        'board_temp_min_alltime': temp_lo + round(_hash01(k('tl')) * 0.9, 1),
        'baro_pressure_max_alltime': baro_hi + round(_hash01(k('bh')) * 0.9, 1),
        'baro_pressure_min_alltime': baro_lo + round(_hash01(k('bl')) * 0.9, 1),
        'deepest_anchorage_ft': anchor_ft,
        'consecutive_days_at_anchor': anchor_days,
    }
    speed = round(_jit(hull * top if kind != 'powerboat' else cruise_kn * top / 2.3, k('sp2'), 0.03), 2)
    totals = {'dist': total, 'alt_wh': v['alt_charged_energy_alltime'] * 1000,
              'solar_wh': (v['solar_kwh_alltime'] or 0) * 1000, 'eng_h': eng_h, 'fuel_l': fuel_gal * 3.785,
              'alt_fuel_l': v['alt_charged_energy_alltime'] * 0.31, 'moving_h': eng_h * 0.93 + sail_days * 15}
    return v, speed, totals


def demo_bundle(now=None):
    now = int(now if now is not None else time.time())
    profiles, values, speeds, totals = {}, {}, {}, []
    for i, b in enumerate(_DEMO_FLEET_BOATS):
        key = str(i)
        profiles[key] = {'username': b[0], 'boat_type': b[1], 'boat_length_ft': b[2], 'boat_make_model': b[3],
                         'boat_year': b[4]}
        values[key], speeds[key], t = _demo_boat(b)
        totals.append(t)
    you = next(k for k, p in profiles.items() if p['username'] == _DEMO_YOU)
    boards = {}
    for column in columns():
        rows = [(k, v[column]) for k, v in values.items() if v[column] is not None]
        low = column in _DEMO_LOW
        rows.sort(key=lambda r: r[1], reverse=not low)
        mine = values[you][column]
        # the RPC's rule: count of strictly better + 1, so a tie shares a rank
        rank = None if mine is None else 1 + sum(1 for _k, x in rows if (x < mine if low else x > mine))
        boards[column] = {'top': [dict({'p': k, 'v': x}, **({'you': True} if k == you else {})) for k, x in rows[:10]],
                          'rank': rank, 'value': mine}
    speed = sorted(({'p': k, 'v': x} for k, x in speeds.items()), key=lambda e: -e['v'])
    for e in speed:
        if e['p'] == you:
            e['you'] = True
    tot = lambda f: sum(t[f] for t in totals) * _DEMO_OTHERS
    alt_wh, solar_wh = tot('alt_wh'), tot('solar_wh')
    charged = (alt_wh + solar_wh) * 0.913
    fleet = {'totalDevices': 47, 'activeDevices': 29, 'avgVoltage': 13.31, 'avgSoc': 81.7,
             'totalDistance': round(tot('dist'), 1), 'avgSpeed': round(tot('dist') / tot('moving_h'), 2),
             'batteryCharged': round(charged), 'discharged': round(charged * 0.884), 'altEnergy': round(alt_wh),
             'solarEnergy': round(solar_wh), 'altFuel': round(tot('alt_fuel_l'), 1),
             'altTime': round(tot('eng_h') * 0.83, 1), 'engineHours': round(tot('eng_h'), 1),
             'engineFuel': round(tot('fuel_l'), 1),
             'calculatedAt': time.strftime('%Y-%m-%dT08:00:00.000000+00:00', time.gmtime(now))}
    you_p = dict(profiles[you], p=you)
    return {'as_of': time.strftime('%Y-%m-%dT%H:%M:%S.000Z', time.gmtime(now)), 'you': you_p, 'profiles': profiles,
            'boards': boards, 'speed': speed, 'fleet': fleet}
