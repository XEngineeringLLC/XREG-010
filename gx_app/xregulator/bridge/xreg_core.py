#!/usr/bin/env python3
# XREG-010 bridge logic with no D-Bus dependency. A regulator is identified by its uid, never by its address: Discovery
# finds every regulator on the GX's networks, the Manager holds the ones the owner picked (each pinned by uid), and each
# of those has a Core: the link (found by uid; event stream in, writes out), the recorders and decoders behind every
# published path, and the settings model. xregulator_bridge.py maps the Manager and each Core onto their own D-Bus
# services; the offline tests drive them with dicts in their place.

import collections, concurrent.futures, gzip, http.client, ipaddress, json, logging, math, os, queue, random, re
import secrets, socket, struct, subprocess, threading, time
import urllib.error, urllib.parse, urllib.request

import xreg_cloud as cloud

log = logging.getLogger('xregulator')

VERSION = '0.2'
CACHE_DIR = '/data/xregulator'   # outside the app folder: push_to_gx.sh mirrors that one with --delete
EMPTY = -1999999999              # wire values at or below this mean "no reading" (firmware ROLL_EMPTY)

# SSE event -> the web app's field list for it, read from the served script.js
STREAMS = {'CSVData': 'CSV1_FIELDS', 'CSVData2': 'CSV2_FIELDS', 'CSVData3': 'CSV3_FIELDS', 'CSVData4': 'CSV4_FIELDS',
           'TimestampData': 'TS_FIELDS'}
# names from /altschema and /perfschema
SCHEMA_EVENTS = ('AltLive', 'PerfLive', 'MotorLive', 'AltSettings', 'PerfSettings')
# /events (the phone stream): CSV1 10 Hz, CSV4 2 Hz, CSV2 every 5 s, CSV3 on change or every 60 s, TS every 3 s,
# schema events ~1 Hz
EVENTS_FRESH_S = {'CSVData': 5.0, 'CSVData4': 5.0, 'CSVData2': 15.0, 'CSVData3': 150.0, 'TimestampData': 10.0,
                  'AltLive': 10.0, 'PerfLive': 10.0, 'MotorLive': 10.0, 'AltSettings': 60.0, 'PerfSettings': 60.0}
# /gxevents before its first GxRate event: assume the slowest (ignition-off nap) schedule, ms
GX_NAP_RATE = {'csv1': 10000, 'csv4': 30000, 'csv2': 60000, 'csv3': 300000, 'ts': 60000}
RATE_KEY = {'CSVData': 'csv1', 'CSVData4': 'csv4', 'CSVData2': 'csv2', 'CSVData3': 'csv3', 'TimestampData': 'ts'}

F_TO_C = lambda f: (f - 32.0) * 5.0 / 9.0
# /Live/<name>: (event, field, wire scale, conversion). Temperatures in Celsius, the D-Bus convention.
LIVE = (
    ('AltAmps',     'CSVData',  'MeasuredAmps',           100, None),   # the tile's alternator instead while CSV4 has it
    ('Field',       'CSVData',  'dutyCycle',              100, None),
    ('BattV',       'CSVData',  'IBV',                    100, None),
    ('BattA',       'CSVData',  'Bcur',                   100, None),   # the tile's battery instead while CSV4 has it (_flow)
    ('Rpm',         'CSVData',  'RPM',                    1,   None),
    ('AltTemp',     'CSVData',  'AlternatorTemperatureF', 100, F_TO_C),
    ('FieldV',      'CSVData',  'vvout',                  100, None),
    ('FieldA',      'CSVData',  'iiout',                  100, None),
    ('VoltTarget',  'CSVData',  'voltageTarget',          100, None),
    ('AmpTarget',   'CSVData',  'setpointLimited',        100, None),
    ('Ignition',    'CSVData',  'Ignition',               1,   None),
    ('OnOff',       'CSVData',  'OnOff',                  1,   None),
    ('FieldStatus', 'CSVData',  'fieldActiveStatus',      1,   None),
    ('OffReason',   'CSVData',  'fieldEventReason',       1,   None),
    ('Alarm',       'CSVData',  'Alarm_Status',           1,   None),
    ('Limiter',     'CSVData4', 'ctrlLimiter',            1,   None),
    ('Stage',       'CSVData4', 'chargeStage',            1,   None),
    ('Soc',         'CSVData2', 'SOC_percent',            100, None),
    ('BattTemp',    'CSVData2', 'battTempActiveF',        10,  F_TO_C),
    ('Baro',        'CSVData2', 'baroPressure',           10,  None),
    ('TrueWind',    'CSVData4', 'TrueWindSpeedNMEA',      100, None),
    ('TrueWindAngle', 'CSVData4', 'TrueWindAngleNMEA',    1,   None),
    ('AppWind',     'CSVData4', 'ApparentWindSpeedNMEA',  100, None),
    ('Health',      'AltLive',  'latchPct',               1,   None),   # 0 = nothing graded this boot (app shows "-")
    # External charge-limit follow: 0 off, 1 waiting, 2 settling, 3 following, 4 stale, 5 untrusted; merged limits;
    # each Victron carrier's age in ms (999999 = never heard)
    ('DvccState',   'CSVData2', 'dvccState',              1,   None),
    ('DvccUntrust', 'CSVData2', 'dvccUntrustReason',      1,   None),
    ('DvccCvl',     'CSVData2', 'dvccRxCvl',              100, None),
    ('DvccCcl',     'CSVData2', 'dvccRxCcl',              10,  None),
    ('DvccCanAge',  'TimestampData', 'ts_DvccVeCan',      1,   None),
    ('DvccWifiAge', 'TimestampData', 'ts_DvccWifi',       1,   None),
)
LIVE_WIND = ('TrueWind', 'TrueWindAngle', 'AppWind')
# Apparent wind's age on the TimestampData stream, ms at that frame (999999 never seen). Older than the firmware's own
# DATA_TIMEOUT it is no reading: firmware before 2026-10-29 sends a never-seen apparent wind as 0, not the empty marker.
WIND_TS = {'ApparentWindSpeedNMEA': 'ts_AppWindSpeed', 'ApparentWindAngleNMEA': 'ts_AppWindAngle'}
WIND_STALE_MS = 10000
HIST_EVERY_S = 2
HIST_LEN = 150       # 5 minutes at 2 s
HISTORY = (('AltAmps', 1), ('BattV', 2), ('Field', 0))   # /History/<name> = comma-separated samples, oldest first
WIND_EVERY_S = 30
WIND_LEN = 120       # /History/<name>: 1 hour of 30 s means of each wind speed below
# (path name, /Live name, demo backfill field): Wind is true wind speed, AppWind apparent (the Dashboard's wind tile)
WIND_HIST = (('Wind', 'TrueWind', 'TrueWindSpeedNMEA'), ('AppWind', 'AppWind', 'ApparentWindSpeedNMEA'))
BARO_FETCH_S = 600   # the regulator stores one barometer sample every 10 minutes
BARO_KEEP = 432      # /History/Baro: 3 days, newest last; /History/BaroEnd is the newest sample's epoch

# /Plot/Short/<name>: (CSV1 field, wire scale, conversion, decimals); 1 s means, last 300
SHORT = (
    ('MeasuredAmps',           'MeasuredAmps',           100, None,   1),
    ('Bcur',                   'Bcur',                   100, None,   1),
    ('iiout',                  'iiout',                  100, None,   2),
    ('dutyCycle',              'dutyCycle',              100, None,   1),
    ('pidAltPV',               'pidAltPV',               100, None,   1),
    ('BatteryV',               'BatteryV',               100, None,   2),
    ('IBV',                    'IBV',                    100, None,   2),
    ('RPM',                    'RPM',                    1,   None,   0),
    ('AlternatorTemperatureC', 'AlternatorTemperatureF', 100, F_TO_C, 1),
)
SHORT_LEN = 300
SHORT_PUB_S = 2
# /Plot/Wind/<name>: 10 s means over the last hour; angles as vector means
WIND_PLOT = (('Tws', 'TrueWindSpeedNMEA', 100, False), ('Twa', 'TrueWindAngleNMEA', 1, True),
             ('Aws', 'ApparentWindSpeedNMEA', 100, False), ('Awa', 'ApparentWindAngleNMEA', 1, True))
WIND_PLOT_S = 10
WIND_PLOT_LEN = 360

WANT_TOPICS = ('Short', 'Long', 'Health', 'Wind', 'Perf', 'Settings', 'Flow', 'Cloud')
WANT_HOLD_S = 45.0
FAST_TOPICS = ('Short', 'Wind', 'Perf', 'Health')   # pages that hold the regulator's engine-running GX schedule
PERF_FAST_KN = 1.0   # the home page always wants Perf; below this SOG and STW, it must not hold the fast schedule at anchor
FAST_EVERY_S = 30
LONG_RANGES = {'1h': 3600, '6h': 21600, '24h': 86400, '7d': 604800, '30d': 2592000}
LONG_POINTS = 300
LONG_REFETCH_S = 600
LONG_REUSE_S = 60    # a range change re-bins the cached ring when it is younger than this, then refetches
HEALTH_REFETCH_S = 300
# The plugin's Health window buttons, engine-hours (0 = the whole history). The regulator thins /alttrend.csv to 200
# points within the window, so the bridge keeps a trend per window: two screens at different windows each get their own.
HEALTH_WINDOWS = (10, 100, 0)
DEMO_RETRY_S = 5     # /demoBackfill answers 404 until the regulator has finished entering demo
DEMO_TRIES = 6       # fetches per entry or reconnect before the bridge gives up on it
CURVE_REFETCH_S = 600
SOURCE_POLL_S = 5    # an endpoint without its own pollS in the schema
# After a GX write the bridge shows the written value until the regulator's echo matches it. The write itself may take
# WRITE_WAIT_S (a control write's three 1.7 s tries, a setting's 8 s timeout, writes queued ahead of it). Once the
# regulator accepted it, the echo gets the interval of the channel carrying it plus CONFIRM_MARGIN_S, and the bridge
# goes back to the echo only after that channel has delivered since the acceptance. Round 8 bench: OnOff came back up
# to 4.6 s after a write, in a CSV1 sent every 5 s with the engine stopped.
WRITE_WAIT_S = 20.0
CONFIRM_MARGIN_S = 3.0

# Home page power flow (/Flow/*), from the CSV4 group flowBattA / flowAltA / flowSolarA (A x100) and flowSrc, plus the
# GX's own devices. Every value is amps at the battery bus: sources and loads >= 0, battery + charging; House is
# sources - battery - measured loads, or, on a boat with a whole-DC-system meter (com.victronenergy.dcsystem), that
# meter plus the sources and less the loads whose current passes through it (through_meter: the owner's word on the
# Power flow devices page, else Victron's layout).
# /Flow/Json, while the tile asks (/Ui/Want/Flow), else '':
#   {"b": {"a": battery A or null, "st": 1 charging / 0 resting / -1 discharging, "from": Shunt | NMEA 2000 | VE.Direct
#          | GX, "lim": the battery limit holding it or null},
#    "rows": [{"id", "k": kind, "a": A or null (not measured), "c": caption, "in": 1 a row whose current passes through
#          the DC system meter, only while House is metered}], sources in FLOW_SOURCE_KINDS order, then the loads; the
#          tile adds House,
#    "house": A or null, "hm": 1 House from the DC system meter, 0 calculated,
#    "why": the footnote's reason: what keeps House unknown ('batt', the kind of the first row not measured), or an
#          imbalance ('unmeasured': the battery and loads take more than the sources give; 'gap': with the meter, the
#          sources give more than the battery, the loads and the meter take; either only once it has held
#          FLOW_IMBALANCE_S), or '',
#    "x": that imbalance, A (calculated House: the unmeasured source's least amps), "fs": full bar, A (the alternator's
#          maximum) or null}
# /Flow/Ok: 1 the regulator sends the flow group, 0 it does not (older firmware), None no stream.
FLOW_SRC_MIN_A = 0.5    # a source or load under this shows 0 (Hall noise at zero)
FLOW_FLIP_A = 0.3       # the battery word changes only past this, held for FLOW_FLIP_S
FLOW_FLIP_S = 2.0
FLOW_UNMEASURED_A = 1.0   # more into the battery than the sources give, past this: an unmeasured source (sensor offsets)
# The DC system meter and the calculated House are two measurements of one thing; they may differ by this much before the
# footnote names the difference: every sensor's offset, gain errors that grow with the current, and GX readings up to a
# second apart from the regulator's
FLOW_GAP_FRAC = 0.05    # of the sources' total, when that is more than FLOW_UNMEASURED_A
# An imbalance ('unmeasured', 'gap') must hold this long before the footnote names it: two CSV4 frames at the tile's 2 s,
# longer than the skew between inputs (a CSV4 frame against GX readings polled every 1 s, each up to 1 s old)
FLOW_IMBALANCE_S = 4.0
FLOW_HOLD_EVERY_S = 30  # /gxrate?flow=1 holds CSV4 at 2 s for 60 s; renewed this often while the tile is shown
DVCC_HEARD_MS = 20000   # a Victron carrier counts as heard within this (the app's Victron card rule)
# Row kinds in display order; the tile's table gives each its words and colour. 'alt' is this regulator, 'altx' any other
# alternator (another regulator, an Orion XS in alternator mode, a Victron alternator), 'source' a generic DC meter;
# 'dcdcload' a DC-DC charger that draws from the house bank, 'dcload' a DC meter on a load. 'dcsystem' (a whole-DC-system
# meter) is no row: it gives House.
FLOW_SOURCE_KINDS = ('alt', 'solar', 'charger', 'multi', 'dcgenset', 'fuelcell', 'altx', 'dcdc', 'wind', 'water',
                     'shaft', 'source')
FLOW_LOAD_KINDS = ('inverter', 'dcdcload', 'dcload')
# D-Bus class -> kind. A Multi is a source row (charging) and a load row (inverting); an inverter and a DC load only a
# load; an Orion DC-DC charger a source, or a load when the owner says it draws from the house bank (memory "h"); a DC
# meter in DC-DC mode a source or a load by its sign.
GX_CLASS_KIND = {'battery': 'battery', 'solarcharger': 'solar', 'charger': 'charger', 'vebus': 'multi',
                 'multi': 'multi', 'inverter': 'inverter', 'dcgenset': 'dcgenset', 'fuelcell': 'fuelcell',
                 'alternator': 'altx', 'dcdc': 'dcdc', 'dcload': 'dcload', 'dcsystem': 'dcsystem'}
# com.victronenergy.dcsource /Settings/MonitorMode -> kind (gui-v2 enums.h MonitorMode_DcSource_*); -1 and any mode not
# listed is a generic source
DCSOURCE_KIND = {-2: 'charger', -3: 'dcdc', -4: 'water', -7: 'shaft', -8: 'wind'}
# com.victronenergy.dcload /Settings/MonitorMode: 7 inverter and 6 DC system make their own kinds; the rest name the row
# when the owner gave no name (gui-v2 1.3.22 enums.h MonitorMode_DcLoad_* and enums.cpp dcMeter_typeToText; 2 and 6
# from the Venus OS wiki's D-Bus page, "dcload"). 1, a generic load, and any mode not listed: the product names it.
DCLOAD_KIND = {6: 'dcsystem', 7: 'inverter'}
DCLOAD_NAME = {2: 'Electric drive', 3: 'Fridge', 4: 'Water pump', 5: 'Bilge pump', 8: 'Water heater'}
# What vecan-dbus names every NMEA 2000 alternator (ProductId 0xB091), this regulator's twin included
ALT_N2K_PRODUCT = 'Alternator'
# An alternator whose /Serial is still empty after this long asking is listed without one. A pinned regulator's twin
# never is: vecan-dbus knows its NMEA 2000 unique number (n2k_unique) from address claim, before any serial.
ALT_SERIAL_WAIT_S = 30.0
# GX-side readings fill what the regulator does not measure (flow_model): battery from systemcalc /Dc/Battery/Current,
# solar from /Dc/Pv/Current less the MPPT the regulator already reads over VE.Direct (flowSolarSer), and a row per GX
# source and load. Read while the tile shows, and for /gxDevices.
GX_FRESH_S = 5.0        # a GX reading older than this is not used
GX_BATT_FROM = {1: 'Shunt', 2: 'NMEA 2000', 3: 'VE.Direct'}   # flowSrc battery source -> label
# Devices seen (Manager store 'gx_devices'): a device that drops off D-Bus when unpowered stays listed at 0.0 A until the
# owner forgets it (/GxForget). Key: D-Bus class and VRM instance. Value: {"k": kind, "n": name, "p": product, "r": roles,
# 's' source row, 'l' load row, 'p' solar; "s": serial, there for an alternator unless it had none after
# ALT_SERIAL_WAIT_S ("w": 1, with "u" its NMEA 2000 unique number when it has one); "h": 1 an Orion (class dcdc) that
# draws from the house bank to charge another battery, which the GX cannot tell from one that feeds it, so the owner says
# (/GxDrawsHouse); "m": 1 / 0 its current does or does not pass through the DC system meter, the owner's word over
# through_meter()'s default (/GxMetered)}. Key "xreg:<uid>", {"k": "reg", "m"}: the same for a pinned regulator's own
# alternator.
GX_POLL_TILE_S = 1      # GxReader cadence while a tile shows
GX_POLL_POST_S = 2      # while only /gxDevices posts need it (inside GX_FRESH_S)
# /gxDevices (GX link 4): the GX's device readings to the regulator, for its web app's Live Data > Integrations. The
# regulator answers the interval it wants (everyS: short while a phone watches, long otherwise).
DEVPOST_EVERY_S = 5.0   # before the first answer
DEVPOST_MAX = 16        # the firmware keeps this many

# Cloud page (/Cloud/*): the get-leaderboard-bundle edge function, called by the bridge itself (xreg_cloud.py)
CLOUD_OFF, CLOUD_UNREGISTERED, CLOUD_LOADING, CLOUD_READY, CLOUD_OFFLINE, CLOUD_ERROR, CLOUD_NO_LINK = range(7)
CLOUD_REFRESH_S = 600   # a shown page refetches this often
CLOUD_RETRY_S = 60      # after a failed fetch
CLOUD_TIMEOUT_S = 15.0
CLOUD_TEXT = {
    CLOUD_OFF: 'Cloud Features is switched off, so leaderboards and fleet stats are unavailable. Turn it on in the '
               'XREG-010 app under Setup → System → Cloud Features.',
    CLOUD_UNREGISTERED: 'This regulator is not registered for cloud features. Register it in the XREG-010 app under '
                        'Cloud Features.',
    CLOUD_LOADING: 'Loading leaderboards',
    CLOUD_OFFLINE: 'No internet connection on the GX',
    CLOUD_NO_LINK: 'Regulator not connected',
}

MGR_SERVICE = 'com.victronenergy.xregulator'   # no ProductName, so gui-v2 never lists it as a device
UNIT_SERVICE = 'com.victronenergy.switch.'
ALT_SERVICE = 'com.victronenergy.alternator.'  # systemcalc's DVCC controls any of these that has /Link/ChargeCurrent
ALT_PRODUCT_ID = 0xB091                       # what NMEA 2000 alternators carry; the plugin's Dashboard attaches to it
ALT_LINK_PATHS = ('/Link/ChargeVoltage', '/Link/ChargeCurrent', '/Link/NetworkMode')
DVCC_FRESH_S = 10.0  # a systemcalc write counts this long: three of its 3 s ADJUST passes, plus a second
GXLIMITS_EVERY_S = 3.0
# /Link/GxLimits/State
LIMITS_NONE, LIMITS_IDLE, LIMITS_SENDING, LIMITS_NATIVE, LIMITS_REFUSED = 0, 1, 2, 3, 4
INSTANCE = 100       # requested VRM instance of a new regulator; localsettings moves it to the next free one
# Demo only: the simulated bank as a battery service, so the GX's own battery tile reads it. systemcalc then uses it as
# the system battery, and from there it can reach real equipment: SOC onto every VE.Can port and to non-VE.Bus
# inverter-chargers (socsync.py, always on, whatever is on the bus), and with DVCC on, voltage, current and temperature
# to solar chargers, inverters, alternators and VE.Can (batterysense.py); a Multi takes it for ESS and SOC; Modbus TCP
# serves it to whatever polls. So it is published only on a GX with no VE.Can or BMS-Can port (Mark 2026-10-28: "Block
# if VE.Can present"), no other battery, none of these device classes, DVCC off and Modbus TCP off. It carries no
# /Info/* limits, so it is never a BMS. A bench GX with its VE.Can port keeps the battery tile's dashes.
BATT_SERVICE = 'com.victronenergy.battery.'
BATT_BLOCKERS = ('vecan', 'battery', 'vebus', 'multi', 'acsystem', 'inverter', 'solarcharger', 'charger', 'dcdc')
MDNS_TYPE = '_xreg._tcp.local.'
UID_RE = re.compile(r'[0-9A-F]{16}$')
HOST_RE = re.compile(r'[A-Za-z0-9.-]{1,64}(:[0-9]{1,5})?$')
LOCATE_RETRY_S = 10  # a missing regulator is looked for again this often, or at once when Discovery sees it move
SWEEP_EVERY_S = 60   # spacing of the /identify sweeps a missing regulator asks for
SWEEP_WORKERS = 32
SWEEP_TIMEOUT_S = 1.5
FOUND_KEEP_S = 600   # an entry not from mDNS leaves the found list after this long unseen


def uid6(uid):
    # The six hex the app, hostname and AP SSID use: device_id_hex is the MAC reversed, so its tail is the shared OUI
    return (uid or '')[-12:-6].upper()


def norm_uid(v):
    u = str(v or '').strip().upper()
    return u if UID_RE.match(u) else ''


def unit_ident(uid):
    # Names this regulator's D-Bus service and its /Settings/Devices/<ident> group
    return 'xregulator_' + uid6(uid).lower()


def unit_service(uid):
    return UNIT_SERVICE + unit_ident(uid)


def batt_service(uid):
    return BATT_SERVICE + unit_ident(uid)


def alt_service(uid):
    return ALT_SERVICE + unit_ident(uid)


def serial_matches(uid, serial, uids):
    # A device's serial names this regulator: all 16 hex, or the last 8 (older firmware's NMEA 2000 serial) when no
    # other pinned regulator ends the same way
    s = str(serial or '').strip().upper()
    if UID_RE.match(s):
        return s == uid
    if not re.fullmatch(r'[0-9A-F]{8,15}', s):
        return False
    return [u for u in uids if u.endswith(s)] == [uid]


def label(uid, name=''):
    tag = 'XREG-' + uid6(uid)
    return '%s (%s)' % (name, tag) if name and name.upper() != tag else tag


def gx_version(ident):
    try:
        return int(ident.get('gx') or 0)
    except (TypeError, ValueError):
        return 0


def xreg_ident(body):
    # A parsed /identify answer from an XREG-010 with a valid uid, else None
    try:
        j = json.loads(body)
    except ValueError:
        return None
    if not isinstance(j, dict) or str(j.get('device', '')).lower() != 'xreg-010' or not norm_uid(j.get('uid')):
        return None
    return j


def fetch_error_text(e):
    return getattr(e, 'reason', None) and str(e.reason) or str(e) or e.__class__.__name__


def fmt_num(v):
    if v is None:
        return ''
    if abs(v - round(v)) < 1e-9:
        return str(int(round(v)))
    return ('%.6f' % v).rstrip('0').rstrip('.')


def _same_number(a, b):
    try:
        return abs(float(a) - float(b)) < 1e-6
    except (TypeError, ValueError):
        return False


def fmt_series(values, decimals):
    return ','.join('' if v is None else '%.*f' % (decimals, v) for v in values)


def field_list(js, name):
    # The web app's own field list, read from the bundle this regulator serves, so the names always match its firmware
    m = re.search(r'const\s+%s\s*=\s*\[(.*?)\];' % name, js, re.S)
    if not m:
        raise ValueError(name + ' not found in script.js')
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    return re.findall(r'"([^"]+)"', body)


def parse_counted(payload):
    # CSV channel frame: first value = field count. Returns the values, or None when malformed.
    try:
        vals = [float(v) for v in payload.split(',')]
    except ValueError:
        return None
    if not vals or int(vals[0]) != len(vals) - 1:
        return None
    return vals[1:]


def parse_plain(payload):
    try:
        return [float(v) for v in payload.split(',')]
    except ValueError:
        return None


def circ_mean(sin_sum, cos_sum, signed):
    if sin_sum == 0 and cos_sum == 0:
        return 0.0
    a = math.degrees(math.atan2(sin_sum, cos_sum))
    if not signed and a < 0:
        a += 360.0
    return a


def parse_backfill(b):
    # GET /demoBackfill (demo only): {"v":1, "short": {"dt":1, "cols":[...], "rows":[[...]]}, "wind": {"dt":10, ...}},
    # rows oldest first, newest at "now", plain engineering units. -> {'short': [row dict], 'wind': [row dict]}
    j = json.loads(gunzip_maybe(b))
    if not isinstance(j, dict) or j.get('v') != 1:
        raise ValueError('not a v1 demo backfill')
    out = {}
    for name in ('short', 'wind'):
        blk = j.get(name) if isinstance(j.get(name), dict) else {}
        cols, rows = blk.get('cols'), blk.get('rows')
        if not isinstance(cols, list) or not isinstance(rows, list):
            out[name] = []
            continue
        num = lambda v: float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) else None
        out[name] = [dict(zip(cols, map(num, r))) for r in rows if isinstance(r, list) and len(r) == len(cols)]
    return out


def parse_baro(b):
    # /baroHistory.bin: u16 head, u32 epoch of the newest sample (0 = regulator clock not set), u16 count, then count
    # u16 samples (mbar x10, 0 = empty) in a ring whose oldest slot is head. -> (last BARO_KEEP, oldest first; epoch)
    head, epoch, count = struct.unpack_from('<HIH', b, 0)
    ring = [int.from_bytes(b[8 + 2 * i:10 + 2 * i], 'little') for i in range(count)]
    ordered = ring[head:] + ring[:head]
    return [v / 10.0 if v else None for v in ordered[-BARO_KEEP:]], epoch


# ---- Long-term plots: /longTermPlots.bin decoded as script.js parseLT() does ----

# (name, wire scale, published decimals), in record order; envelope fields carry [min, max, avg]
LT_ENV = (('battVolt', 100, 2), ('battCurr', 10, 1), ('altCurr', 10, 1), ('victronCurr', 10, 1), ('rpm', 1, 0),
          ('duty', 100, 1), ('altTemp', 10, 1), ('tempTherm', 10, 1), ('sog', 100, 2), ('tws', 100, 1),
          ('vmg', 100, 2), ('aws', 100, 1), ('awa', 10, 0), ('twa', 10, 0), ('heel', 100, 1), ('pitch', 100, 1),
          ('soc', 10, 1))
LT_AVG = (('baro', 10, 1), ('ambTemp', 10, 1), ('cog', 10, 0), ('heading', 10, 0), ('leeway', 10, 1),
          ('altZero', 100, 2))
LT_ONAVG = (('battVolt', 100), ('battCurr', 10), ('altCurr', 10), ('victronCurr', 10), ('duty', 100))
LT_ONAVG_BIT = tuple([n for n, _, _ in LT_ENV].index(k) for k, _ in LT_ONAVG)
# v5 tail envelopes: (name, wire scale, published decimals, validMask bit, index of its min in the unpacked record)
LT_TAIL_ENV = (('battTemp', 10, 1, 24, 70),)
LT_TEMPS = ('altTemp', 'tempTherm', 'ambTemp', 'battTemp')   # stored in F, published in C
LT_ANGLES = ('awa', 'twa', 'cog', 'heading')
LT_NONZERO = ('battVolt', 'baro')                # a 0 here is a boot transient, never a reading
LT_FIELDS = tuple(n for n, _, _ in LT_ENV) + tuple(n for n, _, _ in LT_AVG) + tuple(n for n, *_ in LT_TAIL_ENV)
LT_REC5 = struct.Struct('<IiiI51h6hBB5hBB3hh')   # 152 B (v5: + battery temperature envelope)
LT_REC4 = struct.Struct('<IiiI51h6hBB5hBB')      # 144 B (v4: engine-on averages + coverage)
LT_REC3 = struct.Struct('<IiiI51h6hBB')          # 132 B
LT_AF = 4 + 3 * len(LT_ENV) + len(LT_AVG) + 2 + len(LT_ONAVG)   # activeFrac's index in a v4/v5 record


def parse_longterm(buf, wall=time.time):
    if len(buf) < 16:
        return None
    _head, count, _cap, rec_size, last_epoch, interval = struct.unpack_from('<HHHHII', buf, 0)
    interval = interval or 600
    if not rec_size or len(buf) < 16 + count * rec_size:
        return None
    st = LT_REC5 if rec_size >= LT_REC5.size else LT_REC4 if rec_size >= LT_REC4.size \
        else LT_REC3 if rec_size >= LT_REC3.size else None
    if st is None:
        return None
    # Unsynced records carry epoch 0: anchor the newest to the header epoch (or now) and space back by the interval
    anchor = last_epoch or int(wall())
    recs = {}
    for i in range(count):
        v = st.unpack_from(buf, 16 + i * rec_size)
        ts = v[0] or anchor - (count - 1 - i) * interval
        if ts not in recs:
            recs[ts] = v
    return {'interval': interval, 'last_epoch': last_epoch, 'v4': st is not LT_REC3, 'v5': st is LT_REC5,
            'recs': sorted(recs.items())}


def _lt_value(name, raw, scale):
    v = raw / scale
    if name in LT_NONZERO and v <= 0:
        return None
    if name in LT_TEMPS:
        v = F_TO_C(v)
    return v


def bin_longterm(lt, range_s, max_points=LONG_POINTS):
    # Uniform grid ending on the newest record: sample i sits at start + i*step and holds the records within half a
    # step of it (min of mins, max of maxes, mean of averages; angles as vector means). None = no record there.
    recs = lt['recs'] if lt else []
    out = {'start': None, 'step': None, 'n': 0, 'series': {}}
    if not recs:
        return out
    interval = lt['interval']
    n = max(1, min(max_points, int(math.ceil(range_s / float(interval)))))
    step = int(math.ceil(range_s / float(n)))
    start = recs[-1][0] - (n - 1) * step
    buckets = [[] for _ in range(n)]
    for ts, v in reversed(recs):
        i = int(math.floor((ts - start) / float(step) + 0.5))
        if i < 0:
            break
        if i < n:
            buckets[i].append(v)
    series = {}
    for name, *_ in LT_ENV + LT_TAIL_ENV:
        series[name], series[name + 'Min'], series[name + 'Max'] = [None] * n, [None] * n, [None] * n
    for name, _, _ in LT_AVG:
        series[name] = [None] * n
    for name, _ in LT_ONAVG:
        series[name + 'OnAvg'] = [None] * n
    series['chargeStage'], series['cover'] = [None] * n, [None] * n
    n_env = len(LT_ENV)
    avg_base = 4 + 3 * n_env
    envs = [(name, scale, fi, 4 + 3 * fi) for fi, (name, scale, _) in enumerate(LT_ENV)]
    if lt and lt.get('v5'):
        envs += [(name, scale, bit, o) for name, scale, _, bit, o in LT_TAIL_ENV]
    for bi, group in enumerate(buckets):
        if not group:
            continue
        for name, scale, bit, o in envs:
            mins, maxs, avgs = [], [], []
            for v in group:
                if v[3] & (1 << bit):
                    a = _lt_value(name, v[o + 2], scale)
                    if a is None:
                        continue
                    avgs.append(a)
                    lo, hi = _lt_value(name, v[o], scale), _lt_value(name, v[o + 1], scale)
                    if lo is not None:
                        mins.append(lo)
                    if hi is not None:
                        maxs.append(hi)
            if avgs:
                series[name][bi] = _mean(name, avgs)
                series[name + 'Min'][bi] = min(mins) if mins else None
                series[name + 'Max'][bi] = max(maxs) if maxs else None
        for ai, (name, scale, _) in enumerate(LT_AVG):
            vals = [_lt_value(name, v[avg_base + ai], scale) for v in group if v[3] & (1 << (n_env + ai))]
            vals = [x for x in vals if x is not None]
            if vals:
                series[name][bi] = _mean(name, vals)
        covers, stages = [], collections.Counter()
        on_w = [[0.0, 0.0] for _ in LT_ONAVG]
        for v in group:
            stages[v[avg_base + len(LT_AVG)]] += 1
            if lt['v4']:
                af = v[LT_AF]
                cover = af / 2.0 if v[3] & 1 else None
                for oi, (name, scale) in enumerate(LT_ONAVG):
                    if af > 0 and v[3] & (1 << LT_ONAVG_BIT[oi]):
                        x = _lt_value(name, v[avg_base + len(LT_AVG) + 2 + oi], scale)
                        w = 100.0 if cover is None else cover
                        if x is not None and w > 0:
                            on_w[oi][0] += x * w
                            on_w[oi][1] += w
            else:
                cover = None   # pre-v4 records: the engine-on line falls back to the wall average, as the app does
                for oi, (name, scale) in enumerate(LT_ONAVG):
                    fi = LT_ONAVG_BIT[oi]
                    if v[3] & (1 << fi):
                        x = _lt_value(name, v[4 + 3 * fi + 2], scale)
                        if x is not None:
                            on_w[oi][0] += x * 100.0
                            on_w[oi][1] += 100.0
            if cover is not None:
                covers.append(cover)
        for oi, (name, _) in enumerate(LT_ONAVG):
            if on_w[oi][1] > 0:
                series[name + 'OnAvg'][bi] = on_w[oi][0] / on_w[oi][1]
        series['chargeStage'][bi] = stages.most_common(1)[0][0]
        series['cover'][bi] = sum(covers) / len(covers) if covers else None
    out.update(start=start, step=step, n=n, series=series)
    return out


def longterm_last(lt):
    # Each field's newest recorded average anywhere in the ring: a legend whose window has no reading shows this rather
    # than a dash (demo entry, before the first live record; a field only read now and then)
    out = {}
    if not lt or not lt['recs']:
        return out
    n_env = len(LT_ENV)
    want = [(name, scale, fi, 4 + 3 * fi + 2) for fi, (name, scale, _) in enumerate(LT_ENV)]
    want += [(name, scale, n_env + ai, 4 + 3 * n_env + ai) for ai, (name, scale, _) in enumerate(LT_AVG)]
    if lt.get('v5'):
        want += [(name, scale, bit, o + 2) for name, scale, _, bit, o in LT_TAIL_ENV]
    for name, scale, bit, idx in want:
        for _ts, v in reversed(lt['recs']):
            if v[3] & (1 << bit):
                x = _lt_value(name, v[idx], scale)
                if x is not None:
                    out[name] = round(x, lt_decimals(name))
                    break
    return out


def _mean(name, vals):
    if name in LT_ANGLES:
        rad = [math.radians(x) for x in vals]
        return circ_mean(sum(math.sin(r) for r in rad), sum(math.cos(r) for r in rad), any(x < 0 for x in vals))
    return sum(vals) / len(vals)


def lt_decimals(name):
    for n, _, d, *_ in LT_ENV + LT_AVG + LT_TAIL_ENV:
        if name == n or name in (n + 'Min', n + 'Max', n + 'OnAvg'):
            return d
    return 1 if name == 'cover' else 0


def wind_stale(frames, fresh, marker_ok=False):
    # The CSV4 wind fields the latest fresh TimestampData calls never seen or stale (WIND_TS); with no fresh TS, none.
    # marker_ok: this firmware sends the empty marker for apparent wind with no reading (seen on this stream), so a CSV4
    # frame built after that TS frame speaks for itself: a wind instrument that has just come on shows at once. Without
    # it (older firmware sends 0 for no instrument), the TS verdict stands until the next TS frame.
    if 'TimestampData' not in fresh:
        return set()
    ts, c4 = frames.get('TimestampData') or {}, frames.get('CSVData4') or {}
    if marker_ok and frame_after(c4, ts):
        return set()
    return {f for f, t in WIND_TS.items() if ts.get(t) is not None and ts[t] > WIND_STALE_MS}


def frame_after(c4, ts):
    # The CSV4 frame was built after the TimestampData frame: same boot, sendMs later (millis() wraps at 2^32)
    a, b = c4.get('sendMs'), ts.get('ts_sendMs')
    if a is None or b is None or c4.get('sessionId') != ts.get('ts_sessionId'):
        return False
    d = (int(a) - int(b)) & 0xFFFFFFFF
    return 0 < d < 0x80000000


# ---- Recorders (fed on the main tick) ----

class ShortRecorder:
    # 1 s means of every CSV1 frame that arrived in that second; a second with no frame holds the previous value while
    # the stream is fresh (the GX schedule can send CSV1 only every 5-10 s), else it is a gap.
    def __init__(self):
        self.series = {name: collections.deque(maxlen=SHORT_LEN) for name, *_ in SHORT}
        self.stage = collections.deque(maxlen=SHORT_LEN)

    def close(self, means, fresh, stage):
        for name, _field, _scale, conv, _dec in SHORT:
            q = self.series[name]
            m = means.get(name)
            if m is not None:
                v = conv(m) if conv else m
            elif fresh and q:
                v = q[-1]
            else:
                v = None
            q.append(v)
        self.stage.append(stage if fresh else None)

    def strings(self):
        out = {name: fmt_series(self.series[name], dec) for name, _f, _s, _c, dec in SHORT}
        out['chargeStage'] = fmt_series(self.stage, 0)
        return out

    def fill(self, rows):
        # /demoBackfill "short" rows (parse_backfill), oldest first, one per second, already in engineering units
        self.__init__()
        for r in rows[-SHORT_LEN:]:
            self.close({name: r[field] for name, field, *_ in SHORT if r.get(field) is not None}, True,
                       r.get('chargeStage'))


class WindRecorder:
    def __init__(self):
        self.series = {name: collections.deque(maxlen=WIND_PLOT_LEN) for name, *_ in WIND_PLOT}
        self._reset()

    def _reset(self):
        self.acc = {name: [0.0, 0.0, 0, False] for name, *_ in WIND_PLOT}   # sum (or sin sum), cos sum, count, any < 0

    def sample(self, frame, stale=()):
        for name, field, scale, angle in WIND_PLOT:
            raw = frame.get(field)
            if raw is None or raw <= EMPTY or field in stale:
                continue
            a = self.acc[name]
            v = raw / scale
            if angle:
                r = math.radians(v)
                a[0] += math.sin(r)
                a[1] += math.cos(r)
                a[3] = a[3] or v < 0
            else:
                a[0] += v
            a[2] += 1

    def close(self):
        for name, _field, _scale, angle in WIND_PLOT:
            s, c, n, neg = self.acc[name]
            if not n:
                v = None
            elif angle:
                v = circ_mean(s, c, neg)
            else:
                v = s / n
            self.series[name].append(v)
        self._reset()

    def strings(self):
        return {name: fmt_series(self.series[name], 0 if angle else 1) for name, _f, _s, angle in WIND_PLOT}

    def fill(self, rows):
        # /demoBackfill "wind" rows, oldest first, one per 10 s close, already in kt and degrees
        self.__init__()
        for r in rows[-WIND_PLOT_LEN:]:
            for name, field, _scale, _angle in WIND_PLOT:
                self.series[name].append(r.get(field))


# ---- Settings model (gx_app/xregulator/bridge/settings_schema.json, built by gx_app/gen_settings.py) ----

ECHO_EVENT = {'CSV1': 'CSVData', 'CSV2': 'CSVData2', 'CSV3': 'CSVData3', 'CSV4': 'CSVData4', 'TS': 'TimestampData'}
PATH_ELEMENT = re.compile(r'[A-Za-z0-9_]+$')   # what a D-Bus object path element may hold; "@probes" may not


class SettingsModel:
    def __init__(self, schema):
        self.items = {}    # key -> item (stand-alone items and table cells); writes go through these
        self.values = []   # keys published under /Settings/Value
        self.actions = []  # keys published under /Settings/Action
        self.tables = {}   # tableId -> {'item': table item, 'cells': [[key per column] per row]}
        self.endpoints = {}   # name -> (path, poll seconds): JSON the app polls; echo stream "HTTP" reads from these
        self.probe_roles = []   # the probe list's role-assignment keys, by role index (the app's OW_ASSIGN_PARAM)
        self.probe_arm = True
        for name, ep in ((schema or {}).get('endpoints') or {}).items():
            if PATH_ELEMENT.match(name) and isinstance(ep, dict) and isinstance(ep.get('path'), str) \
                    and ep['path'].startswith('/'):
                poll = ep.get('pollS')
                self.endpoints[name] = (ep['path'], float(poll) if isinstance(poll, (int, float)) and poll > 0
                                        else SOURCE_POLL_S)
        self._walk((schema or {}).get('outline') or [])

    def _walk(self, nodes):
        # outline pages hold items and deeper pages ("children" marks a page); where an item sits does not matter here
        for n in nodes:
            if not isinstance(n, dict):
                continue
            if isinstance(n.get('children'), list):
                self._walk(n['children'])
            else:
                self._add(n)

    @classmethod
    def from_text(cls, text):
        if not text:
            return cls({})
        try:
            return cls(json.loads(text))
        except (ValueError, AttributeError, TypeError) as e:
            log.warning('settings schema unreadable: %s', e)
            return cls({})

    def _add(self, item):
        t = item.get('type')
        if t == 'table':
            tid = item.get('tableId') or item.get('key')
            if not tid or not PATH_ELEMENT.match(tid):
                return
            rows = int(item.get('rows') or 0)
            cols = [col for col in item.get('columns', []) if col.get('keyPattern')]
            cells = []
            for i in range(rows):
                row = []
                for col in cols:
                    pat = col['keyPattern']
                    cell = dict(col, key=pat.replace('{i}', str(i)), type=col.get('type') or 'number')
                    if 'arm' not in col:
                        cell['arm'] = item.get('arm', True)
                    echo = col.get('echo')
                    if isinstance(echo, dict):
                        cell['echo'] = dict(echo, field=(echo.get('field') or pat).replace('{i}', str(i)))
                    row.append(cell['key'])
                    self._add_value(cell)
                cells.append(row)
            self.tables[tid] = {'item': item, 'cells': cells, 'cols': cols}
            return
        key = item.get('key')
        if not key:
            return
        if t == 'info':
            # A widget's own writes (the probe list's role assignment) are named in its *_PARAM string lists
            for sname, keys in (item.get('strings') or {}).items():
                if sname.endswith('_PARAM') and isinstance(keys, list):
                    if sname == 'OW_ASSIGN_PARAM' and all(isinstance(k, str) and PATH_ELEMENT.match(k) for k in keys):
                        self.probe_roles = list(keys)
                        self.probe_arm = item.get('arm', True)
        if t == 'action':
            if PATH_ELEMENT.match(key):
                if key not in self.items:
                    self.actions.append(key)
                    if isinstance(item.get('echo'), dict):
                        self.values.append(key)   # read-only state beside the button (Alarm Test: Active)
                self.items[key] = item
            return
        if t == 'info' and not PATH_ELEMENT.match(key):
            return   # a widget with nothing to echo (its data is /Settings/Http/<source>)
        self._add_value(item)

    def _add_value(self, item):
        if not PATH_ELEMENT.match(item['key']):
            log.warning('setting %r cannot be a D-Bus path; skipped', item['key'])
            return
        if item['key'] not in self.items:
            self.values.append(item['key'])
        self.items[item['key']] = item

    @staticmethod
    def writeable(item):
        return item.get('type') not in ('info', 'action', 'table')

    @staticmethod
    def _numeric_options(item):
        opts = item.get('options') or []
        return [o.get('value') for o in opts if isinstance(o, dict) and isinstance(o.get('value'), (int, float))
                and not isinstance(o.get('value'), bool)]

    @staticmethod
    def _decimals(item):
        if isinstance(item.get('decimals'), int):
            return item['decimals']
        step = item.get('step')
        if isinstance(step, (int, float)) and step > 0:
            s = fmt_num(float(step))
            return len(s.split('.')[1]) if '.' in s else 0
        return None

    def display(self, item, frames):
        echo = item.get('echo')
        if not isinstance(echo, dict) or not echo.get('field'):
            return None
        stream = echo.get('stream')
        if stream == 'HTTP':
            raw = frames.get('HTTP:' + str(echo.get('endpoint')))
            for part in echo['field'].split('.'):
                raw = raw.get(part) if isinstance(raw, dict) else None
            if isinstance(raw, str) and item.get('type') == 'text':
                return raw
            if isinstance(raw, bool):
                raw = int(raw)
            if not isinstance(raw, (int, float)):
                return None
        else:
            frame = frames.get(ECHO_EVENT.get(stream, stream))
            raw = frame.get(echo['field']) if frame else None
        if raw is None or raw <= EMPTY:
            return None
        v = raw / float(echo.get('scale') or 1)
        t = item.get('type')
        if t == 'toggle':
            return int(round(v))
        if t == 'select':
            opts = self._numeric_options(item)
            if opts and all(float(o).is_integer() for o in opts):
                return int(round(v))
        d = self._decimals(item)
        v = round(v, d if d is not None else 6)
        return int(v) if d == 0 else v

    def check(self, item, value):
        # The display value a write would set, or raises ValueError with the reason (what the app's input would refuse)
        t = item.get('type')
        if t == 'text':
            s = '' if value is None else str(value)
            if len(s) > 64 or any(ord(c) < 32 for c in s):
                raise ValueError('Text not accepted')
            return s
        if isinstance(value, str) and t == 'select':
            labels = [o.get('value') for o in item.get('options') or [] if isinstance(o, dict)]
            if value in labels:
                return value
        try:
            v = float(value)
        except (TypeError, ValueError):
            raise ValueError('Not a number')
        if not math.isfinite(v):
            raise ValueError('Not a number')
        if t == 'toggle':
            if v not in (0.0, 1.0):
                raise ValueError('Must be 0 or 1')
            return int(v)
        if t == 'select':
            opts = self._numeric_options(item)
            if opts and not any(abs(v - o) < 1e-9 for o in opts):
                raise ValueError('Not one of the choices')
            return int(v) if v.is_integer() else v
        lo, hi = item.get('min'), item.get('max')
        if isinstance(lo, (int, float)) and v < lo - 1e-9 or isinstance(hi, (int, float)) and v > hi + 1e-9:
            raise ValueError('Out of range (%s to %s)' % ('' if lo is None else fmt_num(lo),
                                                          '' if hi is None else fmt_num(hi)))
        return v

    @staticmethod
    def wire(item, value):
        if isinstance(value, str):
            return value
        send = item.get('send') or {}
        w = float(value) * float(send.get('scale') or 1)
        if send.get('integer'):
            return str(int(round(w)))
        return fmt_num(w)

    @staticmethod
    def _rpm_ascending(rpm):
        # The app's validateLearningTable(): strictly rising up to the last non-zero row, zeros only at the end
        last = max([i for i, v in enumerate(rpm) if v != 0] or [-1])
        for i in range(1, last + 1):
            if rpm[i] <= rpm[i - 1]:
                raise ValueError('RPM values must increase. Row %d (%s) must be greater than Row %d (%s)'
                                 % (i + 1, fmt_num(rpm[i]), i, fmt_num(rpm[i - 1])))
        seen = False
        for i in range(len(rpm) - 1, -1, -1):
            if rpm[i] != 0:
                seen = True
            elif seen:
                raise ValueError('Zero RPM values are only allowed at the end of the table. Found zero at row %d'
                                 % (i + 1))

    def table_json(self, tid, frames):
        t = self.tables[tid]
        return json.dumps({k: self.display(self.items[k], frames) for row in t['cells'] for k in row},
                          separators=(',', ':'))

    def _gated(self, col, frames):
        # True while the app disables this column's inputs (its form then leaves them out), None while unknown
        rw = col.get('readOnlyWhen')
        if not isinstance(rw, dict) or not rw.get('omit') or rw.get('key') not in self.items:
            return False
        v = self.display(self.items[rw['key']], frames)
        return None if v is None else v == rw.get('value')

    @staticmethod
    def _live_batt_v(frames):
        # The app's getLiveBatteryV(): IBV, else BatteryV, else 12 V
        c1 = frames.get('CSVData') or {}
        for f in ('IBV', 'BatteryV'):
            raw = c1.get(f)
            if raw is not None and raw > EMPTY and raw != 0:
                return raw / 100.0
        return 12.0

    def table_params(self, tid, edits, frames):
        # The whole table as the app's Save Table form sends it: extra keys (like capLimitMode) first, then every cell
        # row by row, an unedited cell at its echoed value. A column the app disables (readOnlyWhen omit) is left out,
        # and the cap column the mode does not use is derived from the other at the live battery voltage, as the app's
        # setCapMode() fills it. Raises ValueError naming the first bad cell.
        t = self.tables[tid]
        cell_keys = [k for row in t['cells'] for k in row]
        include = [k for k in t['item'].get('include') or [] if isinstance(k, str)]
        params = []
        for k in include + [k for k in edits if k not in cell_keys and k not in include]:
            item = self.items.get(k)
            if item is None:
                raise ValueError('%s is not a setting' % k)
            v = edits[k] if k in edits else self.display(item, frames)
            if v is not None:
                params.append((k, self.wire(item, self.check(item, v))))
        cols = t.get('cols') or []
        gates = [self._gated(c, frames) for c in cols]
        cap = t['item'].get('capMode')
        kw_mode = None
        if isinstance(cap, dict) and cap.get('key') in self.items:
            m = edits[cap['key']] if cap['key'] in edits else self.display(self.items[cap['key']], frames)
            if m is not None:
                kw_mode = float(m) == float(cap.get('kw', 1))
        derived_cap = None if kw_mode is None else ('amps' if kw_mode else 'kw')
        batt_v = self._live_batt_v(frames)
        rule = t['item'].get('validate')
        if isinstance(rule, dict) and rule.get('rule') == 'ascending' and rule.get('keyPattern'):
            keys = [rule['keyPattern'].replace('{i}', str(i)) for i in range(len(t['cells']))]
        elif rule == 'rpmAscending':
            keys = [row[0] for row in t['cells'] if row]
        else:
            keys = []
        if keys and all(k in self.items for k in keys):
            col = [edits[k] if k in edits else self.display(self.items[k], frames) for k in keys]
            self._rpm_ascending([float(v or 0) for v in col])
        for row in t['cells']:
            for ci, k in enumerate(row):
                item = self.items[k]
                col = cols[ci] if ci < len(cols) else {}
                gate = gates[ci] if ci < len(gates) else False
                if gate:
                    continue
                if gate is None:
                    # state unknown: send only a real edit, never a cell a full-table page copied from the echo
                    cur = self.display(item, frames)
                    if k not in edits or (cur is not None and _same_number(edits[k], cur)):
                        continue
                if k in edits:
                    v = edits[k]
                elif derived_cap and col.get('capCol') == derived_cap:
                    src = next((row[j] for j, c in enumerate(cols[:len(row)])
                                if c.get('capCol') and c.get('capCol') != derived_cap), None)
                    a = None if src is None else (edits[src] if src in edits else self.display(self.items[src], frames))
                    if a is None:
                        continue
                    a = float(a)
                    if derived_cap == 'kw':
                        v = round(batt_v * a / 1000.0, 2) if a else 0
                    else:
                        v = int(math.floor(a * 1000.0 / batt_v + 0.5)) if a and batt_v >= 0.5 else 0
                else:
                    v = self.display(item, frames)
                if v is None:
                    continue
                try:
                    params.append((k, self.wire(item, self.check(item, v))))
                except ValueError as e:
                    raise ValueError('%s: %s' % (item.get('label') or k, e))
        if not params:
            raise ValueError('Nothing to save')
        return params


# ---- HTTP to the regulator ----

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


_OPENER = urllib.request.build_opener(_NoRedirect, urllib.request.ProxyHandler({}))   # LAN only


class Http:
    def __init__(self, host):
        self.host = host
        self.base = 'http://' + host

    def request(self, path, query=None, body=None, headers=None, timeout=5.0):
        # (status, headers, body bytes) for any HTTP answer; raises only when there is no answer
        url = self.base + path
        if query:
            url += ('&' if '?' in path else '?') + urllib.parse.urlencode(query)
        data = urllib.parse.urlencode(body).encode() if isinstance(body, (dict, list)) else body
        req = urllib.request.Request(url, data=data, headers=headers or {})
        try:
            with _OPENER.open(req, timeout=timeout) as r:
                return r.status, r.headers, r.read()
        except urllib.error.HTTPError as e:
            try:
                b = e.read()
            except Exception:
                b = b''
            return e.code, e.headers, b

    def get_ok(self, path, timeout=10.0):
        st, _, b = self.request(path, timeout=timeout)
        if st != 200:
            raise IOError('%s: HTTP %d' % (path, st))
        return b


def gunzip_maybe(b):
    return gzip.decompress(b) if b[:2] == b'\x1f\x8b' else b


# ---- Discovery: every regulator on the GX's networks, by uid ----

def own_subnet_hosts():
    # Every other address in the GX's own IPv4 networks, at most a /24 each (the app sweeps its own /24 the same way)
    try:
        out = subprocess.run(['ip', '-4', '-o', 'addr', 'show'], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    hosts = []
    for m in re.finditer(r'^\d+:\s+\S+\s+inet\s+([\d.]+)/(\d+)', out, re.M):
        ip = ipaddress.IPv4Address(m.group(1))
        if ip.is_loopback or ip.is_link_local:
            continue
        net = ipaddress.IPv4Network('%s/%d' % (ip, max(int(m.group(2)), 24)), strict=False)
        hosts += [str(h) for h in net.hosts() if h != ip]
    return list(dict.fromkeys(hosts))


class Discovery:
    # The _xreg._tcp mDNS browse (python-zeroconf, as Venus's own dbus-shelly does beside avahi), every /identify
    # answer the bridge sees, and an /identify sweep of the GX's own subnets when a pinned regulator is missing or the
    # owner asks. One address answers for one regulator, so a new answer there drops the old one's claim.
    def __init__(self, clock=time.monotonic, sweep_hosts=own_subnet_hosts, browse=True):
        self.clock = clock
        self.sweep_hosts = sweep_hosts
        self.browse = browse
        self.lock = threading.Lock()
        self.found = {}       # uid -> {'uid', 'name', 'host', 'fw', 'pair' (uid6 list), 'via', 'at'}
        self.mdns_names = {}  # mDNS instance name -> uid, for its Removed event
        self.watch = {}       # uid -> Event set when that uid shows up at a new address
        self.sweep_req = threading.Event()
        self.sweeping = False
        self.last_sweep = -1e18
        self.zc = None

    def start(self):
        threading.Thread(target=self._sweeper, name='sweep', daemon=True).start()
        if self.browse:
            try:
                import zeroconf
                self.zc = zeroconf.Zeroconf()
                zeroconf.ServiceBrowser(self.zc, MDNS_TYPE, handlers=[self._mdns_event])
            except Exception as e:
                log.warning('mDNS browse unavailable (%s): finding regulators by /identify sweep only', e)

    def stop(self):
        if self.zc is not None:
            self.zc.close()

    def note(self, uid, host, name='', fw='', pair=None, via=''):
        uid = norm_uid(uid)
        if not uid or not host:
            return
        with self.lock:
            for u in [u for u, e in self.found.items() if u != uid and e['host'] == host]:
                del self.found[u]
            e = self.found.get(uid) or {'uid': uid, 'name': '', 'host': '', 'fw': '', 'pair': [], 'via': ''}
            moved = e['host'] != host
            e.update(host=host, name=name or e['name'], fw=fw or e['fw'], via=via, at=self.clock())
            if pair is not None:
                e['pair'] = pair
            self.found[uid] = e
            wake = self.watch.get(uid) if moved else None
        if wake is not None:
            wake.set()

    def note_ident(self, host, ident, via):
        paired = ident.get('paired')
        pair = [uid6(p.get('uid')) for p in paired if isinstance(p, dict) and norm_uid(p.get('uid'))] \
            if isinstance(paired, list) else None
        self.note(ident.get('uid'), host, str(ident.get('name') or ''), str(ident.get('fw') or ''), pair, via)

    def get(self, uid):
        with self.lock:
            e = self.found.get(uid)
            return dict(e) if e else None

    def hosts_for(self, uid):
        e = self.get(uid)
        return [e['host']] if e and e['host'] else []

    def snapshot(self):
        now = self.clock()
        with self.lock:
            for u in [u for u, e in self.found.items() if e['via'] != 'mdns' and now - e['at'] > FOUND_KEEP_S]:
                del self.found[u]
            return [dict(e) for e in self.found.values()]

    def request_sweep(self, force=False):
        if force or self.clock() - self.last_sweep >= SWEEP_EVERY_S:
            self.last_sweep = self.clock()
            self.sweep_req.set()

    def _sweeper(self):
        while True:
            self.sweep_req.wait()
            self.sweep_req.clear()
            self.sweeping = True
            try:
                hosts = self.sweep_hosts()
                with concurrent.futures.ThreadPoolExecutor(SWEEP_WORKERS) as ex:
                    for host, ident in zip(hosts, ex.map(self._identify, hosts)):
                        if ident:
                            self.note_ident(host, ident, 'sweep')
            except Exception:
                log.exception('sweep')
            finally:
                self.sweeping = False
                self.last_sweep = self.clock()

    @staticmethod
    def _identify(host):
        try:
            st, _, body = Http(host).request('/identify', timeout=SWEEP_TIMEOUT_S)
        except Exception:
            return None
        return xreg_ident(body) if st == 200 else None

    def _mdns_event(self, zeroconf, service_type, name, state_change):
        # zeroconf's browser thread; resolving here is its documented pattern
        if state_change.name == 'Removed':
            with self.lock:
                uid = self.mdns_names.pop(name, None)
                if uid and self.found.get(uid, {}).get('via') == 'mdns':
                    del self.found[uid]
            return
        info = zeroconf.get_service_info(service_type, name, timeout=3000)
        if info is None:
            return
        txt = {(k or b'').decode('utf-8', 'replace'): (v or b'').decode('utf-8', 'replace')
               for k, v in (info.properties or {}).items()}
        uid = norm_uid(txt.get('uid'))
        addrs = [a for a in info.parsed_addresses() if ':' not in a]
        if not uid or not addrs:
            return
        host = addrs[0] if info.port in (None, 80) else '%s:%d' % (addrs[0], info.port)
        with self.lock:
            self.mdns_names[name] = uid
        self.note(uid, host, txt.get('name', ''), txt.get('fw', ''),
                  [p.upper() for p in txt.get('pair', '').split(',') if re.fullmatch(r'[0-9A-Fa-f]{6}', p)], 'mdns')


# ---- The link: find the pinned regulator, field lists, schemas, and the event stream ----

class NotFound(Exception):
    def __init__(self, other, reason):
        super().__init__(reason)
        self.other, self.reason = other, reason


class Link:
    # One pinned regulator. Streams only from the address where /identify answers with the pinned uid: the cached
    # address first, then wherever Discovery last saw that uid.
    def __init__(self, uid, host, discovery, cache_dir=CACHE_DIR, clock=time.monotonic):
        self.uid = uid
        self.host = host
        self.http = Http(host) if host else None
        self.discovery = discovery
        self.cache_dir = cache_dir
        self.clock = clock
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.wake = threading.Event()
        self.state = 'connecting'    # connecting, missing, unreachable, streaming, lost
        self.reason = ''
        self.other = None            # (address, /identify) of a different regulator answering at the cached address
        self.ever_live = False
        self.ident = {}
        self.names = {}
        self.schema = {'AltLive': [], 'AltSettings': [], 'PerfLive': [], 'MotorLive': [], 'PerfSettings': []}
        self.perf_schema_text = ''
        self.gx = False              # /identify "gx" >= 2: /gxevents, /gxrate and gx=1 writes
        self.channel = ''
        self.generation = 0          # counts stream connects
        self.stream_at = -1e18       # when the current stream connected
        self.rate = None
        self.mismatch = None
        self.frames = {}
        self.frame_at = {}
        self.short_acc = {}
        self.sock = None
        self.uid_mismatch = False
        discovery.watch[uid] = self.wake

    def start(self):
        threading.Thread(target=self._run, name='link', daemon=True).start()

    def close(self):
        self.stop.set()
        self.wake.set()
        self.discovery.watch.pop(self.uid, None)
        with self.lock:
            sock = self.sock
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def hint(self, host):
        # The owner typed where this regulator is: try there next
        with self.lock:
            if self.state != 'streaming':
                self.host = host
        self.wake.set()

    def relocate(self):
        # A request carrying the pinned uid reached another unit (409): drop the stream and find the uid again now
        self.uid_mismatch = True
        with self.lock:
            self.frames, self.frame_at = {}, {}
            sock = self.sock
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.wake.set()

    # -- state for the main tick --

    def fresh_limit(self, ev):
        if self.channel != 'gxevents':
            return EVENTS_FRESH_S.get(ev, 10.0)
        rate = self.rate or GX_NAP_RATE
        key = RATE_KEY.get(ev)
        if key:
            return 3.0 * float(rate.get(key, GX_NAP_RATE[key])) / 1000.0 + 5.0
        return max(10.0, 3.0 * float(rate.get('csv4', GX_NAP_RATE['csv4'])) / 1000.0 + 5.0)

    def snapshot(self):
        now = self.clock()
        with self.lock:
            fresh = {ev for ev, t in self.frame_at.items() if now - t < self.fresh_limit(ev)}
            info = {'state': self.state, 'reason': self.reason, 'ident': self.ident, 'channel': self.channel,
                    'host': self.host, 'other': self.other, 'gx': self.gx, 'mismatch': self.mismatch,
                    'schema': self.schema, 'perf_schema_text': self.perf_schema_text, 'ever_live': self.ever_live,
                    'rate': self.rate, 'generation': self.generation,
                    'csv3_current': self.frame_at.get('CSVData3', -1e18) >= self.stream_at,
                    'frame_at': dict(self.frame_at)}
            return dict(self.frames), fresh, info

    def take_short(self):
        with self.lock:
            acc, self.short_acc = self.short_acc, {}
        return {name: s / n for name, (s, n) in acc.items() if n}

    # -- connection thread --

    def _sleep(self, s):
        self.wake.wait(s)
        self.wake.clear()

    def _run(self):
        while not self.stop.is_set():
            try:
                self._prepare()
            except NotFound as e:
                with self.lock:
                    if (self.state, self.reason) != ('missing', e.reason):
                        log.warning('%s not found: %s', label(self.uid), e.reason)
                    self.state, self.reason, self.other = 'missing', e.reason, e.other
                self._sleep(LOCATE_RETRY_S)
                continue
            except Exception as e:
                with self.lock:
                    self.state, self.reason = 'unreachable', fetch_error_text(e)
                log.warning('connect %s: %s', self.host, self.reason)
                self._sleep(LOCATE_RETRY_S)
                continue
            try:
                self._stream()
            except Exception as e:
                log.warning('stream: %s', fetch_error_text(e))
            with self.lock:
                was_streaming = self.state == 'streaming'
                self.state = 'lost'
                self.sock = None
            self._sleep(2 if was_streaming else LOCATE_RETRY_S)

    def _locate(self):
        other, reason = None, ''
        with self.lock:
            cached = self.host
        for host in dict.fromkeys(h for h in [cached] + self.discovery.hosts_for(self.uid) if h):
            try:
                st, _, body = Http(host).request('/identify')
            except Exception as e:
                reason = reason or 'no answer at %s (%s)' % (host, fetch_error_text(e))
                continue
            ident = xreg_ident(body) if st == 200 else None
            if ident is None:
                reason = reason or 'no XREG-010 at %s' % host
                continue
            self.discovery.note_ident(host, ident, 'link')
            if norm_uid(ident['uid']) == self.uid:
                return host, ident
            if other is None:
                other = (host, ident)
        self.discovery.request_sweep()
        if other:
            reason = '%s answers at %s' % (label(other[1]['uid'], other[1].get('name')), other[0])
        raise NotFound(other, reason or 'no address known yet')

    def _prepare(self):
        host, ident = self._locate()
        self.wake.clear()   # this link's own /identify note may have set it; only a later move should cut a wait short
        with self.lock:
            self.other = None
        http_ = Http(host)
        names = self._field_lists(http_)
        schema = dict(self.schema)
        perf_text = self.perf_schema_text
        try:
            alt = json.loads(http_.get_ok('/altschema'))
            schema['AltLive'], schema['AltSettings'] = alt.get('live', []), alt.get('settings', [])
        except Exception as e:
            log.warning('altschema: %s', fetch_error_text(e))
        try:
            raw = http_.get_ok('/perfschema')
            perf = json.loads(raw)
            schema['PerfLive'], schema['MotorLive'] = perf.get('live', []), perf.get('motorLive', [])
            schema['PerfSettings'] = perf.get('settings', [])
            perf_text = raw.decode('utf-8', 'replace')
        except Exception as e:
            log.warning('perfschema: %s', fetch_error_text(e))
        with self.lock:
            self.host, self.http = host, http_
            self.ident, self.names, self.schema, self.perf_schema_text = ident, names, schema, perf_text
            self.gx, self.mismatch = gx_version(ident) >= 2, None
            self.uid_mismatch = False

    def _field_lists(self, http_):
        path = os.path.join(self.cache_dir, 'fieldlists_%s.json' % uid6(self.uid).lower())
        cached = None
        try:
            with open(path) as f:
                cached = json.load(f)
        except (OSError, ValueError):
            pass
        headers = {'Accept-Encoding': 'gzip'}
        if cached and cached.get('etag'):
            headers['If-None-Match'] = cached['etag']
        st, hdr, body = http_.request('/script.js', headers=headers, timeout=60)
        if st == 304 and cached:
            return cached['fields']
        if st != 200:
            raise IOError('/script.js: HTTP %d' % st)
        js = gunzip_maybe(body).decode('utf-8', 'replace')
        fields = {ev: field_list(js, const) for ev, const in STREAMS.items()}
        etag = hdr.get('ETag') if hdr else None
        if etag:
            try:
                os.makedirs(self.cache_dir, exist_ok=True)
                tmp = path + '.tmp'
                with open(tmp, 'w') as f:
                    json.dump({'etag': etag, 'fields': fields}, f)
                os.replace(tmp, path)
            except OSError as e:
                log.warning('field list cache: %s', e)
        return fields

    def _open(self, path, read_timeout):
        conn = http.client.HTTPConnection(self.host, timeout=10)
        conn.connect()
        sock = conn.sock
        conn.request('GET', path, headers={'Accept': 'text/event-stream', 'Cache-Control': 'no-cache'})
        r = conn.getresponse()
        if r.status == 200 and 'text/event-stream' in (r.getheader('Content-Type') or ''):
            sock.settimeout(read_timeout)
            return r, sock
        r.close()
        conn.close()
        return None, None

    def _stream(self):
        if self.gx:
            # before the first GxRate, read with the nap schedule's patience (CSV1 every 10 s); never /events
            # instead, which would count this GX as a phone watching
            r, sock = self._open('/gxevents', 3.0 * GX_NAP_RATE['csv1'] / 1000.0 + 5.0)
            channel = 'gxevents'
        else:
            r, sock = self._open('/events', EVENTS_FRESH_S['CSVData'])
            channel = 'events'
        if r is None:
            raise IOError('/%s refused' % channel)
        with self.lock:
            self.state, self.reason, self.channel, self.sock, self.rate = 'streaming', '', channel, sock, None
            self.generation += 1
            self.stream_at = self.clock()
        log.info('%s streaming on /%s from %s', label(self.uid), channel, self.host)
        try:
            event = None
            while not self.stop.is_set():
                raw = r.readline()
                if not raw:
                    raise IOError('stream closed')
                line = raw.decode('utf-8', 'replace').rstrip('\r\n')
                if line.startswith('event:'):
                    event = line[6:].strip()
                elif line.startswith('data:'):
                    self.take(event, line[5:].lstrip(' '))
                elif not line:
                    event = None
                if self.uid_mismatch:
                    with self.lock:
                        self.frames, self.frame_at = {}, {}   # nothing from another unit stays on show
                    raise IOError('the address now answers as a different regulator')
        finally:
            r.close()

    def take(self, event, payload):
        now = self.clock()
        if event in STREAMS:
            names = self.names.get(event)
            vals = parse_counted(payload)
            if names is None or vals is None:
                return
            if len(vals) != len(names):
                with self.lock:
                    if self.mismatch is None:
                        log.warning('%s has %d fields, script.js lists %d', event, len(vals), len(names))
                    self.mismatch = (event, len(vals), len(names))
                return
            frame = dict(zip(names, vals))
            if event == 'CSVData2' and not self._same_unit(frame):
                self.uid_mismatch = True
                return
            with self.lock:
                self.frames[event] = frame
                self.frame_at[event] = now
                if event == 'CSVData':
                    self.ever_live = True
                    for name, field, scale, _conv, _dec in SHORT:
                        raw = frame.get(field)
                        if raw is not None and raw > EMPTY:
                            a = self.short_acc.setdefault(name, [0.0, 0])
                            a[0] += raw / scale
                            a[1] += 1
        elif event in SCHEMA_EVENTS:
            vals = parse_plain(payload)
            if vals is None:
                return
            with self.lock:
                # zip: a newer firmware appending a field before the schema refresh just drops the extra values
                self.frames[event] = dict(zip(self.schema.get(event, []), vals))
                self.frame_at[event] = now
        elif event == 'GxRate':
            try:
                rate = json.loads(payload)
            except ValueError:
                return
            if not isinstance(rate, dict):
                return
            with self.lock:
                self.rate = rate
                sock = self.sock
            if sock is not None:
                try:
                    sock.settimeout(max(10.0, 3.0 * float(rate.get('csv1', GX_NAP_RATE['csv1'])) / 1000.0 + 5.0))
                except (OSError, ValueError, TypeError):
                    pass

    def _same_unit(self, frame):
        up, lo = frame.get('deviceIdUpper'), frame.get('deviceIdLower')
        if up is None or lo is None:
            return True
        seen = '%08X%08X' % (int(up) & 0xFFFFFFFF, int(lo) & 0xFFFFFFFF)
        if seen != self.uid:
            log.warning('stream reports regulator %s, pinned %s', seen, self.uid)
            return False
        return True


# ---- Job threads ----

class Worker:
    def __init__(self, name):
        self.name = name
        self.q = queue.Queue()
        self.lock = threading.Lock()
        self.busy = collections.Counter()

    def start(self):
        threading.Thread(target=self._run, name=self.name, daemon=True).start()

    def put(self, kind, fn, dedupe=False):
        with self.lock:
            if dedupe and self.busy[kind]:
                return False
            self.busy[kind] += 1
        self.q.put((kind, fn))
        return True

    def stop(self):
        self.q.put((None, None))

    def _run(self):
        while True:
            kind, fn = self.q.get()
            if fn is None:
                return
            try:
                fn()
            except Exception:
                log.exception('%s job %s', self.name, kind)
            finally:
                with self.lock:
                    self.busy[kind] -= 1


# ---- Core ----

def ser_hash(serial):
    # The firmware's veSerHash (5_functions.ino): FNV-1a over the serial without spaces, 31 bits, never 0
    h = 2166136261
    for b in str(serial or '').encode('utf-8', 'replace'):
        if b in (0x20, 0x0D, 0x0A):
            continue
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    h &= 0x7FFFFFFF
    return h or 1


def _src(v):
    return None if v is None else 0.0 if v < FLOW_SRC_MIN_A else v


def _mode(dev):
    try:
        return int(dev.get('mode'))
    except (TypeError, ValueError):
        return None


def gx_class(key):
    # A device key's D-Bus class: 'dcdc:293' -> 'dcdc'; a key without a VRM instance is the service name
    key = str(key or '')
    if ':' in key:
        return key.split(':')[0]
    parts = key.split('.')
    return parts[2] if len(parts) > 3 and key.startswith('com.victronenergy.') else ''


def gx_kind(dev):
    # A GX device's kind from its D-Bus class (and a DC meter's monitor mode); None: not one the power flow lists
    cls = dev.get('cls')
    if cls == 'dcsource':
        return DCSOURCE_KIND.get(_mode(dev), 'source')
    if cls == 'dcload':
        return DCLOAD_KIND.get(_mode(dev), 'dcload')
    return GX_CLASS_KIND.get(cls)


def gx_roles(kind, pv=None):
    # 's' a source row, 'l' a load row, 'p' counts as solar on the boat (an MPPT, or an inverter/charger with PV); a
    # battery monitor and a DC system meter make no row
    roles = {'multi': 'sl', 'inverter': 'l', 'dcload': 'l', 'solar': 'p', 'battery': '', 'dcsystem': ''}.get(kind, 's')
    return roles + 'p' if pv is not None and kind in ('multi', 'inverter') else roles


def draws_house(key, m):
    # m: the device's memory entry. An Orion (class dcdc) the owner says draws from the house bank (memory "h"); the flag
    # on anything else (an older bridge offered it for a DC meter in DC-DC mode too) does nothing.
    return gx_class(key) == 'dcdc' and isinstance(m, dict) and m.get('h') == 1


def through_meter(key, roles, m):
    # Whether a device's current passes through the DC system meter: the owner's word (memory "m"), else Victron's layout
    # (gui-v2 lists DC load meters and DC-DC chargers under the DC system total; systemcalc counts every source, inverter
    # and Multi outside it): a DC load meter or an Orion drawing from the house bank inside, the rest outside
    if isinstance(m, dict) and m.get('m') in (0, 1):
        return m['m'] == 1
    return roles == 'l' and gx_class(key) in ('dcload', 'dcdc')


def n2k_unique(uid):
    # A regulator's NMEA 2000 NAME unique number, the low 21 bits of its chip ID (4_functions.ino SetDeviceInformation):
    # vecan-dbus has it from address claim, before the serial, as /N2kUniqueNumber and in the service name (_uc<n>)
    try:
        return int(uid, 16) & 0x1FFFFF
    except (TypeError, ValueError):
        return None


def stored_twin(key, m):
    # A remembered alternator with no serial under the twin's identity, not listed after the serial wait ("w"): how an
    # older bridge stored this regulator's NMEA 2000 twin when it came back at a demo exit with its name read before its
    # serial (bench round 7, 2026-10-28)
    return gx_class(key) == 'alternator' and isinstance(m, dict) and m.get('k') == 'altx' and not m.get('s') \
        and not m.get('w') and m.get('p') == ALT_N2K_PRODUCT


def gx_devices(gx, memory=None, uid='', uids=(), peers=()):
    # The GX's devices as the power flow and /gxDevices list them: each one on D-Bus now (gx['devices']), then each
    # remembered one that is not on D-Bus now, at 0 A (on 0). Left out: this bridge's own services, every pinned
    # regulator's NMEA 2000 twin (by its serial, or with none read yet by its NMEA 2000 unique number), and an alternator
    # until its serial is read or the reader has asked for it ALT_SERIAL_WAIT_S ("since"): a twin coming back names
    # itself before its /Serial reads, and a remembered device under its key stays listed as not on D-Bus meanwhile. The
    # other pinned regulators come from their own links instead (peers: uid, name, amps or None, live), at 0 A while one
    # is not streaming. A DC-DC charger that draws from the house bank is a load, its amps drawn from the battery bus
    # (negative), as an inverter's: an Orion when the owner says so (memory "h"; FakeGx marks its own), a DC meter in
    # DC-DC mode by its sign. A DC load meter with no name of its own is named by its monitor mode. meter: its current
    # passes through the DC system meter (through_meter).
    uids = list(uids) or [uid]
    ours = {alt_service(u) for u in uids} | {batt_service(u) for u in uids}
    twin = lambda serial: any(serial_matches(u, serial, uids) for u in uids)
    uniques = {n2k_unique(u) for u in uids} - {None}
    memory = memory or {}
    out, seen = [], set()
    for d in (gx or {}).get('devices') or []:
        kind = gx_kind(d)
        svc = str(d.get('service') or '')
        key = str(d.get('key') or svc)
        cls = str(d.get('cls') or '')
        serial = str(d.get('serial') or '').strip()
        n2k_twin = kind == 'altx' and not serial and d.get('n2k') in uniques
        if kind == 'altx' and not serial and not n2k_twin and svc not in ours \
                and (d.get('since') or 0.0) < ALT_SERIAL_WAIT_S:
            continue
        seen.add(key)
        if kind is None or svc in ours or n2k_twin or (kind == 'altx' and twin(d.get('serial'))):
            continue
        roles, amps, age, m = gx_roles(kind, d.get('pv')), d.get('amps'), d.get('age'), memory.get(key)
        if cls == 'dcdc' and (draws_house(key, m) or d.get('h') == 1):
            # the house bank feeds its input: the input current where it reports one (the output current leaves out the
            # conversion loss, and on a 12 to 24 V charger more than half the draw)
            if d.get('in_amps') is not None:
                amps, age = d['in_amps'], d.get('in_age')
            roles = 'l'
            amps = None if amps is None else -amps or 0.0   # never -0.0
        elif cls == 'dcsource' and kind == 'dcdc':
            # + into the house bank, as every Victron DC source reads; inside the deadband it stays where it was last seen
            if amps is not None and abs(amps) >= FLOW_SRC_MIN_A:
                roles = 'l' if amps < 0 else 's'
            elif isinstance(m, dict) and m.get('r') in ('s', 'l'):
                roles = m['r']
        name = str(d.get('name') or '').strip() or (DCLOAD_NAME.get(_mode(d), '') if cls == 'dcload' else '')
        out.append({'key': key, 'service': svc, 'cls': cls, 'kind': kind, 'roles': roles, 'name': name,
                    'product': str(d.get('product') or '').strip(), 'serial': str(d.get('serial') or ''),
                    'n2k': d.get('n2k'), 'amps': amps, 'pv': d.get('pv'), 'age': age, 'on': 1,
                    'meter': through_meter(key, roles, m)})
    # with no fresh GX reading at all, whether a remembered device is on is not known either
    for key, m in sorted(memory.items()):
        if key in seen or not isinstance(m, dict) or m.get('k') not in FLOW_SOURCE_KINDS + FLOW_LOAD_KINDS \
                or twin(m.get('s')) or (not m.get('s') and m.get('u') in uniques) or stored_twin(key, m):
            continue
        roles = 'l' if draws_house(key, m) else str(m.get('r') or '')
        out.append({'key': key, 'service': '', 'cls': gx_class(key), 'kind': m['k'], 'roles': roles,
                    'name': str(m.get('n') or ''), 'product': str(m.get('p') or ''), 'serial': str(m.get('s') or ''),
                    'n2k': m.get('u'), 'amps': 0.0 if gx else None, 'pv': None, 'age': None, 'on': 0,
                    'meter': through_meter(key, roles, m)})
    for p in peers:
        if p['uid'] == uid:
            continue
        key = 'xreg:' + p['uid']
        out.append({'key': key, 'service': '', 'cls': '', 'kind': 'altx', 'roles': 's',
                    'name': p['name'], 'product': 'XREG-010', 'serial': p['uid'], 'n2k': None,
                    'amps': p['amps'] if p['live'] else 0.0, 'pv': None, 'age': 0.0 if p['live'] else None,
                    'on': int(bool(p['live'])), 'meter': through_meter(key, 's', memory.get(key))})
    return out


def flow_model(reg, gx, devs, uid='', uids=(), memory=None):
    # The power-flow numbers: the regulator's own battery and solar readings where it has one, the GX's (GxReader or
    # FakeGx shape) only where it has none, and a row per GX device (devs: gx_devices). reg: batt, alt, solar (A or None),
    # src (flowSrc), ser (flowSolarSer, 0 = none). Sources and loads under FLOW_SRC_MIN_A read 0. memory: the remembered
    # devices, for the owner's word on this regulator's own alternator and the DC system meter ("xreg:<uid>").
    src = reg.get('src')
    bsrc, ssrc = (int(src) // 10, int(src) % 10) if src is not None else (0, 3)
    batt, solar = reg.get('batt'), reg.get('solar')
    batt_from = GX_BATT_FROM.get(bsrc, '') if batt is not None else ''
    solar_from = 'VE.Direct' if solar is not None and ssrc in (1, 2) else ''
    gx = gx or {}
    # systemcalc's battery current, only from a battery monitor: with none it estimates one from Multi and solar
    # chargers alone, which leaves the alternators out
    gb, gsvc = gx.get('batt'), str(gx.get('batt_service') or '')
    if batt is None and gb is not None and gsvc.startswith(BATT_SERVICE) \
            and gsvc not in [batt_service(u) for u in uids or [uid]]:
        batt, batt_from = gb, 'GX'
    pv = gx.get('pv')
    mppts = [d for d in devs if d['kind'] == 'solar' and d['on']]
    if pv is not None:
        if ssrc in (1, 2) and solar is not None:
            # A VE.Can MPPT can be wired to both: the one whose serial the regulator reports is already in its reading.
            # Without that serial (older firmware) the GX's cannot be told from it, so the regulator's stands alone.
            ser = reg.get('ser') or 0
            if ser:
                mine = sum(m['amps'] for m in mppts if m['amps'] is not None and ser_hash(m['serial']) == ser)
                extra = pv - mine
                if extra >= FLOW_SRC_MIN_A:
                    solar, solar_from = solar + extra, 'VE.Direct + GX'
        elif mppts or pv >= FLOW_SRC_MIN_A:
            # no MPPT on the regulator's VE.Direct port: none configured (0 A), or an array it cannot see
            solar, solar_from = max(0.0, pv), 'GX'
    alt, solar = _src(reg.get('alt')), _src(solar)
    # the boat has solar: an array set on the regulator (flowSrc solar code 0 = none), or an MPPT on the GX, now or
    # remembered; the row then stays at 0.0 A through the night
    has_solar = ssrc != 0 or any('p' in d['roles'] for d in devs) or (solar or 0.0) > 0.0
    rows = [{'id': 'alt', 'k': 'alt', 'a': alt, 'c': ''}]
    if has_solar:
        rows.append({'id': 'solar', 'k': 'solar', 'a': solar, 'c': solar_from if solar is not None else ''})
    order = {k: i for i, k in enumerate(FLOW_SOURCE_KINDS + FLOW_LOAD_KINDS)}
    # the rows whose current passes through the DC system meter: sources it nets out, loads it already counts
    srcs, loads, in_srcs, in_loads = [], [], [], []
    if through_meter('xreg:' + uid, 's', (memory or {}).get('xreg:' + uid)):
        in_srcs.append(rows[0])
    for d in devs:
        a, share = d['amps'], d['pv'] or 0.0   # an inverter/charger's own PV is in the GX's solar
        cap = d['name'] or d['product']
        if 's' in d['roles']:
            srcs.append({'id': d['key'], 'k': d['kind'], 'a': None if a is None else _src(max(0.0, a - share)),
                         'c': cap})
            if d.get('meter'):
                in_srcs.append(srcs[-1])
        if 'l' in d['roles']:
            loads.append({'id': d['key'] + ('/load' if 's' in d['roles'] else ''),
                          'k': {'multi': 'inverter', 'dcdc': 'dcdcload'}.get(d['kind'], d['kind']),
                          'a': None if a is None else _src(max(0.0, share - a)), 'c': cap})
            if d.get('meter'):
                in_loads.append(loads[-1])
    ranked = lambda r: (order[r['k']], r['c'].lower(), r['id'])
    rows += sorted(srcs, key=ranked) + sorted(loads, key=ranked)
    house = None
    x = 0.0
    missing = next((r for r in rows if r['a'] is None), None)
    sources = sum(r['a'] for r in rows if r['k'] in FLOW_SOURCE_KINDS and r['a'] is not None)
    net = None if batt is None or missing is not None \
        else sources - batt - sum(r['a'] for r in rows if r['k'] in FLOW_LOAD_KINDS)
    meters = [d['amps'] for d in devs if d['kind'] == 'dcsystem' and d['on']]
    inside = in_srcs + in_loads
    metered = -sum(meters) + sum(r['a'] for r in in_srcs) - sum(r['a'] for r in in_loads) \
        if meters and None not in meters and all(r['a'] is not None for r in inside) else None
    if metered is not None:
        for r in inside:
            r['in'] = 1
        why, house = '', max(0.0, metered)
        # The meter measures House; past the sensors' tolerance the calculated rest disagrees because of something no
        # meter sees: a load (a windlass or thruster wired to the battery, say) when the sources give more, a source when
        # they give less
        if net is not None and abs(net - metered) > max(FLOW_UNMEASURED_A, FLOW_GAP_FRAC * sources):
            why, x = ('gap', net - metered) if net > metered else ('unmeasured', metered - net)
    elif batt is None:
        why = 'batt'
    elif missing is not None:
        why = missing['k']
    elif net < -FLOW_UNMEASURED_A:
        # more into the battery than the measured sources give: an unmeasured source, never a negative house load. Up to
        # FLOW_UNMEASURED_A it is sensor offset, and House reads 0.
        why, x = 'unmeasured', -net
    else:
        why, house = '', max(0.0, net)
    return {'batt': batt, 'batt_from': batt_from, 'rows': rows, 'house': house, 'hm': int(metered is not None),
            'why': why, 'x': x}


def device_post(devs, gx):
    # /gxDevices rows: [kind, name, product ('' when the name is the product), amps (the device's own DC current, +
    # into the battery bus, - drawn from it: an inverter, a DC load, a DC-DC charger that draws from the house bank, the
    # DC system), reading age s, on D-Bus now 1 / 0], the battery monitor systemcalc uses first
    order = {k: i for i, k in enumerate(('battery',) + FLOW_SOURCE_KINDS + FLOW_LOAD_KINDS + ('dcsystem',))}
    bsvc = str((gx or {}).get('batt_service') or '')
    rows = []
    for d in sorted(devs, key=lambda d: (order.get(d['kind'], 99), d['service'] != bsvc, not d['on'],
                                          (d['name'] or d['product']).lower(), d['key'])):
        name = d['name'] or d['product']
        rows.append([d['kind'], name, d['product'] if d['product'] != name else '',
                     None if d['amps'] is None else round(d['amps'], 2),
                     None if d['age'] is None else round(max(0.0, d['age']), 1), d['on']])
    return rows[:DEVPOST_MAX]


class FakeGx:
    # Demo only (Hardware Present? = No): the GX-side readings of a boat whose regulator measures only its alternator,
    # made up around the regulator's simulated boat so the tile, its names and the regulator's GX device list show
    # without the devices. A battery monitor, two named MPPTs, a shore charger, an inverter, an Orion XS charging the
    # start battery from the house bank and a DC load meter on the fridge. Never published on D-Bus (a battery service
    # on a real GX can steer DVCC): it only feeds the power flow and /gxDevices. The shore charger, the fridge and the
    # Orion and the inverter are the regulator's simulation's own (CSV4 demoShoreA, demoFridgeA, demoOrionA,
    # demoInverterA: the charger only alongside the town dock with the engine off, the Orion after an engine start, the
    # inverter early in the sail leg), so the battery here and the simulated battery are one sum; firmware that sends
    # none reads 0. The two arrays share the simulated sun (its clouds included), split by how the boat lies to it; the
    # monitor reads the sources less the simulated loads, with its own offset and noise.
    SUN_AZ = 200.0          # deg; the demo's sun is an afternoon sun whatever the clock
    SERIALS = ('HQ2241DMO1A', 'HQ2241DMO2B')
    # (service, class, key, name, product, monitor mode): the made-up devices, named the way an owner and Victron would
    DEVICES = (('demo.battery', 'battery', 'battery:512', '', 'SmartShunt 500A/50mV', None),
               ('demo.mppt1', 'solarcharger', 'solarcharger:278', 'Bimini array', 'SmartSolar MPPT 100/30', None),
               ('demo.mppt2', 'solarcharger', 'solarcharger:279', 'Deck array', 'SmartSolar MPPT 75/15', None),
               ('demo.charger', 'charger', 'charger:280', '', 'Phoenix Smart IP43 Charger 12/30', None),
               ('demo.inverter', 'inverter', 'inverter:281', '', 'Phoenix Inverter 12/800 Smart', None),
               ('demo.orion', 'dcdc', 'dcdc:282', 'Start battery charger', 'Orion XS 12/12-50A', None),
               ('demo.fridge', 'dcload', 'dcload:283', 'Fridge', 'SmartShunt 500A/50mV', 3))
    ORION_EFF = 0.95        # its output over its input current, 12 V to 12 V

    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.t = None
        self.share_ou = 0.0
        self.noise = 0.0

    def _ou(self, x, tau, sigma, dt):
        a = math.exp(-dt / tau)
        return a * x + sigma * math.sqrt(1.0 - a * a) * self.rng.gauss(0.0, 1.0)

    def step(self, now, reg, heading=None):
        # reg: alt, solar, batt (flowAltA, flowSolarA, flowBattA) and shore, fridge, orion, inverter (demoShoreA,
        # demoFridgeA, demoOrionA, demoInverterA), A or None
        dt = 0.5 if self.t is None else min(max(now - self.t, 0.0), 5.0)
        self.t = now
        self.share_ou = self._ou(self.share_ou, 90.0, 0.03, dt)
        self.noise = self._ou(self.noise, 1.5, 1.0, dt)
        lie = math.sin(math.radians((heading if heading is not None else self.SUN_AZ) - self.SUN_AZ))
        share = min(0.85, max(0.25, 0.58 + 0.12 * lie + self.share_ou))
        alt, solar, batt = reg.get('alt'), reg.get('solar'), reg.get('batt')
        charger, fridge, orion, inverter = (round(max(0.0, reg.get(k) or 0.0), 1)
                                            for k in ('shore', 'fridge', 'orion', 'inverter'))
        q = lambda v: None if v is None else round(v, 1)
        s1 = s2 = pv = gb = None
        if solar is not None:
            s1 = q(max(0.0, solar) * share)
            s2 = q(max(0.0, solar) - s1)
            pv = round(s1 + s2, 1)
        if alt is not None and solar is not None and batt is not None:
            house = alt + solar + charger - batt   # the simulated loads, the fridge, the Orion and the inverter among them
            gb = q(1.004 * (alt + pv + charger - house) + 0.04 + 0.03 * self.noise)
        amps = {'demo.battery': gb, 'demo.mppt1': s1, 'demo.mppt2': s2, 'demo.charger': charger,
                'demo.inverter': -inverter or 0.0, 'demo.orion': round(self.ORION_EFF * orion, 1),
                'demo.fridge': -fridge or 0.0}
        serial = {'demo.mppt1': self.SERIALS[0], 'demo.mppt2': self.SERIALS[1]}
        devices = [{'service': svc, 'cls': cls, 'key': key, 'name': name, 'product': product,
                    'serial': serial.get(svc, ''), 'mode': mode, 'amps': amps[svc], 'pv': None, 'age': 0.4}
                   for svc, cls, key, name, product, mode in self.DEVICES]
        for d in devices:
            if d['cls'] == 'dcdc':
                # draws from the house bank: FakeGx's own flag, never the owner's remembered one (no store sees FakeGx)
                d.update(h=1, in_amps=orion, in_age=0.4)
        return {'batt': gb, 'batt_service': BATT_SERVICE + 'demo', 'pv': pv, 'devices': devices, 'at': now}


class Core:
    # One pinned regulator: its link, recorders and settings, published as its own D-Bus service
    def __init__(self, uid, store, schema_text='', discovery=None, cache_dir=CACHE_DIR, clock=time.monotonic,
                 wall=time.time, instance=INSTANCE, alt_instance=INSTANCE, batt_instance=INSTANCE, on_repin=None):
        self.clock, self.wall = clock, wall
        self.uid = norm_uid(uid)
        self.store = store   # 'host': where this regulator last answered
        self.host = (store.get('host') or '').strip()
        self.instance = instance
        self.alt_instance = alt_instance
        self.batt_instance = batt_instance
        self.batt_ok = False       # the Manager's say: this GX can take a demo battery service from this regulator
        self.on_repin = on_repin or (lambda old, new, host: False)
        self.schema_text = schema_text or ''
        self.model = SettingsModel.from_text(self.schema_text)
        self.link = Link(self.uid, self.host, discovery or Discovery(browse=False), cache_dir, clock)
        self.inbox = queue.Queue()
        self.writer = Worker('writer')     # writes and schedule requests: small and in order
        self.fetcher = Worker('fetcher')   # file downloads, so a 600 KB ring never delays a write
        self.cloud_worker = Worker('cloud')   # internet calls, so a slow cloud never delays the regulator
        self.src = 'gx' + secrets.token_hex(4)
        self.n = 0
        self.hold = {}
        self.want_at = {}
        self.want_on = dict.fromkeys(WANT_TOPICS, False)
        self.trend_want_at = {}   # Health window -> when a screen last asked for it
        self.trend_on = dict.fromkeys(HEALTH_WINDOWS, False)
        self.long_range = '24h'
        self.long_dirty = False
        self.lt = None
        self.lt_at = None
        self.next = collections.defaultdict(float)   # next due time per periodic job
        self.short = ShortRecorder()
        self.wind_plot = WindRecorder()
        self.hist = {name: collections.deque(maxlen=HIST_LEN) for name, _ in HISTORY}
        self.wind = {name: collections.deque(maxlen=WIND_LEN) for name, _, _ in WIND_HIST}
        self.wind_acc = {name: [] for name, _, _ in WIND_HIST}
        self.baro = []
        self.baro_end = None       # epoch of the newest barometer sample
        self.health_csv = {'SessionCsv': ''}
        self.health_trend = dict.fromkeys(HEALTH_WINDOWS, '')
        self.curve_csv = ''
        self.sources = {}   # endpoint name -> (parsed JSON, raw text, fetched at)
        self.result_seq = 0
        self.reset_paths = set()
        self.added = set()
        self.fast_held = False
        self.fast_next = 0.0
        self.last_generation = 0
        self.last_name = None
        self.live = False
        self.status = ''
        # DVCC over WiFi: what systemcalc writes to this regulator's alternator service, forwarded to /gxLimits
        self.alt_writes = {}       # /Link path -> (value, clock) of systemcalc's last SetValue, equal values included
        self.alt_twin = False      # an NMEA 2000 service carries this regulator: ours carries no measurements
        self.alt_native = False    # another alternator service controls this regulator: ours steps aside
        self.alt_shared = False    # another controllable alternator that can charge splits DVCC's current with this one
        self.alt_quiet = False     # a new alternator service is not yet checked for being this regulator's twin
        self.able = False          # free to charge (_can_charge), as of the last tick
        self.limits_sent = False   # the last /gxLimits post carried a value, so an empty one is owed when that stops
        self.limits_hold = -1      # link generation a refused post stopped posting for
        self.demo = False          # the regulator is in demo mode (Hardware Present? = No)
        self.demo_gen = -1         # link generation /demoBackfill was fetched for in this demo spell
        self.demo_tries = 0        # failed /demoBackfill fetches since the entry or reconnect
        self.switch_frames = {}    # the frames on hand at the last demo switch: the other boat's, kept out of the history
        self.wind_marker_gen = -1  # link generation whose CSV4 carried the empty marker for apparent wind (wind_stale)
        self.republish = False     # history rings were dropped or refilled: publish them on this tick
        self.flow_state = None     # battery word: 1 charging, 0 resting, -1 discharging; None no reading
        self.flow_flip_at = None   # when the battery started reading another way
        self.flow_imb = None       # (the imbalance reason, 'unmeasured' or 'gap', when it began), until it holds
        self.flow_held = False
        self.flow_next = 0.0
        self.flow_devs = None      # gx_devices() of the last tick with a GX reading, for /gxDevices
        self.gx_side = None        # the GX's own readings for the power flow (set_gx_side), real mode only
        self.gx_uids = []          # every pinned regulator, to tell this one's NMEA 2000 twin from another alternator
        self.gx_memory = {}        # the Manager's remembered GX devices
        self.gx_peers = []         # the other pinned regulators' alternators (Manager.set_gx_side)
        self.fake_gx = None        # FakeGx while in demo
        self.devpost_every = DEVPOST_EVERY_S   # what the regulator last asked for
        self.devpost_rate = None   # s, this stream's GxRate "devPost" (None: firmware that sends none)
        self.devpost_hold = -1     # link generation a refused /gxDevices post stopped posting for
        self.devpost_keys = None   # the device set of the last post: a change posts at once
        self.devpost_at = 0.0
        self.cloud_state = CLOUD_LOADING
        self.cloud_msg = ''
        self.cloud_text = ''       # the page model (xreg_cloud.view) as JSON
        self.cloud_as_of = None
        self.cloud_at = None       # clock of the last good bundle
        self.cloud_next = 0.0
        self.cloud_busy = False
        self.cloud_refresh = False
        self.cloud_demo = False    # cloud_text is the canned demo bundle

    def start(self):
        self.link.start()
        self.writer.start()
        self.fetcher.start()
        self.cloud_worker.start()

    def stop(self):
        self.link.close()
        self.writer.stop()
        self.fetcher.stop()
        self.cloud_worker.stop()

    def summary(self):
        # This regulator's row in the Manager's /Units
        _, _, info = self.link.snapshot()
        return {'uid': self.uid, 'id': uid6(self.uid), 'name': self.last_name or '', 'host': self.host,
                'fw': info['ident'].get('fw', ''), 'service': unit_service(self.uid), 'instance': self.instance,
                'connected': int(self.live), 'status': self.status}

    # -- paths --

    def static_paths(self, process_name='xregulator_bridge.py'):
        # (path, initial value, write mode): None read-only, 'free' accepted as written, 'core' handled by on_write
        group = label(self.uid)   # two regulators' switches must not share one group
        p = [('/Mgmt/ProcessName', process_name, None), ('/Mgmt/ProcessVersion', VERSION, None),
             ('/Mgmt/Connection', 'WiFi ' + self.host if self.host else '', None),
             ('/DeviceInstance', self.instance, None), ('/ProductId', 0xFFFF, None),
             ('/ProductName', 'XREG-010 alternator regulator', None), ('/CustomName', '', 'free'),
             ('/FirmwareVersion', '', None), ('/Serial', self.uid, None), ('/Connected', 0, None),
             ('/State', 0x100, None)]
        o = '/SwitchableOutput/0'
        p += [(o + '/Name', 'Alternator', None), (o + '/State', None, 'core'), (o + '/Status', 0, None),
              (o + '/Settings/Type', 1, 'core'), (o + '/Settings/ValidTypes', 1 << 1, None),
              (o + '/Settings/Group', group, 'free'), (o + '/Settings/CustomName', '', 'free')]
        for i, (name, unit, decimals) in enumerate((('Alternator current', 'A', 1), ('Battery voltage', 'V', 2),
                                                     ('Field', '%', 0))):
            g = '/GenericInput/%d' % i
            p += [(g + '/Name', name, None), (g + '/Value', None, None), (g + '/Status', 0, None),
                  (g + '/Settings/Type', 1, None), (g + '/Settings/ValidTypes', 1 << 1, None),
                  (g + '/Settings/Unit', unit, None), (g + '/Settings/Decimals', decimals, None),
                  (g + '/Settings/Group', group, 'free'), (g + '/Settings/CustomName', '', 'free')]
        p += [('/Live/' + name, None, None) for name, *_ in LIVE]
        p += [('/Live/AltCapMax', None, None)]
        p += [('/History/' + name, '', None) for name, _ in HISTORY]
        p += [('/History/IntervalS', HIST_EVERY_S, None), ('/History/Baro', '', None), ('/History/BaroEnd', None, None)]
        p += [('/History/' + name, '', None) for name, _, _ in WIND_HIST]
        p += [('/Ui/Want/' + t, None, 'core') for t in WANT_TOPICS]
        p += [('/Ui/LongRange', self.long_range, 'core')]
        p += [('/Ui/Want/HealthTrend/%d' % h, None, 'core') for h in HEALTH_WINDOWS]
        p += [('/Plot/Short/' + name, '', None) for name, *_ in SHORT] + [('/Plot/Short/chargeStage', '', None)]
        p += [('/Plot/Long/Fields', '', None), ('/Plot/Long/StartEpoch', None, None), ('/Plot/Long/StepS', None, None),
              ('/Plot/Long/Range', self.long_range, None), ('/Plot/Long/Last', '', None)]
        p += [('/Plot/Long/' + name, '', None) for name in self._long_series_names()]
        p += [('/Plot/Wind/' + name, '', None) for name, *_ in WIND_PLOT]
        p += [('/Health/TrendCsv/%d' % h, '', None) for h in HEALTH_WINDOWS] + [('/Health/SessionCsv', '', None)]
        p += [('/Perf/Schema', '', None), ('/Perf/CurveCsv', '', None), ('/Perf/RidgeFrac', None, None),
              ('/Perf/IdwPower', None, None)]
        p += [('/Settings/Schema', self.schema_text, None), ('/Settings/Result', '', None)]
        p += [('/Settings/Http/' + name, '', None) for name in self.model.endpoints]
        if self.model.probe_roles:
            p.append(('/Settings/ProbeRole', '', 'core'))
        for key in self.model.values:
            item = self.model.items[key]
            p.append(('/Settings/Value/' + key, None, 'core' if self.model.writeable(item) else None))
        p += [('/Settings/Action/' + key, None, 'core') for key in self.model.actions]
        p += [('/Settings/Table/' + tid, '', 'core') for tid in self.model.tables]
        p += [('/Link/Channel', '', None), ('/Link/Firmware', '', None), ('/Link/Status', '', None),
              ('/Link/Host', self.host, None), ('/Link/OtherUid', '', None), ('/Link/OtherName', '', None),
              ('/Link/AcceptRegulator', None, 'core')]
        p += [('/Flow/Json', '', None), ('/Flow/Ok', None, None)]
        p += [('/Cloud/State', None, None), ('/Cloud/Message', '', None), ('/Cloud/AsOf', None, None),
              ('/Cloud/Json', '', None), ('/Cloud/Refresh', None, 'core')]
        p += [('/Link/GxLimits/State', LIMITS_NONE, None), ('/Link/GxLimits/Cvl', None, None),
              ('/Link/GxLimits/Ccl', None, None)]
        return p

    @staticmethod
    def _long_series_names():
        names = []
        for n, *_ in LT_ENV + LT_TAIL_ENV:
            names += [n, n + 'Min', n + 'Max']
        names += [n for n, _, _ in LT_AVG]
        names += [n + 'OnAvg' for n, _ in LT_ONAVG]
        return names + ['chargeStage', 'cover']

    def _dynamic(self, pub, info):
        for ev, prefix in (('AltLive', '/Health/Live/'), ('PerfLive', '/Perf/Live/'), ('MotorLive', '/Perf/Motor/')):
            for name in info['schema'].get(ev, []):
                path = prefix + name
                if path not in self.added:
                    pub.add(path, None, None)
                    self.added.add(path)

    def want(self, topic, now):
        return now - self.want_at.get(topic, -1e18) < WANT_HOLD_S

    # -- writes from D-Bus (main thread) --

    def trend_wanted(self, hours, now):
        return now - self.trend_want_at.get(hours, -1e18) < WANT_HOLD_S

    def on_write(self, path, value):
        now = self.clock()
        if path.startswith('/Ui/Want/HealthTrend/'):
            # a screen showing the Health trend for this window, written every 15 s as the other /Ui/Want/ paths are
            try:
                hours = int(path[21:])
            except ValueError:
                return False
            if hours not in HEALTH_WINDOWS:
                return False
            self.trend_want_at[hours] = now
            return True
        if path.startswith('/Ui/Want/'):
            topic = path[9:]
            if topic not in WANT_TOPICS:
                return False
            self.want_at[topic] = now
            return True
        if path == '/Cloud/Refresh':
            self.cloud_refresh = True
            self.cloud_next = 0.0
            self.reset_paths.add(path)
            return True
        if path == '/Ui/LongRange':
            if value not in LONG_RANGES:
                return False
            if value != self.long_range:
                self.long_range = value
                self.long_dirty = True
            return True
        if path == '/SwitchableOutput/0/Settings/Type':
            return value == 1
        if path == '/SwitchableOutput/0/State':
            try:
                v = int(value)
            except (TypeError, ValueError):
                return False
            if v not in (0, 1):
                return False
            self.hold[path] = [v, now, None]
            self._queue_write('OnOff', [('OnOff', str(v))], arm=(v == 1), control=True, hold=[(path, v)])
            return True
        if path.startswith('/Settings/Value/'):
            key = path[16:]
            item = self.model.items.get(key)
            if item is None or not self.model.writeable(item):
                return False
            try:
                v = self.model.check(item, value)
            except ValueError as e:
                self._post_result(key, False, str(e))
                return False
            self.hold[path] = [v, now, None]
            self._queue_write(key, [(key, self.model.wire(item, v))], arm=item.get('arm', True), hold=[(path, v)],
                              control=bool(item.get('tagged')) or key in ('OnOff', 'ManualFieldToggle'))
            return True
        if path.startswith('/Settings/Action/'):
            key = path[17:]
            item = self.model.items.get(key)
            if item is None or key not in self.model.actions:
                return False
            self.reset_paths.add(path)
            endpoint = item.get('endpoint')
            self._queue_write(key, [] if endpoint else [(key, '1')], arm=item.get('arm', True), endpoint=endpoint,
                              method=item.get('method'))
            return True
        if path.startswith('/Settings/Table/'):
            tid = path[16:]
            if tid not in self.model.tables:
                return False
            try:
                edits = json.loads(value)
                if not isinstance(edits, dict):
                    raise ValueError('not a JSON object')
                params = self.model.table_params(tid, edits, self._echo_frames(now))
            except ValueError as e:
                self._post_result(tid, False, str(e))
                return False
            hold = [(path, value)]
            self.hold[path] = [value, now, None]
            sent = {k for k, _ in params}
            for k, v in edits.items():
                if k in sent and k in self.model.values:
                    shown = self.model.check(self.model.items[k], v)
                    self.hold['/Settings/Value/' + k] = [shown, now, None]
                    hold.append(('/Settings/Value/' + k, shown))
            self._queue_write(tid, params, arm=self.model.tables[tid]['item'].get('arm', True), hold=hold)
            return True
        if path == '/Settings/ProbeRole':
            try:
                req = json.loads(value)
                pid, old, new = str(req['id']).lower(), int(req['from']), int(req['to'])
            except (ValueError, KeyError, TypeError):
                self._post_result('ProbeRole', False, 'Not a probe role change')
                return False
            roles = self.model.probe_roles
            if not re.fullmatch(r'[0-9a-f]{16}', pid) or not -1 <= old < len(roles) or not -1 <= new < len(roles) \
                    or old == new:
                self._post_result('ProbeRole', False, 'Not a probe role change')
                return False
            # The app's owWriteRole(): clear the old role first, then bind the new one, as two requests (the firmware
            # refuses an id still bound to another role, and handles the roles in index order within one request)
            steps = ([(roles[old], 'none')] if old >= 0 else []) + ([(roles[new], pid)] if new >= 0 else [])
            self.reset_paths.add(path)
            self._queue_steps('ProbeRole', steps, self.model.probe_arm)
            return True
        if path == '/Link/AcceptRegulator':
            # "Use this regulator": the different unit answering at this one's address takes its place
            other = self.link.snapshot()[2]['other']
            new = norm_uid(value)
            if not other or not new or norm_uid(other[1].get('uid')) != new or not self.on_repin(self.uid, new,
                                                                                                   other[0]):
                return False
            self.reset_paths.add(path)
            return True
        return False

    def _echo_frames(self, now):
        frames = dict(self.link.snapshot()[0])
        for name, (parsed, _text, at) in self.sources.items():
            if now - at < max(30.0, 3.0 * self.model.endpoints.get(name, ('', SOURCE_POLL_S))[1]):
                frames['HTTP:' + name] = parsed
        return frames

    def _post_result(self, key, ok, message):
        self.inbox.put(('result', (key, ok, message)))

    # -- authorization --

    def _auth(self):
        _, _, info = self.link.snapshot()
        if not info['ident'] or info['state'] == 'missing':
            return 'down', None
        return ('gx' if info['gx'] else 'legacy'), info

    def _queue_write(self, key, params, arm=True, control=False, endpoint=None, method=None, hold=()):
        self.n += 1
        n = self.n
        self.writer.put('write', lambda: self._write_job(key, params, arm, control, endpoint, method, n, list(hold)))

    def _queue_steps(self, key, steps, arm=True):
        # Requests that must land one after another; the first refusal ends the sequence. One result for all.
        ns = []
        for _ in steps:
            self.n += 1
            ns.append(self.n)

        def job():
            ok, msg = True, ''
            for (k, v), n in zip(steps, ns):
                ok, msg = self._write(k, [(k, v)], arm, False, None, None, n)
                if not ok:
                    break
            if ok:
                self.inbox.put(('sources_now', None))
            self._post_result(key, ok, msg)
        self.writer.put('write', job)

    def _write_job(self, key, params, arm, control, endpoint, method, n, hold):
        ok, msg = self._write(key, params, arm, control, endpoint, method, n)
        for p in hold:
            self.inbox.put(('accepted' if ok else 'unhold', p))
        if ok:
            self.inbox.put(('sources_now', None))
        self._post_result(key, ok, msg)

    def _write(self, key, params, arm, control, endpoint, method, n):
        mode, info = self._auth()
        if mode == 'down':
            return False, 'Not connected to %s' % label(self.uid, self.last_name)
        # uid= on every write: the firmware refuses (409) one that reaches another unit
        tags = [('src', self.src), ('n', str(n)), ('uid', self.uid)]
        if mode == 'gx':
            tags.append(('gx', '1'))
        path = endpoint or '/get'
        post = (method or 'GET').upper() == 'POST'
        query = tags if post else params + tags
        body = params if post else None
        http_ = self.link.http
        if mode == 'legacy' and arm:
            return self._legacy_write(http_, path, query, body, control)
        try:
            st, _, b = self._send(http_, path, query, body, control)
        except Exception as e:
            return False, 'No answer from the regulator (%s)' % fetch_error_text(e)
        if st == 409 and re.fullmatch(rb'\s*[0-9A-Fa-f]{16}\s*', b or b''):
            self.link.relocate()
        return self._answer(st, b)

    def _send(self, http_, path, query, body, control):
        if not control:
            return http_.request(path, query=query, body=body, timeout=8)
        # The app's control-write pattern: the same URL (same n, so the regulator drops repeats), a hedge copy after
        # 500 ms unanswered on the first try, up to 3 tries on silence; any HTTP status is an answer.
        def one(answers):
            try:
                answers.put(('ok', http_.request(path, query=query, body=body, timeout=1.6)))
            except Exception as e:
                answers.put(('err', e))
        err = None
        for attempt in range(3):
            answers = queue.Queue()
            threading.Thread(target=one, args=(answers,), daemon=True).start()
            inflight = 1
            deadline = time.monotonic() + 1.7
            hedged = attempt > 0
            while inflight:
                wait = (0.5 if not hedged else deadline - time.monotonic())
                try:
                    kind, res = answers.get(timeout=max(0.01, wait))
                except queue.Empty:
                    if not hedged:
                        hedged = True
                        threading.Thread(target=one, args=(answers,), daemon=True).start()
                        inflight += 1
                        deadline = time.monotonic() + 1.7
                        continue
                    break
                if kind == 'ok':
                    return res
                err = res
                inflight -= 1
        raise err or IOError('timed out')

    def _answer(self, st, body):
        text = (body or b'').decode('utf-8', 'replace').strip()
        if 200 <= st < 300:
            # /get answers 200 even when it refuses a value ("Rejected: ...") or matched no setting at all
            if text.startswith('Rejected'):
                return False, text
            if text.startswith('No message sent'):
                return False, 'The regulator did not recognize this setting or its value'
            return True, ''
        if st == 409:
            if text.startswith('Turn the alternator off'):
                return False, 'Turn the alternator off before changing the RPM scaling factor.'
            if not text or re.fullmatch(r'[0-9A-Fa-f]{16}', text):
                return False, 'A different regulator answered at this address'
        return False, text or 'Refused (HTTP %d)' % st

    def _legacy_write(self, http_, path, query, body, control):
        # Firmware before GX link version 2: unlock for this one write and put the lock back as it was. The unlock
        # is one shared flag on the regulator, so this briefly unlocks the phone too.
        try:
            st, _, b = http_.request('/armSettings', timeout=5)
            was_armed = st == 200 and json.loads(b).get('armed') == 1
            if not was_armed:
                st, _, b = http_.request('/armSettings', body=b'arm=1', timeout=5)
                if st == 403:
                    return False, 'The regulator has a settings password. Update its firmware to change ' \
                                  'settings from the GX.'
                if st != 200:
                    return False, 'Unlock refused (HTTP %d)' % st
            try:
                st, _, b = self._send(http_, path, query, body, control)
            finally:
                if not was_armed:
                    http_.request('/armSettings', body=b'arm=0', timeout=5)
        except Exception as e:
            return False, 'No answer from the regulator (%s)' % fetch_error_text(e)
        return self._answer(st, b)

    # -- main tick (every 500 ms on the D-Bus thread) --

    def tick(self, pub):
        now = self.clock()
        frames, fresh, info = self.link.snapshot()
        self._drain(pub, now, info)
        live = 'CSVData' in fresh
        self.live = live
        self.able = self._can_charge(frames, fresh)
        self._dynamic(pub, info)
        self._identity(pub, info, live, frames)
        self._demo(now, frames, info)
        vals = self._live(pub, frames, fresh, live, info)
        self._recorders(pub, now, frames, fresh, live, vals)
        self._wanted(pub, now, frames, fresh, info, live)
        if self.republish:
            self._republish(pub, now)
        self._settings(pub, now, frames, info)
        self._limits(pub, now, info)
        self._flow(pub, now, frames, fresh)
        self._devices(now, info)
        self._cloud(pub, now, frames, info)
        for path in self.reset_paths:
            pub[path] = None
        self.reset_paths.clear()

    def _drain(self, pub, now, info):
        while True:
            try:
                kind, data = self.inbox.get_nowait()
            except queue.Empty:
                return
            if kind == 'result':
                key, ok, message = data
                self.result_seq += 1
                pub['/Settings/Result'] = json.dumps({'seq': self.result_seq, 'key': key, 'ok': ok,
                                                      'message': message}, separators=(',', ':'))
            elif kind in ('accepted', 'unhold'):
                path, value = data
                h = self.hold.get(path)
                if h and h[0] == value:   # a later write to the same path has its own hold
                    if kind == 'unhold':
                        del self.hold[path]
                    elif h[2] is None:
                        h[2] = now
            elif kind == 'limits':
                self._limits_answer(*data)
            elif kind == 'devpost':
                self._devices_answer(*data)
            elif kind == 'demo':
                if self.demo:
                    self._backfill(data)
            elif kind == 'demo_retry':
                self.demo_tries += 1
                if self.demo_gen == data and self.demo_tries < DEMO_TRIES:
                    self.demo_gen = -1
                    self.next['demo'] = now + DEMO_RETRY_S
            elif kind == 'baro':
                # an unset regulator clock: the newest sample is at most one interval older than the fetch
                self.baro, self.baro_end = data[0], data[1] or int(self.wall())
                pub['/History/Baro'] = ','.join('' if x is None else '%.1f' % x for x in self.baro)
                pub['/History/BaroEnd'] = self.baro_end
            elif kind == 'long':
                self.lt, self.lt_at = data, now
                if self.want('Long', now):
                    self._publish_long(pub)
            elif kind == 'health':
                name, text = data
                self.health_csv[name] = text
                if self.want('Health', now):
                    pub['/Health/' + name] = text
            elif kind == 'trend':
                hours, text = data
                self.health_trend[hours] = text
                if self.trend_wanted(hours, now):
                    pub['/Health/TrendCsv/%d' % hours] = text
            elif kind == 'curve':
                self.curve_csv = data
                if self.want('Perf', now):
                    pub['/Perf/CurveCsv'] = data
            elif kind == 'source':
                name, text, parsed = data
                self.sources[name] = (parsed, text, now)
                if self.want('Settings', now):
                    pub['/Settings/Http/' + name] = text
            elif kind == 'cloud':
                self._cloud_answer(now, *data)
            elif kind == 'sources_now':
                for name in self.model.endpoints:
                    self.next['src:' + name] = 0.0

    def _identity(self, pub, info, live, frames):
        ident = info['ident']
        pub['/Connected'] = 1 if live else 0
        if ident:
            if ident.get('name') != self.last_name:
                self.last_name = ident.get('name')
                pub['/CustomName'] = self.last_name or ''
            pub['/FirmwareVersion'] = ident.get('fw', '')
            pub['/Link/Firmware'] = ident.get('fw', '')
        if info['host'] and info['host'] != self.host:
            self.host = info['host']
            self.store.set('host', self.host)
            log.info('%s now at %s', label(self.uid, self.last_name), self.host)
        pub['/Link/Host'] = self.host
        pub['/Mgmt/Connection'] = 'WiFi ' + self.host if self.host else ''
        other = info['other'][1] if info['other'] else {}
        pub['/Link/OtherUid'] = norm_uid(other.get('uid'))
        pub['/Link/OtherName'] = str(other.get('name') or '')
        pub['/Link/Channel'] = info['channel'] if info['state'] == 'streaming' else ''
        self.status = self._status(info, live, frames)
        pub['/Link/Status'] = self.status

    def _status(self, info, live, frames):
        me = label(self.uid, self.last_name)
        if info['state'] == 'missing':
            if info['other']:
                host, other = info['other']
                return '%s not found. %s answers at %s' % (me, label(other.get('uid'), other.get('name')), host)
            return '%s not found: %s' % (me, info['reason'])
        if info['mismatch']:
            ev, got, want = info['mismatch']
            return 'Interface Out of Date: the regulator sends %d %s fields, its web app lists %d' % (got, ev, want)
        if live:
            return 'Connected'
        if info['ever_live']:
            ign = (frames.get('CSVData') or {}).get('Ignition')
            return 'Standby (ignition off)' if ign == 0 else 'No data from the regulator'
        if info['state'] == 'unreachable':
            return 'No answer from %s (%s)' % (info['host'], info['reason'])
        return 'Looking for %s' % me

    # -- demo mode --

    def _demo(self, now, frames, info):
        # Demo mode (Hardware Present? = No): CSV3 hardwarePresent 0 once this connection has sent CSV3, /identify
        # "demo" before that. The regulator swaps a simulated boat's stores in and out, so the bridge drops its own
        # history at each switch; in demo /demoBackfill fills the rings again, once per entry and after each reconnect.
        c3 = frames.get('CSVData3') or {}
        if info['csv3_current'] and c3.get('hardwarePresent') is not None:
            demo = c3['hardwarePresent'] == 0
        else:
            demo = info['ident'].get('demo') == 1
        if demo != self.demo:
            self.demo = demo
            self.demo_gen, self.demo_tries = -1, 0   # a later entry on the same stream needs its own backfill
            log.info('%s %s demo mode', label(self.uid, self.last_name), 'in' if demo else 'out of')
            self._drop_history()
            self.switch_frames = dict(frames)
        if demo and info['state'] == 'streaming' and info['generation'] != self.demo_gen and now >= self.next['demo']:
            if self.demo_gen >= 0:
                self.demo_tries = 0   # a reconnect: a fresh set of tries
            self.demo_gen = info['generation']
            self._fetch('demo', '/demoBackfill', parse_backfill, failed=('demo_retry', self.demo_gen))

    def _drop_history(self):
        self.short, self.wind_plot = ShortRecorder(), WindRecorder()
        for q in self.hist.values():
            q.clear()
        for name, _, _ in WIND_HIST:
            self.wind[name].clear()
            self.wind_acc[name] = []
        self.baro, self.baro_end, self.lt, self.lt_at, self.curve_csv, self.sources = [], None, None, None, '', {}
        self.health_csv = dict.fromkeys(self.health_csv, '')
        self.health_trend = dict.fromkeys(HEALTH_WINDOWS, '')
        for k in ('baro', 'long', 'health', 'curve') + tuple('trend%d' % h for h in HEALTH_WINDOWS):
            self.next[k] = 0.0
        for name in self.model.endpoints:
            self.next['src:' + name] = 0.0
        self.republish = True

    def _backfill(self, data):
        short, wind = data['short'], data['wind']
        if short:
            self.short.fill(short)
            fields = {name: field for name, _ev, field, _s, _c in LIVE}
            picked = short[::-1][::2][:HIST_LEN][::-1]   # one row per 2 s, ending at the newest
            for name, _ in HISTORY:
                q = self.hist[name]
                q.clear()
                for r in picked:
                    v = r.get(fields[name])
                    q.append(v if v is not None else (q[-1] if q else 0.0))
        if wind:
            self.wind_plot.fill(wind)
            rows = wind[-3 * WIND_LEN:]
            rows = rows[len(rows) % 3:]   # whole 30 s means, the last ending at the newest row
            for name, _, field in WIND_HIST:
                q = self.wind[name]
                q.clear()
                for i in range(0, len(rows), 3):
                    v = [r[field] for r in rows[i:i + 3] if r.get(field) is not None]
                    q.append(sum(v) / len(v) if v else None)
        self.republish = True

    def _republish(self, pub, now):
        self.republish = False
        for name, decimals in HISTORY:
            pub['/History/' + name] = ','.join('%.*f' % (decimals, x) for x in self.hist[name])
        self._publish_wind(pub)
        pub['/History/Baro'] = ','.join('' if x is None else '%.1f' % x for x in self.baro)
        pub['/History/BaroEnd'] = self.baro_end
        if self.want('Short', now):
            for name, text in self.short.strings().items():
                pub['/Plot/Short/' + name] = text
        if self.want('Wind', now):
            for name, text in self.wind_plot.strings().items():
                pub['/Plot/Wind/' + name] = text
        if self.want('Long', now):
            self._publish_long(pub)
        if self.want('Health', now):
            for name, text in self.health_csv.items():
                pub['/Health/' + name] = text
        for hours in HEALTH_WINDOWS:
            if self.trend_wanted(hours, now):
                pub['/Health/TrendCsv/%d' % hours] = self.health_trend[hours]
        if self.want('Perf', now):
            pub['/Perf/CurveCsv'] = self.curve_csv

    def _live(self, pub, frames, fresh, live, info):
        vals = {}
        # the wind plots are cleared at a demo switch, so the header's wind waits for the new boat's first CSV4 too
        wind_ok = self._since_switch(frames, fresh, 'CSVData4')
        stale = self._wind_stale(frames, fresh)
        for name, ev, field, scale, conv in LIVE:
            raw = (frames.get(ev) or {}).get(field)
            if ev not in fresh or raw is None or raw <= EMPTY or (name in LIVE_WIND and not wind_ok) or field in stale:
                vals[name] = None
                continue
            v = raw / scale
            vals[name] = round(conv(v) if conv else v, 3)
        if not vals['Health']:
            vals['Health'] = None
        vals['OnOff'] = self._onoff(frames, fresh, vals['OnOff'])
        # Gauge full scale: the largest current cap in the RPM table
        csv3 = frames.get('CSVData3') or {}
        caps = [csv3.get('rpmCapCurrentTable%d' % i) for i in range(10)]
        caps = [c / 100.0 for c in caps if c is not None and c > 0]
        for name, v in vals.items():
            pub['/Live/' + name] = v
        pub['/Live/AltCapMax'] = max(caps) if caps else None
        if not live:
            for i in range(3):
                pub['/GenericInput/%d/Value' % i] = None
            return vals
        on = 1 if vals['OnOff'] else 0
        self._set(pub, '/SwitchableOutput/0/State', on, info)
        pub['/SwitchableOutput/0/Status'] = 0x09 if on else 0x00
        pub['/GenericInput/0/Value'] = None if vals['AltAmps'] is None else round(vals['AltAmps'], 1)
        pub['/GenericInput/1/Value'] = None if vals['BattV'] is None else round(vals['BattV'], 2)
        pub['/GenericInput/2/Value'] = None if vals['Field'] is None else round(vals['Field'])
        return vals

    @staticmethod
    def _onoff(frames, fresh, v1):
        # OnOff rides CSV3 as well, which the regulator sends the moment a write changes a setting; whichever frame the
        # regulator built last, by its own clock (sendMs, same boot), carries the current state
        c1, c3 = frames.get('CSVData') or {}, frames.get('CSVData3') or {}
        on3, s1, s3 = c3.get('OnOff'), c1.get('sendMs'), c3.get('sendMs')
        if v1 is None or 'CSVData3' not in fresh or None in (on3, s1, s3) or on3 <= EMPTY:
            return v1
        if c1.get('sessionId') != c3.get('sessionId') or not 0 < (int(s3) - int(s1)) % 2 ** 32 < 2 ** 31:
            return v1
        return float(on3)

    def _set(self, pub, path, value, info):
        held = self._held(path, value, self.clock(), info)
        pub[path] = value if held is None else held

    def _shown(self, path, echo, now, info, connected):
        if not connected:   # nothing to confirm a write against; the regulator's state is unknown
            self.hold.pop(path, None)
            return echo
        held = self._held(path, echo, now, info)
        return echo if held is None else held

    def _held(self, path, echo, now, info):
        # The value written to path while its write waits for the regulator's echo (see WRITE_WAIT_S), else None
        h = self.hold.get(path)
        if not h:
            return None
        value, wrote_at, accepted_at = h
        if self._echoes(path, value, echo):
            del self.hold[path]
            return None
        if accepted_at is None:
            if now - wrote_at < WRITE_WAIT_S:
                return value
        elif now - accepted_at < self._confirm_s(path, info) or self._echo_at(path, info) <= accepted_at:
            return value
        del self.hold[path]
        return None

    def _echoes(self, path, value, echo):
        if path.startswith('/Settings/Table/'):
            try:
                want, got = json.loads(value), json.loads(echo) if echo else {}
            except ValueError:
                return False
            return all(_same(v, got.get(k)) for k, v in want.items())
        return _same(value, echo)

    def _confirm_s(self, path, info):
        # OnOff rides CSV1 at the GX schedule's interval (and CSV3 once a write makes it dirty); a setting's echo is sent
        # at once, so its channel's own (slow) interval does not apply
        if path == '/SwitchableOutput/0/State':
            return float((info['rate'] or GX_NAP_RATE)['csv1']) / 1000.0 + CONFIRM_MARGIN_S
        return CONFIRM_MARGIN_S

    def _echo_at(self, path, info):
        # when the channel carrying path's echo last delivered, on the bridge's clock
        at = info.get('frame_at') or {}
        if path == '/SwitchableOutput/0/State':
            return max(at.get('CSVData', -1e18), at.get('CSVData3', -1e18))
        item = self.model.items.get(path[16:]) if path.startswith('/Settings/Value/') else None
        echo = (item or {}).get('echo') or {}
        if echo.get('stream') == 'HTTP':
            return self.sources.get(echo.get('endpoint'), (None, None, -1e18))[2]
        return at.get(ECHO_EVENT.get(echo.get('stream'), 'CSVData3'), -1e18)

    def _wind_stale(self, frames, fresh):
        raw = (frames.get('CSVData4') or {}).get('ApparentWindSpeedNMEA')
        gen = self.link.generation
        if raw is not None and raw <= EMPTY:
            self.wind_marker_gen = gen
        return wind_stale(frames, fresh, self.wind_marker_gen == gen)

    def _since_switch(self, frames, fresh, ev):
        # A frame from before the last demo switch is still fresh for a while (CSV4 comes every 1-10 s) but belongs to
        # the other boat: at the exit the demo's wind made one point after the plots were cleared
        f = frames.get(ev)
        if f is None or ev not in fresh or self.switch_frames.get(ev) is f:
            return False
        self.switch_frames.pop(ev, None)
        return True

    def _recorders(self, pub, now, frames, fresh, live, vals):
        wind_ok = self._since_switch(frames, fresh, 'CSVData4')
        if now >= self.next['short']:
            self.next['short'] = max(self.next['short'] + 1.0, now + 0.5) if self.next['short'] else now + 1.0
            means = self.link.take_short()
            stage = (frames.get('CSVData4') or {}).get('chargeStage') if 'CSVData4' in fresh else None
            if 'CSVData' in fresh and (frames['CSVData'].get('shutdownPhase') or 0) > 0:
                stage = 9   # the app's ribbon code for a shutdown in progress
            self.short.close(means, live, stage)
        if wind_ok:
            self.wind_plot.sample(frames['CSVData4'], self._wind_stale(frames, fresh))
        if now >= self.next['wind_plot']:
            first = not self.next['wind_plot']
            self.next['wind_plot'] = now + WIND_PLOT_S
            if not first:
                self.wind_plot.close()
                if self.want('Wind', now):
                    for name, s in self.wind_plot.strings().items():
                        pub['/Plot/Wind/' + name] = s
        if not live:
            return
        if now >= self.next['hist']:
            self.next['hist'] = now + HIST_EVERY_S
            for name, decimals in HISTORY:
                q = self.hist[name]
                v = vals[name]
                q.append(v if v is not None else (q[-1] if q else 0.0))
                pub['/History/' + name] = ','.join('%.*f' % (decimals, x) for x in q)
        for name, live_name, _ in WIND_HIST:
            if vals[live_name] is not None and wind_ok:
                self.wind_acc[name].append(vals[live_name])
        if now >= self.next['wind']:
            self.next['wind'] = now + WIND_EVERY_S
            for name, _, _ in WIND_HIST:
                acc = self.wind_acc[name]
                self.wind[name].append(sum(acc) / len(acc) if acc else None)
                self.wind_acc[name] = []
            self._publish_wind(pub)
            pub['/History/Baro'] = ','.join('' if x is None else '%.1f' % x for x in self.baro)
            pub['/History/BaroEnd'] = self.baro_end

    def _publish_wind(self, pub):
        for name, _, _ in WIND_HIST:
            pub['/History/' + name] = ','.join('' if x is None else '%.1f' % x for x in self.wind[name])

    def _fetch(self, kind, path, parse, timeout=20.0, failed=None):
        # failed: an inbox message to post when the fetch or its parse fails
        http_ = self.link.http

        def job():
            try:
                self.inbox.put((kind, parse(http_.get_ok(path, timeout=timeout))))
            except Exception as e:
                log.warning('%s: %s', path, fetch_error_text(e))
                if failed:
                    self.inbox.put(failed)
        self.fetcher.put(path, job, dedupe=True)

    def _wanted(self, pub, now, frames, fresh, info, live):
        connected = info['state'] == 'streaming'
        on = {t: self.want(t, now) for t in WANT_TOPICS}
        rose = {t: on[t] and not self.want_on[t] for t in WANT_TOPICS}
        fell = {t: self.want_on[t] and not on[t] for t in WANT_TOPICS}
        self.want_on.update(on)
        text = lambda b: gunzip_maybe(b).decode('utf-8', 'replace')

        if connected and now >= self.next['baro']:
            self.next['baro'] = now + BARO_FETCH_S
            self._fetch('baro', '/baroHistory.bin', parse_baro)

        # Short term
        if on['Short'] and (rose['Short'] or now >= self.next['short_pub']):
            self.next['short_pub'] = now + SHORT_PUB_S
            for name, s in self.short.strings().items():
                pub['/Plot/Short/' + name] = s
        if fell['Short']:
            for name, *_ in SHORT:
                pub['/Plot/Short/' + name] = ''
            pub['/Plot/Short/chargeStage'] = ''

        # Long term
        pub['/Plot/Long/Range'] = self.long_range
        if on['Long']:
            fresh_pick = rose['Long'] or self.long_dirty
            self.long_dirty = False
            if fresh_pick and self.lt is not None:
                self._publish_long(pub)
            stale = self.lt_at is None or now - self.lt_at > (LONG_REUSE_S if fresh_pick else LONG_REFETCH_S)
            if fresh_pick:
                self.next['long'] = 0.0
            if connected and stale and now >= self.next['long']:
                self.next['long'] = now + 30   # retry spacing while a fetch fails
                wall = self.wall
                self._fetch('long', '/longTermPlots.bin', lambda b: parse_longterm(b, wall), timeout=90)
        if fell['Long']:
            pub['/Plot/Long/Fields'] = ''
            pub['/Plot/Long/StartEpoch'] = None
            pub['/Plot/Long/StepS'] = None
            pub['/Plot/Long/Last'] = ''
            for name in self._long_series_names():
                pub['/Plot/Long/' + name] = ''

        # Health: live fields always; the session file while wanted, and each window's trend while a screen shows it
        hl = frames.get('AltLive') or {}
        alt_fresh = 'AltLive' in fresh
        for name in info['schema'].get('AltLive', []):
            v = hl.get(name) if alt_fresh else None
            pub['/Health/Live/' + name] = None if v is None else round(v, 3)
        if on['Health']:
            # Session rows are placed by arrival time, so a cached copy would sit early by its age; wait for fresh
            if connected and (rose['Health'] or now >= self.next['health']):
                self.next['health'] = now + HEALTH_REFETCH_S
                self._fetch('health', '/altsess.csv', lambda b: ('SessionCsv', text(b)))
        if fell['Health']:
            pub['/Health/SessionCsv'] = ''
        for hours in HEALTH_WINDOWS:
            path, was = '/Health/TrendCsv/%d' % hours, self.trend_on[hours]
            self.trend_on[hours] = is_on = self.trend_wanted(hours, now)
            if is_on and not was:
                pub[path] = self.health_trend[hours]
            if is_on and connected and (not was or now >= self.next['trend%d' % hours]):
                self.next['trend%d' % hours] = now + HEALTH_REFETCH_S
                self._fetch('trend', '/alttrend.csv' + ('?hours=%d' % hours if hours else ''),
                            lambda b, h=hours: (h, text(b)))
            if was and not is_on:
                pub[path] = ''

        # Speed performance
        pub['/Perf/Schema'] = info['perf_schema_text']
        for ev, prefix in (('PerfLive', '/Perf/Live/'), ('MotorLive', '/Perf/Motor/')):
            f = frames.get(ev) or {}
            ok = on['Perf'] and ev in fresh
            if on['Perf'] or fell['Perf']:
                for name in info['schema'].get(ev, []):
                    v = f.get(name) if ok else None
                    pub[prefix + name] = None if v is None else round(v, 3)
        # The plugin's curve evaluator needs these two settings; they change only when the owner edits them, so an old
        # frame still holds them
        ps = frames.get('PerfSettings') or {}
        for name, path in (('perfRidgeFrac', '/Perf/RidgeFrac'), ('perfIdwPower', '/Perf/IdwPower')):
            v = ps.get(name)
            pub[path] = round(v, 4) if v is not None and math.isfinite(v) else None
        if on['Perf']:
            if rose['Perf']:
                pub['/Perf/CurveCsv'] = self.curve_csv
            if connected and (rose['Perf'] or now >= self.next['curve']):
                self.next['curve'] = now + CURVE_REFETCH_S
                self._fetch('curve', '/perfcurve.csv', text)
        if fell['Perf']:
            pub['/Perf/CurveCsv'] = ''

        # Wind plots publish on their 10 s close; a page opening gets the buffer at once
        if rose['Wind']:
            for name, s in self.wind_plot.strings().items():
                pub['/Plot/Wind/' + name] = s
        if fell['Wind']:
            for name, *_ in WIND_PLOT:
                pub['/Plot/Wind/' + name] = ''

        # Settings sources: the JSON endpoints a settings page shows or echoes from, polled at the app's own rate
        for name, (path, poll) in self.model.endpoints.items():
            cached = self.sources.get(name)
            if rose['Settings']:
                pub['/Settings/Http/' + name] = cached[1] if cached else ''
            elif fell['Settings']:
                pub['/Settings/Http/' + name] = ''
            if on['Settings'] and connected and (rose['Settings'] or now >= self.next['src:' + name]):
                self.next['src:' + name] = now + poll
                self._fetch('source', path, lambda b, name=name: _source_payload(name, b), timeout=6)

        # Fast GX schedule while a page shows live detail
        c4 = (frames.get('CSVData4') or {}) if 'CSVData4' in fresh else {}
        moving = any(c4.get(f) is not None and c4[f] > EMPTY and c4[f] / 100.0 >= PERF_FAST_KN
                     for f in ('SOGNMEA', 'STWNMEA'))
        fast = any(on[t] for t in FAST_TOPICS if t != 'Perf' or moving) and connected and info['channel'] == 'gxevents'
        if info['generation'] != self.last_generation:
            self.last_generation = info['generation']
            self.fast_next, self.fast_held = 0.0, False
            self.flow_next, self.flow_held = 0.0, False
        if fast and now >= self.fast_next:
            self.fast_next = now + FAST_EVERY_S
            self.fast_held = True
            self._gxrate('fast', 1)
        elif not fast and self.fast_held:
            self.fast_held = False
            self.fast_next = 0.0
            if connected:
                self._gxrate('fast', 0)
        # The power-flow tile holds only CSV4 at 2 s (firmware without that hold ignores flow=)
        flow = on['Flow'] and connected and info['channel'] == 'gxevents'
        if flow and now >= self.flow_next:
            self.flow_next = now + FLOW_HOLD_EVERY_S
            self.flow_held = True
            self._gxrate('flow', 1)
        elif not flow and self.flow_held:
            self.flow_held = False
            self.flow_next = 0.0
            if connected:
                self._gxrate('flow', 0)

    def _gxrate(self, hold, value):
        http_ = self.link.http
        q = [(hold, str(value)), ('uid', self.uid)]

        def job():
            try:
                http_.request('/gxrate', query=q, timeout=5)
            except Exception as e:
                log.warning('/gxrate: %s', fetch_error_text(e))
        self.writer.put('gxrate_' + hold, job, dedupe=True)

    # -- home page power flow --

    def wants_gx(self, now):
        # The GxReader cadence this regulator needs, s (0: none): the tile while it shows, else /gxDevices on GX link 4.
        # Never in demo (FakeGx instead).
        if self.demo or not self.live:
            return 0
        if self.want('Flow', now):
            return GX_POLL_TILE_S
        info = self.link.snapshot()[2]
        return GX_POLL_POST_S if gx_version(info['ident']) >= 4 and info['generation'] != self.devpost_hold else 0

    def set_gx_side(self, snapshot, uids=(), memory=None, peers=None):
        self.gx_side, self.gx_uids = snapshot, list(uids)
        self.gx_memory = memory if memory is not None else self.gx_memory
        self.gx_peers = peers if peers is not None else self.gx_peers

    def alt_reading(self):
        # This regulator's alternator amps for another pinned regulator's power flow (CSV4 flowAltA: None when it does
        # not measure them), and whether it is streaming
        frames, fresh, _ = self.link.snapshot()
        raw = (frames.get('CSVData4') or {}).get('flowAltA') if 'CSVData4' in fresh else None
        return (None if raw is None or not math.isfinite(raw) or raw <= EMPTY else raw / 100.0), self.live

    def _flow(self, pub, now, frames, fresh):
        c4 = frames.get('CSVData4') or {}
        streaming = self.live and 'CSVData4' in fresh
        pub['/Flow/Ok'] = (1 if 'flowBattA' in c4 else 0) if streaming else None
        if not streaming or 'flowBattA' not in c4:
            pub['/Flow/Json'] = ''
            self.flow_state, self.flow_flip_at, self.flow_devs, self.flow_imb = None, None, None, None
            return

        def amps(f, ev='CSVData4', scale=100.0):
            raw = (frames.get(ev) or {}).get(f) if ev == 'CSVData4' or ev in fresh else None
            return None if raw is None or not math.isfinite(raw) or raw <= EMPTY else raw / scale

        reg = {'batt': amps('flowBattA'), 'alt': amps('flowAltA'), 'solar': amps('flowSolarA'),
               'src': c4.get('flowSrc'), 'ser': c4.get('flowSolarSer') or 0}   # firmware before flowSolarSer: none
        if self.demo:
            # the made-up GX of a boat whose regulator measures only its alternator: the GX fills the rest
            if self.fake_gx is None:
                self.fake_gx = FakeGx(hash((self.uid, int(self.wall()))) & 0xFFFFFFFF)
            sim = dict(reg, shore=amps('demoShoreA'), fridge=amps('demoFridgeA'), orion=amps('demoOrionA'),
                       inverter=amps('demoInverterA'))
            gx = self.fake_gx.step(now, sim, heading=amps('HeadingNMEA', scale=1.0))
            reg = {'alt': reg['alt'], 'batt': None, 'solar': None, 'src': 3, 'ser': 0}
            devs = gx_devices(gx, None, self.uid, self.gx_uids)
        else:
            self.fake_gx = None
            gx = self.gx_side if self.gx_side and now - self.gx_side.get('at', -1e18) <= GX_FRESH_S else None
            devs = gx_devices(gx, self.gx_memory, self.uid, self.gx_uids, self.gx_peers)
        self.flow_devs = (devs, gx) if gx is not None else None
        m = flow_model(reg, gx, devs, self.uid, self.gx_uids, None if self.demo else self.gx_memory)
        # An imbalance shows only once it has held FLOW_IMBALANCE_S: a step in one input reaches the bridge up to one
        # update before the matching step in another (bench round 8: a fridge start, a charger unplug), and that skew
        # passes within the hold. Meanwhile House reads what the meter, or the clamped sum, gives.
        imb = m['why'] if m['why'] in ('unmeasured', 'gap') else ''
        if not imb:
            self.flow_imb = None
        elif self.flow_imb is None or self.flow_imb[0] != imb:
            self.flow_imb = (imb, now)
        if imb and now - self.flow_imb[1] < FLOW_IMBALANCE_S:
            m = dict(m, why='', x=0.0, house=m['house'] if m['hm'] else 0.0)
        batt = m['batt']
        # the Dashboard's Batt and Alt chips show these same battery and alternator readings, so a chip and the tile never
        # disagree on one screen (bench round 8: the chips from CSV1, every 4-6 s, against CSV4 every 2 s, 30 A apart
        # after a step)
        pub['/Live/BattA'] = _r2(batt)
        pub['/Live/AltAmps'] = _r2(m['rows'][0]['a'])
        if batt is None:
            self.flow_state, self.flow_flip_at = None, None
        else:
            st = 1 if batt > FLOW_FLIP_A else -1 if batt < -FLOW_FLIP_A else 0
            if self.flow_state is None or st == self.flow_state:
                self.flow_state, self.flow_flip_at = st, None
            elif self.flow_flip_at is None:
                self.flow_flip_at = now
            elif now - self.flow_flip_at >= FLOW_FLIP_S:
                self.flow_state, self.flow_flip_at = st, None
        if not self.want('Flow', now):
            pub['/Flow/Json'] = ''
            return
        # the limit holds the regulator's own battery reading; a GX monitor's is a different measurement
        lim = self._flow_limit(frames, fresh) if m['batt_from'] not in ('', 'GX') else None
        out = {'b': {'a': _r2(batt), 'st': self.flow_state, 'from': m['batt_from'], 'lim': lim},
               'rows': [dict(r, a=_r2(r['a'])) for r in m['rows']], 'house': _r2(m['house']), 'hm': m['hm'],
               'why': m['why'], 'x': _r2(m['x']), 'fs': self._charge_current_limit(frames)}
        pub['/Flow/Json'] = json.dumps(out, separators=(',', ':'))

    # -- the GX's devices to the regulator (/gxDevices), for its web app --

    def _devices(self, now, info):
        # Contract: claude_docs/firmware-internals.md, "Victron GX stream", "GX device readings over WiFi"
        self._devices_pace(now, info)
        if gx_version(info['ident']) < 4 or info['state'] != 'streaming' or info['generation'] == self.devpost_hold \
                or self.flow_devs is None:
            return
        devs, gx = self.flow_devs
        keys = frozenset(d['key'] for d in devs)
        if now < self.next['devpost'] and (keys == self.devpost_keys or now < self.next['devpost_soon']):
            return
        self.devpost_keys = keys
        self.devpost_at = now
        self.next['devpost'] = now + self.devpost_every
        self.next['devpost_soon'] = now + 1.0   # a device arriving or leaving posts at once, at most once a second
        text = json.dumps({'v': 1, 'd': device_post(devs, gx)}, separators=(',', ':'))
        http_, gen = self.link.http, info['generation']
        q = [('gx', '1'), ('uid', self.uid)]

        def job():
            try:
                st, _, body = http_.request('/gxDevices', query=q, body={'j': text}, timeout=5)
            except Exception as e:
                log.warning('/gxDevices: %s', fetch_error_text(e))
                return
            self.inbox.put(('devpost', (gen, st, body)))
        self.writer.put('gxdevices', job, dedupe=True)

    def _devices_pace(self, now, info):
        # GxRate "devPost" (ms): the interval the regulator wants, sent the moment a dashboard connects or the last one
        # leaves; a shorter one posts at once. Firmware that sends none: each post's answer (everyS) alone, so a page
        # that opens waits up to one long interval.
        rate = info.get('rate') if info['state'] == 'streaming' else None
        try:
            every = min(120.0, max(2.0, float(rate['devPost']) / 1000.0))
        except (KeyError, TypeError, ValueError):
            self.devpost_rate = None
            return
        if every == self.devpost_rate:
            return
        self.devpost_rate = every
        self.next['devpost'] = now if every < self.devpost_every else self.devpost_at + every
        self.devpost_every = every

    def _devices_answer(self, gen, st, body):
        # Only the JSON answer proves the route: older firmware in AP mode answers any path 200 with a page
        try:
            j = json.loads(body) if st == 200 else None
        except ValueError:
            j = None
        if isinstance(j, dict) and j.get('ok') == 1:
            if self.devpost_rate is not None:
                return   # the stream paces the posts; this answer may predate its latest devPost
            try:
                self.devpost_every = min(120.0, max(2.0, float(j.get('everyS'))))
            except (TypeError, ValueError):
                return
            # a phone just started watching: the shorter interval counts from this post, not the next
            self.next['devpost'] = min(self.next['devpost'], self.devpost_at + self.devpost_every)
            return
        self.devpost_hold = gen
        log.warning('%s: /gxDevices refused (HTTP %d %s); not posting until the link reconnects',
                    label(self.uid, self.last_name), st, (body or b'')[:60].decode('utf-8', 'replace').strip())
        if st == 409:
            self.link.relocate()

    @staticmethod
    def _flow_limit(frames, fresh):
        # The battery-current ceiling holding the battery block, when one binds on the battery's own basis: the
        # regulator's Battery Charge Current Limit (limiter 4), or a Victron limit over VE.Can (limiter 8) that is the
        # lower fresh one. A WiFi limit is the alternator's share (dvcc.py already took solar off), so no line for it.
        c2, c3, c4 = (frames.get(e) or {} for e in ('CSVData2', 'CSVData3', 'CSVData4'))
        if c3.get('BatteryShuntPresent') != 1:
            return None
        limiter = c4.get('ctrlLimiter')
        if limiter == 4 and c3.get('BattLimitEnable') == 1 and (c3.get('BattCurrentLimitA') or 0) > 0:
            return round(c3['BattCurrentLimitA'] / 10.0, 1)
        if limiter != 8 or 'TimestampData' not in fresh or 'CSVData2' not in fresh:
            return None
        ts = frames.get('TimestampData') or {}

        def heard(age_f, ccl_f):
            age, ccl = ts.get(age_f), c2.get(ccl_f)
            ok = age is not None and 0 <= age <= DVCC_HEARD_MS and ccl is not None and ccl > EMPTY and ccl >= 0
            return ccl / 10.0 if ok else None
        can, wifi = heard('ts_DvccVeCan', 'dvccRxCanCcl'), heard('ts_DvccWifi', 'dvccRxWifiCcl')
        return round(can, 1) if can is not None and (wifi is None or can <= wifi) else None

    # -- Cloud page --

    def _cloud(self, pub, now, frames, info):
        want = self.want('Cloud', now)
        c3 = frames.get('CSVData3') or {}
        if info['state'] != 'streaming':
            self._cloud_set(CLOUD_NO_LINK)
        elif c3.get('CloudFeatures') == 0:
            # the app's gate: no cloud contact at all, and nothing from an earlier fetch stays on show
            self._cloud_set(CLOUD_OFF)
            self.cloud_text, self.cloud_as_of, self.cloud_at, self.cloud_demo = '', None, None, False
        elif self.demo:
            # regulator demo mode: no cloud, a canned bundle in the function's shape (the app's demoFillCloudPanes)
            if not self.cloud_demo:
                epoch = int(self.wall())
                self.cloud_text = json.dumps(cloud.view(cloud.demo_bundle(epoch), epoch), separators=(',', ':'))
                self.cloud_as_of, self.cloud_at, self.cloud_demo = epoch, None, True
            self._cloud_set(CLOUD_READY)
        elif c3.get('CloudFeatures') != 1:
            self._cloud_set(CLOUD_LOADING)   # the setting not heard yet on this stream
        else:
            if self.cloud_demo:
                self.cloud_text, self.cloud_as_of, self.cloud_demo = '', None, False
                self._cloud_set(CLOUD_LOADING)
            stale = self.cloud_at is None or now - self.cloud_at >= CLOUD_REFRESH_S
            if (want or self.cloud_refresh) and not self.cloud_busy and now >= self.cloud_next \
                    and (stale or self.cloud_refresh):
                self.cloud_busy, self.cloud_refresh = True, False
                http_ = self.link.http
                self.cloud_worker.put('cloud', lambda: self._cloud_job(http_), dedupe=True)
            if self.cloud_busy and not self.cloud_text:
                self._cloud_set(CLOUD_LOADING)
        pub['/Cloud/State'] = self.cloud_state
        pub['/Cloud/Message'] = self.cloud_msg
        pub['/Cloud/AsOf'] = self.cloud_as_of
        pub['/Cloud/Json'] = self.cloud_text if want else ''

    def _cloud_set(self, state, message=None):
        self.cloud_state = state
        self.cloud_msg = CLOUD_TEXT.get(state, '') if message is None else message

    def _cloud_job(self, http_):
        # cloud worker thread: the regulator's token, then one POST; the token lives only in this call
        try:
            st, _, b = http_.request('/getAuthToken', timeout=8)
            j = json.loads(b) if st == 200 else None
        except Exception as e:
            log.warning('/getAuthToken: %s', fetch_error_text(e))
            self.inbox.put(('cloud', (CLOUD_NO_LINK, None, None)))
            return
        if not isinstance(j, dict):
            self.inbox.put(('cloud', (CLOUD_ERROR, 'The regulator did not give its cloud token', None)))
            return
        if not j.get('registered') or not j.get('token'):
            self.inbox.put(('cloud', (CLOUD_UNREGISTERED, None, None)))
            return
        try:
            v = cloud.view(cloud.fetch(str(j['token']), timeout=CLOUD_TIMEOUT_S), int(self.wall()))
        except urllib.error.HTTPError as e:
            code, msg = cloud.http_error_body(e)
            if e.code == 401 and code == 'token_not_found':
                self.inbox.put(('cloud', (CLOUD_UNREGISTERED, None, None)))
            else:
                log.warning('cloud: HTTP %d %s', e.code, msg)
                self.inbox.put(('cloud', (CLOUD_ERROR, 'The cloud could not answer (%s)' % (msg or 'HTTP %d' % e.code),
                                          None)))
            return
        except (urllib.error.URLError, OSError) as e:
            log.warning('cloud: %s', fetch_error_text(e))
            self.inbox.put(('cloud', (CLOUD_OFFLINE, None, None)))
            return
        except ValueError as e:
            log.warning('cloud: %s', e)
            self.inbox.put(('cloud', (CLOUD_ERROR, 'The cloud answered with something unreadable', None)))
            return
        self.inbox.put(('cloud', (CLOUD_READY, None, v)))

    def _cloud_answer(self, now, state, message, v):
        self.cloud_busy = False
        if self.demo or self.cloud_demo:
            return   # demo started while the call was out
        self.cloud_next = now + (CLOUD_REFRESH_S if state == CLOUD_READY else CLOUD_RETRY_S)
        if state == CLOUD_READY:
            self.cloud_text = json.dumps(v, separators=(',', ':'))
            self.cloud_as_of, self.cloud_at = v['asOf'], now
        elif state == CLOUD_UNREGISTERED:
            self.cloud_text, self.cloud_as_of, self.cloud_at = '', None, None
        self._cloud_set(state, message)

    # -- DVCC over WiFi: the alternator service systemcalc controls, and /gxLimits --

    # -- demo battery service --

    def batt_mode(self, info=None):
        info = info or self.link.snapshot()[2]
        return 'shown' if self.demo and self.batt_ok and info['state'] == 'streaming' else None

    def batt_static_paths(self, process_name='xregulator_bridge.py'):
        return [('/Mgmt/ProcessName', process_name, None), ('/Mgmt/ProcessVersion', VERSION, None),
                ('/Mgmt/Connection', 'WiFi', None), ('/DeviceInstance', self.batt_instance, None),
                ('/ProductId', 0xFFFF, None), ('/ProductName', 'XREG-010 battery monitor', None),
                ('/FirmwareVersion', '', None), ('/Serial', self.uid, None), ('/Connected', 0, None),
                ('/Dc/0/Voltage', None, None), ('/Dc/0/Current', None, None), ('/Dc/0/Power', None, None),
                ('/Dc/0/Temperature', None, None), ('/Soc', None, None), ('/TimeToGo', None, None),
                ('/ConsumedAmphours', None, None), ('/Capacity', None, None)]

    def batt_tick(self, pub):
        frames, fresh, info = self.link.snapshot()
        live = 'CSVData' in fresh

        def val(ev, f, scale):
            raw = (frames.get(ev) or {}).get(f)
            return None if ev not in fresh or raw is None or not math.isfinite(raw) or raw <= EMPTY else raw / scale
        v = next((x for x in (val('CSVData', 'IBV', 100), val('CSVData', 'BatteryV', 100)) if x), None)
        a = val('CSVData4', 'flowBattA', 100)
        a = val('CSVData', 'Bcur', 100) if a is None else a
        soc = val('CSVData2', 'SOC_percent', 100)
        t = val('CSVData2', 'battTempActiveF', 10)
        cap = val('CSVData3', 'BatteryCapacity_Ah', 1)
        pub['/Connected'] = int(live)
        pub['/Dc/0/Voltage'] = None if v is None else round(v, 2)
        pub['/Dc/0/Current'] = None if a is None else round(a, 1)
        pub['/Dc/0/Power'] = None if v is None or a is None else round(v * a)
        pub['/Dc/0/Temperature'] = None if t is None else round(F_TO_C(t), 1)
        pub['/Soc'] = None if soc is None else round(soc, 1)
        pub['/Capacity'] = cap
        pub['/ConsumedAmphours'] = None if soc is None or not cap else round(-(100 - soc) / 100 * cap, 1)
        # Victron's monitors give time to go only while discharging, in seconds
        pub['/TimeToGo'] = round(soc / 100 * cap / -a * 3600) if soc is not None and cap and a is not None and a < -0.5 \
            else None

    def alt_mode(self, info=None):
        # None: no alternator service. 'hidden': DVCC paths only, because an NMEA 2000 service already carries this
        # regulator's measurements and device-list entry. 'shown': its own measurements and device-list entry.
        info = info or self.link.snapshot()[2]
        if self.alt_native or info['state'] == 'missing' or gx_version(info['ident']) < 3:
            return None
        # demo mode turns NMEA 2000 off, so a twin still listed has stopped (its page shows dashes until VE.Can drops it):
        # carry the WiFi readings from the moment demo starts
        return 'hidden' if self.alt_twin and not self.demo else 'shown'

    def alt_static_paths(self, mode, process_name='xregulator_bridge.py'):
        # Write mode 'stamp': systemcalc writes it, and every write is stamped (alt_write), equal values included.
        # No /ProductName when hidden: gui-v2 lists only services with a name, so no second entry and no double count.
        p = [('/Mgmt/ProcessName', process_name, None), ('/Mgmt/ProcessVersion', VERSION, None),
             ('/Mgmt/Connection', 'WiFi', None), ('/DeviceInstance', self.alt_instance, None),
             ('/ProductId', ALT_PRODUCT_ID, None), ('/FirmwareVersion', '', None), ('/Serial', self.uid, None),
             ('/Connected', 0, None), ('/Settings/ChargeCurrentLimit', None, None)]
        p += [(path, None, 'stamp') for path in ALT_LINK_PATHS]
        if mode == 'shown':
            p += [('/ProductName', 'Alternator', None), ('/State', None, None), ('/Dc/0/Voltage', None, None),
                  ('/Dc/0/Current', None, None), ('/Dc/0/Power', None, None)]
        return p

    def alt_write(self, path, value):
        self.alt_writes[path] = (value, self.clock())

    def alt_tick(self, pub, mode):
        frames, fresh, info = self.link.snapshot()
        live = 'CSVData' in fresh
        pub['/Connected'] = int(live)
        pub['/FirmwareVersion'] = info['ident'].get('fw', '')
        # dvcc.py splits the current over every controllable alternator by this value, running or not: a regulator
        # that cannot charge would take a running one's share. With no running one beside it, it keeps its capacity,
        # so systemcalc keeps writing and its limit stays in force across an engine start.
        lim = self._charge_current_limit(frames)
        pub['/Settings/ChargeCurrentLimit'] = 0 if lim and self.alt_shared and not self.able else lim
        if mode != 'shown':
            return
        # until a new alternator service is known not to be the twin, its measurements may already count
        c1 = (frames.get('CSVData') or {}) if live and not self.alt_quiet else {}
        c4 = (frames.get('CSVData4') or {}) if 'CSVData4' in fresh else {}
        v = next((c1[f] / 100.0 for f in ('IBV', 'BatteryV') if c1.get(f) is not None and c1[f] > 0), None)
        a = c1.get('MeasuredAmps')
        a = a / 100.0 if a is not None and a > EMPTY else None
        pub['/Dc/0/Voltage'] = None if v is None else round(v, 2)
        pub['/Dc/0/Current'] = None if a is None else round(a, 1)
        pub['/Dc/0/Power'] = None if v is None or a is None else round(v * a)
        # Victron charger states: 0 off, 3 bulk, 4 absorption, 5 float; anything else the regulator does has no match
        status = c1.get('fieldActiveStatus')
        pub['/State'] = None if not live else 0 if status == 0 else \
            {1: 3, 2: 4, 3: 5}.get(c4.get('chargeStage')) if status == 1 else None

    @staticmethod
    def _can_charge(frames, fresh):
        # Field held off for a reason (engine stopped, switched off, a fault, a rest), or no stream: no share. A nonzero
        # reason alone is not enough (some warnings keep the field on), nor is a field at 0 % alone (that is how a
        # followed 0 A limit looks, and zeroing it then would never hand the share back).
        c1 = frames.get('CSVData') or {}
        return 'CSVData' in fresh and not (c1.get('fieldActiveStatus') == 0 and (c1.get('fieldEventReason') or 0) != 0)

    @staticmethod
    def _charge_current_limit(frames):
        # The most this alternator is ever asked for: Command Limit (MaxTableValue), within the largest entry of the
        # Charge Rate Limits table (rpmCapCurrentTable, the live amps column in either cap mode). DVCC caps this
        # alternator's share at it and splits among several alternators in proportion to it.
        c3 = frames.get('CSVData3') or {}
        cmd = c3.get('MaxTableValue')
        caps = [c3.get('rpmCapCurrentTable%d' % i) for i in range(10)]
        caps = [c / 100.0 for c in caps if c is not None and c > 0]
        lim = cmd / 100.0 if cmd is not None and cmd > 0 else None
        if caps:
            lim = min(lim, max(caps)) if lim else max(caps)
        return round(lim, 1) if lim else None

    def _written(self, path, now):
        v, at = self.alt_writes.get(path, (None, None))
        if at is None or now - at >= DVCC_FRESH_S or isinstance(v, bool) or not isinstance(v, (int, float)) \
                or not math.isfinite(v):
            return None
        return float(v)

    def _limits(self, pub, now, info):
        # Contract: claude_docs/firmware-internals.md, "Victron GX stream", "Charge limits over WiFi"
        mode = self.alt_mode(info)
        cvl = self._written('/Link/ChargeVoltage', now) if mode else None
        ccl = self._written('/Link/ChargeCurrent', now) if mode else None
        gx3 = gx_version(info['ident']) >= 3
        can_post = gx3 and info['state'] == 'streaming' and info['generation'] != self.limits_hold
        if can_post and (cvl is not None or ccl is not None):
            if now >= self.next['gxlimits']:
                self.next['gxlimits'] = now + GXLIMITS_EVERY_S
                self._post_limits(cvl, ccl, info['generation'])
                self.limits_sent = True
        elif can_post and self.limits_sent:
            self._post_limits(None, None, info['generation'])   # withdraws both WiFi limits at once
            self.limits_sent = False
            self.next['gxlimits'] = 0.0
        pub['/Link/GxLimits/State'] = (LIMITS_NONE if not gx3 else LIMITS_NATIVE if self.alt_native
                                       else LIMITS_REFUSED if info['generation'] == self.limits_hold
                                       else LIMITS_SENDING if cvl is not None or ccl is not None else LIMITS_IDLE)
        pub['/Link/GxLimits/Cvl'] = cvl
        pub['/Link/GxLimits/Ccl'] = ccl

    def _post_limits(self, cvl, ccl, gen):
        q = [('gx', '1'), ('uid', self.uid)]
        if cvl is not None:
            q.append(('cvl', fmt_num(cvl)))
        if ccl is not None:
            q.append(('ccl', fmt_num(ccl)))
        http_ = self.link.http

        def job():
            try:
                st, _, body = http_.request('/gxLimits', query=q, timeout=5)
            except Exception as e:
                log.warning('/gxLimits: %s', fetch_error_text(e))
                return
            self.inbox.put(('limits', (gen, st, body)))
        # a slow regulator skips a repeat rather than queue them; the empty post is never skipped
        self.writer.put('gxlimits' if len(q) > 2 else 'gxlimits_end', job, dedupe=len(q) > 2)

    def _limits_answer(self, gen, st, body):
        # Only the JSON answer proves the route: older firmware in AP mode answers any path 200 with a page
        try:
            j = json.loads(body) if st == 200 else None
        except ValueError:
            j = None
        if isinstance(j, dict) and j.get('ok') == 1:
            return
        self.limits_hold, self.limits_sent = gen, False
        log.warning('%s: /gxLimits refused (HTTP %d %s); not posting until the link reconnects',
                    label(self.uid, self.last_name), st, (body or b'')[:60].decode('utf-8', 'replace').strip())
        if st == 409:
            self.link.relocate()

    def _publish_long(self, pub):
        b = bin_longterm(self.lt, LONG_RANGES[self.long_range])
        pub['/Plot/Long/Fields'] = ','.join(LT_FIELDS)
        pub['/Plot/Long/StartEpoch'] = b['start']
        pub['/Plot/Long/StepS'] = b['step']
        for name in self._long_series_names():
            pub['/Plot/Long/' + name] = fmt_series(b['series'].get(name, []), lt_decimals(name))
        pub['/Plot/Long/Last'] = json.dumps(longterm_last(self.lt), separators=(',', ':'))

    def _settings(self, pub, now, frames, info):
        connected = info['state'] == 'streaming'
        frames = self._echo_frames(now)
        for key in self.model.values:
            path = '/Settings/Value/' + key
            echo = self.model.display(self.model.items[key], frames) if connected else None
            pub[path] = self._shown(path, echo, now, info, connected)
        for tid in self.model.tables:
            path = '/Settings/Table/' + tid
            echo = self.model.table_json(tid, frames) if connected else ''
            pub[path] = self._shown(path, echo, now, info, connected)


def _same(a, b):
    # a written value against its echo: numbers equal to rounding, anything else exactly
    num = lambda x: isinstance(x, (int, float)) and not isinstance(x, bool)
    if num(a) and num(b):
        return abs(a - b) <= 1e-6 * max(1.0, abs(a))
    return a == b


def _r2(v):
    return None if v is None else round(v, 2)


def _source_payload(name, b):
    text = gunzip_maybe(b).decode('utf-8', 'replace')
    try:
        parsed = json.loads(text)
    except ValueError:
        parsed = None
    return name, text, parsed


def _instance(class_and_instance, default):
    # localsettings ClassAndVrmInstance, e.g. "switch:101"
    try:
        return int(str(class_and_instance).split(':')[1])
    except (IndexError, ValueError):
        return default


# ---- Manager: the regulators this GX stands for ----

class Manager:
    # The pinned regulators, each a Core with its own D-Bus service, and what Discovery finds for the owner to pick from.
    # Adding, removing and re-pinning run on the main tick, never inside a D-Bus callback of a service they remove.
    def __init__(self, store, schema_text, discovery, unit_store, forget_store=None, listener=None,
                 cache_dir=CACHE_DIR, clock=time.monotonic, wall=time.time):
        self.store = store                 # 'units': the pinned uids, comma-separated, in the order added
        self.schema_text = schema_text or ''
        self.discovery = discovery
        self.unit_store = unit_store       # (uid, requested VRM instance) -> that regulator's store
        self.forget_store = forget_store or (lambda uid: None)
        self.listener = listener           # unit_added(core) before it starts, unit_removed(core) after it stops
        self.cache_dir, self.clock, self.wall = cache_dir, clock, wall
        self.units = {}
        self.order = []
        self.pending = []
        self.inbox = queue.Queue()
        self.worker = Worker('manager')    # address probes
        self.probe_seq = 0
        self.reset_paths = {}
        self.alternators = []              # the GX's other alternator services, from set_alternators
        self.batt_gate = False             # the GX can take a demo battery service (set_battery_gate)
        self.gx_memory = {}                # the GX devices seen (GX_CLASS_KIND), store 'gx_devices'
        self.gx_present = set()            # their keys on D-Bus at the last GX reading
        self.gx_meter = False              # a DC system meter among them

    def start(self):
        try:
            mem = json.loads(self.store.get('gx_devices') or '{}')
            self.gx_memory = {str(k): v for k, v in mem.items() if isinstance(v, dict)} if isinstance(mem, dict) else {}
        except ValueError:
            self.gx_memory = {}
        self.discovery.start()
        self.worker.start()
        for uid in dict.fromkeys(norm_uid(u) for u in (self.store.get('units') or '').split(',')):
            if uid:
                self._add(uid)
                self.order.append(uid)
        if self._purge_memory(list(self.units)):
            self._save_memory()
        if not self.order:
            self.discovery.request_sweep()   # nothing picked yet: have a list ready without mDNS too

    def stop(self):
        for core in self.units.values():
            core.stop()
        self.worker.stop()
        self.discovery.stop()

    def static_paths(self, process_name='xregulator_bridge.py'):
        # /Probe stays a valid '' so the address field that writes it stays editable
        return [('/Mgmt/ProcessName', process_name, None), ('/Mgmt/ProcessVersion', VERSION, None),
                ('/Mgmt/Connection', 'WiFi', None), ('/DeviceInstance', 0, None),
                ('/Units', '[]', None), ('/Found', '[]', None), ('/Scanning', 0, None), ('/ProbeResult', '', None),
                ('/Add', None, 'core'), ('/Remove', None, 'core'), ('/Scan', None, 'core'), ('/Probe', '', 'core'),
                ('/GxDevices', '[]', None), ('/GxForget', '', 'core'), ('/GxDrawsHouse', '', 'core'),
                ('/GxMetered', '', 'core')]

    # -- writes from D-Bus (main thread) --

    def on_write(self, path, value):
        uid = norm_uid(value)
        if path == '/Add':
            # a found regulator, or one the address probe reached; for one already pinned it only says where it is now
            e = self.discovery.get(uid) if uid else None
            if not e or not e['host']:
                return False
            if uid in self.units:
                self.units[uid].link.hint(e['host'])
            else:
                self.pending.append(('add', uid, e['host']))
            self.reset_paths[path] = None
            return True
        if path == '/Remove':
            if uid not in self.units:
                return False
            self.pending.append(('remove', uid))
            self.reset_paths[path] = None
            return True
        if path == '/Scan':
            self.discovery.request_sweep(force=True)
            self.reset_paths[path] = None
            return True
        if path == '/GxForget':
            # a remembered GX device the owner says the boat no longer has: its power-flow row goes. One still on D-Bus
            # comes straight back.
            key = str(value or '')
            if key not in self.gx_memory:
                return False
            del self.gx_memory[key]
            self._save_memory()
            self.reset_paths[path] = ''
            return True
        if path == '/GxDrawsHouse':
            # {"key": a remembered Orion (class dcdc; a DC meter in DC-DC mode goes by its sign instead), "on": 1 it
            # draws from the house bank to charge another battery (a Loads row), 0 it feeds the house bank (a source, the
            # default)}
            try:
                req = json.loads(value)
                key, on = str(req['key']), int(req['on'])
            except (ValueError, KeyError, TypeError):
                return False
            m = self.gx_memory.get(key)
            if not isinstance(m, dict) or gx_class(key) != 'dcdc' or on not in (0, 1):
                return False
            if on:
                m['h'] = 1
            else:
                m.pop('h', None)
            self._save_memory()
            self.reset_paths[path] = ''
            return True
        if path == '/GxMetered':
            # {"key": a remembered device that makes a row, or "xreg:<uid>" a pinned regulator's own alternator, "on": 1
            # its current passes through the DC system meter, 0 it does not}: how it is wired, which the GX cannot see
            try:
                req = json.loads(value)
                key, on = str(req['key']), int(req['on'])
            except (ValueError, KeyError, TypeError):
                return False
            m = self.gx_memory.get(key)
            if on not in (0, 1):
                return False
            if key.startswith('xreg:') and key[5:] in self.units:
                self.gx_memory[key] = {'k': 'reg', 'm': on}
            elif isinstance(m, dict) and m.get('k') in FLOW_SOURCE_KINDS + FLOW_LOAD_KINDS \
                    and set(str(m.get('r') or '')) & set('sl'):
                m['m'] = on
            else:
                return False
            self._save_memory()
            self.reset_paths[path] = ''
            return True
        if path == '/Probe':
            host = str(value or '').strip()
            if not HOST_RE.match(host):
                return False
            self.worker.put('probe', lambda: self._probe(host))
            self.reset_paths[path] = ''
            return True
        return False

    def set_alternators(self, services, settled=True):
        # The GX's other alternator services, [{'service', 'serial', 'link': has /Link/ChargeCurrent}]. One carrying a
        # regulator's serial is its NMEA 2000 twin; one that also has /Link/ChargeCurrent is control by someone else
        # (Victron's own support), and ours steps aside so DVCC does not split the current between two copies.
        # settled: every service's serial was read, so a quiet spell from alternator_appeared() can end.
        self.alternators = list(services)
        self._match_alternators()
        if settled:
            for core in self.units.values():
                core.alt_quiet = False

    def set_battery_gate(self, ok):
        # ok: the GX has no battery service, no device that takes a battery reading, and DVCC off (BATT_SERVICE). One
        # demo battery at most: the first pinned regulator in demo gets it.
        self.batt_gate = ok
        self._match_batteries()

    def _match_batteries(self):
        first = next((u for u in self.order if u in self.units and self.units[u].demo), None) if self.batt_gate else None
        for uid, core in self.units.items():
            core.batt_ok = uid == first

    def wants_gx(self):
        # How often xregulator_bridge.py reads the GX's devices, s (0: not at all): a power-flow tile on screen outside
        # demo, else a regulator taking /gxDevices
        now = self.clock()
        every = [e for e in (core.wants_gx(now) for core in self.units.values()) if e]
        return min(every) if every else 0

    def set_gx_side(self, snapshot):
        # The GX's own readings for the power flow and /gxDevices (xregulator_bridge.py GxReader), the same for every
        # regulator; each Core leaves out itself and the twins. A device seen with its identity read is remembered.
        uids = list(self.units)
        peers = []
        for u, c in self.units.items():
            amps, live = c.alt_reading()
            peers.append({'uid': u, 'name': c.last_name or 'XREG-' + uid6(u), 'amps': amps, 'live': live})
        self._remember(snapshot, uids)
        for core in self.units.values():
            core.set_gx_side(snapshot, uids, self.gx_memory, peers)

    def _remember(self, snapshot, uids):
        changed, present = False, set()
        uniques = {n2k_unique(u) for u in uids} - {None}
        # a key the GX now shows to be a pinned regulator's twin is never kept, whatever was stored under it
        for d in (snapshot or {}).get('devices') or []:
            serial = str(d.get('serial') or '').strip()
            if gx_kind(d) == 'altx' and (any(serial_matches(u, serial, uids) for u in uids) if serial
                                         else d.get('n2k') in uniques) \
                    and self.gx_memory.pop(str(d.get('key') or d.get('service') or ''), None) is not None:
                changed = True
        # the memory goes in for the side a DC meter in DC-DC mode was last seen on
        devs = gx_devices(snapshot, self.gx_memory, '', uids)
        for d in devs:
            if not d['on']:
                continue
            present.add(d['key'])
            # once its D-Bus identity has been read (the key from the VRM instance, not the service name), and only
            # what makes a row
            if not (d['product'] or d['name']) or ':' not in d['key'] or not set(d['roles']) & set('slp'):
                continue
            # an Orion keeps its own side; the owner's flag ("h") makes it a load
            roles = gx_roles(d['kind'], d['pv']) if d['cls'] == 'dcdc' else d['roles']
            m = {'k': d['kind'], 'n': d['name'], 'p': d['product'], 'r': roles}
            old = self.gx_memory.get(d['key'])
            if d['serial']:
                m['s'] = d['serial']
            elif d['kind'] == 'altx':
                m['w'] = 1   # listed without a serial after ALT_SERIAL_WAIT_S
                if d['n2k'] is not None:
                    m['u'] = d['n2k']
            if draws_house(d['key'], old):
                m['h'] = 1
            if isinstance(old, dict) and old.get('m') in (0, 1):
                m['m'] = old['m']
            if old != m:
                self.gx_memory[d['key']] = m
                changed = True
        self.gx_present = present
        self.gx_meter = any(d['kind'] == 'dcsystem' and d['on'] for d in devs)
        if self._purge_memory(uids) or changed:
            self._save_memory()

    def _purge_memory(self, uids):
        # Drops what must never be a row: an alternator whose serial, or with none its NMEA 2000 unique number, is a
        # pinned regulator's, and one stored without a serial under the twin's identity (stored_twin). Clears a "draws
        # from the house bank" flag on anything but an Orion. True when something changed.
        changed = False
        uniques = {n2k_unique(u) for u in uids} - {None}
        for key, m in list(self.gx_memory.items()):
            if m.get('k') == 'altx' and (any(serial_matches(u, m.get('s'), uids) for u in uids) or stored_twin(key, m)
                                         or (not m.get('s') and m.get('u') in uniques)):
                del self.gx_memory[key]
                changed = True
            elif 'h' in m and gx_class(key) != 'dcdc':
                del m['h']
                changed = True
        return changed

    def _save_memory(self):
        self.store.set('gx_devices', json.dumps(self.gx_memory, separators=(',', ':'), sort_keys=True))

    def alternator_appeared(self):
        # A new alternator service on the GX could be a regulator's NMEA 2000 twin coming back, which systemcalc counts
        # as soon as it scans it: no measurements from ours until set_alternators() has read its serial
        for core in self.units.values():
            core.alt_quiet = True

    def _match_alternators(self):
        uids = list(self.units)
        for uid, core in self.units.items():
            mine = [x for x in self.alternators if serial_matches(uid, x.get('serial'), uids)]
            core.alt_twin = bool(mine)
            core.alt_native = any(x.get('link') for x in mine)
        # another make's controllable alternator counts as able to charge: its state is not known here
        for uid, core in self.units.items():
            core.alt_shared = any(x.get('link') and not serial_matches(uid, x.get('serial'), uids)
                                  for x in self.alternators) \
                or any(o.able and o.alt_mode() for u, o in self.units.items() if u != uid)

    def _repin(self, old, new, host):
        if old not in self.units or new in self.units:
            return False
        self.pending.append(('repin', old, new, host))
        return True

    def _probe(self, host):
        try:
            st, _, body = Http(host).request('/identify', timeout=5)
            ident = xreg_ident(body) if st == 200 else None
            message = '' if ident else 'Something answered at %s, but not an XREG-010' % host
        except Exception as e:
            ident, message = None, 'No answer at %s (%s)' % (host, fetch_error_text(e))
        if ident is None:
            self.inbox.put({'host': host, 'ok': False, 'message': message})
            return
        uid = norm_uid(ident['uid'])
        self.discovery.note_ident(host, ident, 'manual')
        self.inbox.put({'host': host, 'ok': True, 'uid': uid, 'id': uid6(uid), 'name': str(ident.get('name') or ''),
                        'fw': str(ident.get('fw') or ''), 'message': ''})

    # -- the set of regulators --

    def _add(self, uid, host='', instance=INSTANCE):
        st = self.unit_store(uid, instance)
        if host:
            st.set('host', host)
        core = Core(uid, st, self.schema_text, self.discovery, self.cache_dir, self.clock, self.wall,
                    instance=_instance(st.get('instance'), instance),
                    alt_instance=_instance(st.get('alt_instance'), INSTANCE),
                    batt_instance=_instance(st.get('batt_instance'), INSTANCE), on_repin=self._repin)
        self.units[uid] = core
        if self.listener:
            self.listener.unit_added(core)
        core.start()
        log.info('%s pinned (VRM instance %d)', label(uid), core.instance)
        return core

    def _remove(self, uid):
        core = self.units.pop(uid)
        core.stop()
        if self.listener:
            self.listener.unit_removed(core)
        self.forget_store(uid)
        log.info('%s removed', label(uid, core.last_name))

    def _apply(self):
        ops, self.pending = self.pending, []
        for op in ops:
            if op[0] == 'add' and op[1] not in self.units:
                self._add(op[1], op[2])
                self.order.append(op[1])
            elif op[0] == 'remove' and op[1] in self.units:
                self._remove(op[1])
                self.order.remove(op[1])
            elif op[0] == 'repin' and op[1] in self.units and op[2] not in self.units:
                _, old, new, host = op
                instance = self.units[old].instance
                self._remove(old)
                self._add(new, host, instance)   # takes the old unit's place in the list and its VRM instance
                self.order[self.order.index(old)] = new
            else:
                continue
            self.store.set('units', ','.join(self.order))

    # -- main tick --

    def tick(self, pub):
        self._apply()
        self._match_alternators()
        self._match_batteries()
        while True:
            try:
                r = self.inbox.get_nowait()
            except queue.Empty:
                break
            self.probe_seq += 1
            r.update(seq=self.probe_seq, unit=int(r.get('uid') in self.units))
            pub['/ProbeResult'] = json.dumps(r, separators=(',', ':'))
        for core in self.units.values():
            ident = core.link.snapshot()[2]['ident']
            if core.live and ident:
                self.discovery.note_ident(core.host, ident, 'link')   # a streaming regulator stays on the found list
        pub['/Units'] = json.dumps([self.units[u].summary() for u in self.order], separators=(',', ':'))
        order = {k: i for i, k in enumerate(FLOW_SOURCE_KINDS + FLOW_LOAD_KINDS)}
        devices = []
        # with a DC system meter on the GX: the pinned regulators' own alternators first ("reg"), for their "m" switch,
        # and "m" on every device that makes a row (through_meter)
        if self.gx_meter:
            for u in self.order:
                if u in self.units:
                    key = 'xreg:' + u
                    devices.append({'key': key, 'k': 'alt', 'n': self.units[u].last_name or 'XREG-' + uid6(u),
                                    'p': 'XREG-010', 'on': 1, 'reg': 1,
                                    'm': int(through_meter(key, 's', self.gx_memory.get(key)))})
        for k, m in sorted(self.gx_memory.items(), key=lambda e: (order.get(e[1].get('k'), 99), e[0])):
            if m.get('k') == 'reg':
                continue
            row = {'key': k, 'k': m['k'], 'n': m.get('n', ''), 'p': m.get('p', ''), 'on': int(k in self.gx_present)}
            if gx_class(k) == 'dcdc':
                row['h'] = int(draws_house(k, m))   # the page's switch: Orions only
            roles = 'l' if draws_house(k, m) else str(m.get('r') or '')
            if self.gx_meter and set(roles) & set('sl'):
                row['m'] = int(through_meter(k, roles, m))
            devices.append(row)
        pub['/GxDevices'] = json.dumps(devices, separators=(',', ':'))
        pub['/Found'] = json.dumps(self._found(), separators=(',', ':'))
        pub['/Scanning'] = int(self.discovery.sweeping)
        for path, v in self.reset_paths.items():
            pub[path] = v
        self.reset_paths.clear()

    def _found(self):
        boat = set()
        for uid in self.order:
            e = self.discovery.get(uid)
            boat.update(e['pair'] if e else [])
        rows = [{'uid': e['uid'], 'id': uid6(e['uid']), 'name': e['name'], 'host': e['host'], 'fw': e['fw'],
                 'unit': int(e['uid'] in self.units), 'boat': int(uid6(e['uid']) in boat)}
                for e in self.discovery.snapshot()]
        return sorted(rows, key=lambda r: (-r['boat'], (r['name'] or r['id']).lower(), r['uid']))
