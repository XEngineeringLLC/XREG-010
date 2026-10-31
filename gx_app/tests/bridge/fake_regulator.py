#!/usr/bin/env python3
# Fake XREG-010 for offline bridge tests. Field lists come from the real web_src/script.js, the schema-event names
# from the real firmware source, CSV3 wire scales from the research streams table (fixtures/), so frames have the
# real shape.
# gx is the /identify "gx" version: 4 = 3 plus POST /gxDevices (the GX's device readings for the web app), 3 = the GX
# link plus GET /gxLimits (charge limits over WiFi), 2 = the GX link
# (/gxevents, /gxrate, gx=1 writes need no unlock), 1 = the unshipped pairing build (has /gxevents and /gxrate, but a
# write still needs the unlock), 0 = 0.0.7-0.0.9 (no flag).

import csv, gzip, hashlib, json, math, os, re, struct, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
RESEARCH = os.path.join(HERE, 'fixtures')
UID = '0000FEEDC0DE1234'
PROBE = '28ff001122334455'
OW_ASSIGN = ('owAssignAlt', 'owAssignBatt', 'owAssignExtra')
DENIED = {'commissionStart', 'startSystemID', 'startTuningSweep', 'startKneeSweep'}


def js_list(js, name):
    m = re.search(r'const\s+%s\s*=\s*\[(.*?)\];' % name, js, re.S)
    body = re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S))
    return re.findall(r'"([^"]+)"', body)


with open(REPO + '/web_src/script.js', 'rb') as f:
    SCRIPT = f.read()
# The round 3 CSV4 power-flow group (contract F1); appended here until the firmware lane's script.js carries it
FLOW_FIELDS = ('flowBattA', 'flowAltA', 'flowSolarA', 'flowSrc')
_m = re.search(rb'const\s+CSV4_FIELDS\s*=\s*\[(.*?)\];', SCRIPT, re.S)
if not all(b'"' + f.encode() + b'"' in _m.group(1) for f in FLOW_FIELDS):
    SCRIPT = SCRIPT[:_m.end(1)] + b',\n' + b', '.join(b'"' + f.encode() + b'"' for f in FLOW_FIELDS) + b'\n' \
        + SCRIPT[_m.end(1):]
SCRIPT_GZ = gzip.compress(SCRIPT, 6)
SCRIPT_ETAG = '"%s"' % hashlib.md5(SCRIPT).hexdigest()[:16]
_js = SCRIPT.decode('utf-8', 'replace')
FIELDS = {'CSVData': js_list(_js, 'CSV1_FIELDS'), 'CSVData2': js_list(_js, 'CSV2_FIELDS'),
          'CSVData3': js_list(_js, 'CSV3_FIELDS'), 'CSVData4': js_list(_js, 'CSV4_FIELDS'),
          'TimestampData': js_list(_js, 'TS_FIELDS')}
with open(REPO + '/7_functions.ino') as f:
    _fw = f.read()
ALT_LIVE = re.findall(r'\{"(\w+)", alf_\w+\}', _fw)
PERF_LIVE = re.findall(r'\{"(\w+)", plf_\w+\}', _fw)
MOTOR_LIVE = re.findall(r'\{"(\w+)", pmlf_\w+\}', _fw)
CSV3_SCALE = {}
with open(RESEARCH + '/webapp_streams.csv') as f:
    for row in csv.DictReader(f):
        if row['stream'] == 'CSV3':
            try:
                CSV3_SCALE[row['field']] = float(row['wire_scale'])
            except ValueError:
                CSV3_SCALE[row['field']] = 1.0


SHORT_COLS = ['Bcur', 'MeasuredAmps', 'iiout', 'dutyCycle', 'pidAltPV', 'BatteryV', 'IBV', 'RPM',
              'AlternatorTemperatureF', 'chargeStage', 'protEventMask']
WIND_COLS = ['TrueWindSpeedNMEA', 'TrueWindAngleNMEA', 'ApparentWindSpeedNMEA', 'ApparentWindAngleNMEA']


def make_backfill(now):
    # The firmware's /demoBackfill shape. Short row k: alternator 40 + k/100 A, 13.5 V, 212 F, field 30 %, bulk;
    # wind row k: 8, 9, 10 kt in turn (each 30 s mean 9 kt) at 45 deg, apparent 10 kt at 30 deg.
    e = int(now)
    short = [[20.0, 40 + k / 100.0, 2.5, 30.0, 40.0, 13.55, 13.5, 1500, 212.0, 1, 0] for k in range(300)]
    wind = [[8.0 + k % 3, 45.0, 10.0, 30.0] for k in range(360)]
    return {'v': 1, 'now': e, 'short': {'dt': 1, 't0': e - 299, 'cols': SHORT_COLS, 'rows': short},
            'wind': {'dt': 10, 't0': e - 3590, 'cols': WIND_COLS, 'rows': wind}}


def signed32(v):
    return v - (1 << 32) if v >= 1 << 31 else v


LT_REC = struct.Struct('<IiiI51h6hBB5hBB')


def make_longterm(n=1000, last_epoch=1_790_000_000, interval=600, gap_at=None, unsynced_tail=0, v5=False):
    # Record i: battVolt avg 13.00 + i/1000 V, envelope +-0.1; altTemp 150 F; awa alternates 350/10 deg; stage i%4;
    # one gap (records missing) at gap_at; the newest unsynced_tail records carry epoch 0. v5 adds the battery
    # temperature envelope (bit 24, the firmware's v5 tail): 77 F +-1, none on every fifth record.
    recs = []
    for i in range(n):
        if gap_at and gap_at[0] <= i < gap_at[1]:
            continue
        ts = last_epoch - (n - 1 - i) * interval
        if unsynced_tail and i >= n - unsynced_tail:
            ts = 0
        valid = 0
        env = []
        def put(fi, lo, hi, avg):
            nonlocal valid
            valid |= 1 << fi
            env.extend([lo, hi, avg])
        for fi in range(17):
            if fi == 0:
                a = 1300 + i // 10
                put(0, a - 10, a + 10, a)
            elif fi == 2:
                put(2, 400, 600, 500)              # altCurr 50 A (x10)
            elif fi == 5:
                put(5, 3000, 5000, 4000)           # duty 40 % (x100)
            elif fi == 6:
                put(6, 1400, 1600, 1500)           # altTemp 150 F (x10)
            elif fi == 12:
                v = 3500 if i % 2 else 100         # awa 350 or 10 deg (x10)
                put(12, v, v, v)
            elif fi == 16:
                put(16, 800, 900, 850)             # soc 85 % (x10)
            else:
                env.extend([0, 0, 0])
        avg = [10132, 770, 0, 0, 0, 0]             # baro 1013.2 mbar, ambTemp 77 F
        valid |= (1 << 17) | (1 << 18)
        on_avg = [1310, 0, 480, 0, 4100]
        tail = b''
        if v5:
            if i % 5:
                valid |= 1 << 24
            tail = struct.pack('<3hh', 760, 780, 770, 0) if i % 5 else struct.pack('<4h', 0, 0, 0, 0)
        recs.append(struct.pack('<IiiI', ts, 0, 0, valid) + struct.pack('<51h', *env) + struct.pack('<6h', *avg)
                    + struct.pack('<BB', i % 4, 0) + struct.pack('<5h', *on_avg) + struct.pack('<BB', 100, 0) + tail)
    hdr = struct.pack('<HHHHII', len(recs) % 4320, len(recs), 4320, LT_REC.size + (8 if v5 else 0), last_epoch,
                      interval)
    return hdr + b''.join(recs)


def make_baro(count=432):
    samples = [10000 + i for i in range(count)]
    head = 5
    ring = [0] * count
    for i, v in enumerate(samples):
        ring[(head + i) % count] = v   # the oldest sample sits at slot head
    return struct.pack('<HIH', head, 1_790_000_000, count) + b''.join(struct.pack('<H', v) for v in ring)


class FakeReg:
    def __init__(self, gx=2, password=False, schema_events_on_gx=True, uid=UID, fw='0.0.9', name='Port engine',
                 paired=()):
        self.gx, self.password, self.schema_events_on_gx = gx, password, schema_events_on_gx
        self.wind_demo_only = False   # CSV4 wind empty outside demo (a bench with no wind instrument)
        self.backfill_refuse = 0   # /demoBackfill answers 404 this many more times (the regulator still entering demo)
        self.uid, self.fw, self.name = uid, fw, name
        self.paired = list(paired)   # uids of "Regulators on this boat"
        self.lock = threading.Lock()
        self.log = []
        self.armed = False
        self.on_off = 1
        self.fast = []
        self.flow_holds = []           # /gxrate flow= values received
        self.flow = {'flowBattA': 5000, 'flowAltA': 6800, 'flowSolarA': 2200, 'flowSrc': 11}   # CSV4, A x100
        self.token = 'tok-' + uid[-6:]   # /getAuthToken; None = not registered
        self.token_requests = 0
        self.csv3 = {f: 0 for f in FIELDS['CSVData3']}
        for k, v in (('BulkVoltage', 1420), ('TemperatureLimitF', 200), ('capLimitMode', 0), ('ShuntLead', 1),
                     ('MaxTableValue', 15000), ('hardwarePresent', 1)):
            self.csv3[k] = v
        for i in range(10):
            self.csv3['rpmTableRPMPoints%d' % i] = 500 + 300 * i
            self.csv3['rpmCapCurrentTable%d' % i] = (20 + 10 * i) * 100
            self.csv3['rpmCapPowerTable%d' % i] = (1 + i) * 250
            self.csv3['rpmMinDutyTable%d' % i] = 1000 + i * 5
        self.csv3_dirty = threading.Event()
        self.n_track = {}
        self.delay_onoff_s = 0.0
        # (toggle lane, 2026-10-29) the write echo's timing, all off by default
        self.started = time.time()
        self.clock_fields = False      # CSV1 and CSV3 carry sendMs (ms since start) and sessionId, as the firmware's do
        self.gx_csv1_s = 0.5           # CSV1 interval on /gxevents, also the GxRate "csv1" it announces
        self.live_ms = None            # firmware with /gxrate?fast=2: CSV1 at this interval under it; None reads 2 as 1
        self.base_csv1_s = None        # gx_csv1_s before a fast=2 hold
        self.fast_level = 0            # the GxRate "fast" it announces
        self.session = 4242            # CSV1/CSV3 sessionId (a reboot changes it)
        self.csv3_every_s = 5.0        # the periodic CSV3 (a written setting is echoed at once)
        self.confirm_delay_s = None    # OnOff: the next CSV1 goes this long after a write, none before it
        self.onoff_csv3_echo = False   # with confirm_delay_s: the CSV3 echo of an OnOff write goes at once
        self.csv3_echo_delay_s = 0.0   # a written setting's CSV3 echo goes this long after the write
        self.refuse_onoff = None       # a /get OnOff answer starting "Rejected" (the bridge reports ok:false)
        self.ignore_onoff = False      # answer an OnOff write 200 with an empty body and leave OnOff as it was
        self.on_off_c3 = None          # CSV3's OnOff when it differs from on_off (an echo not sent yet)
        self.confirm = None            # (time, value) of a CSV1 confirmation still to come
        self.extra_csv1_field = False
        self.uid_in_stream = uid
        self.roles = {'owAssignAlt': PROBE, 'owAssignBatt': '', 'owAssignExtra': ''}
        self.unknown_keys = set()      # keys this firmware's /get does not know (answers "No message sent")
        self.gxevents_refuse = False   # /gxevents answers 503 on GX firmware
        self.script_downloads = 0
        self.streams = 0
        self.stream_gen = 0            # bumped by swap(): open streams end, as a TCP stream does when its address moves
        self.field_status, self.off_reason = 1, 0   # CSV1 fieldActiveStatus / fieldEventReason
        self.limits = []               # accepted /gxLimits posts: {'cvl': '14.2', 'ccl': '37.5'}, either may be absent
        self.limits_uid = None         # the uid /gxLimits takes as its own (another unit's: the post is misaddressed)
        self.limits_page = None        # a 200 page /gxLimits answers instead (older firmware's AP-mode catch-all)
        self.dvcc = {'dvccState': 3, 'dvccUntrustReason': 0, 'dvccRxCvl': 1420, 'dvccRxCcl': 375}   # CSV2
        self.ts = {'ts_DvccVeCan': 999999, 'ts_DvccWifi': 1200}
        self.devposts = []             # accepted /gxDevices bodies, parsed: {'v': 1, 'd': [[kind, name, ...], ...]}
        self.dev_every = 5             # everyS it answers
        self.dev_post_ms = None        # GxRate "devPost" (firmware 2026-10-29); None: firmware that sends none
        self.rate_push = threading.Event()   # send GxRate again now (a dashboard connected or left)
        self.demo_devs = {}            # CSV4 demoShoreA / demoFridgeA / demoOrionA / demoInverterA, A x100 (absent: ROLL_EMPTY)
        self.csv4_extra = {}           # any other CSV4 values (sendMs, sessionId)
        self.devices_page = None       # a 200 page /gxDevices answers instead (older firmware's AP-mode catch-all)
        self.stop = threading.Event()
        self.lt_bin = make_longterm()
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), self._handler())
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.host = '127.0.0.1:%d' % self.port
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.stop.set()
        self.server.shutdown()
        self.server.server_close()

    def set_on_off(self, v):
        # The regulator applies a write at once; with confirm_delay_s its stream shows it only at the confirming CSV1
        # (and in CSV3 at once when onoff_csv3_echo, as the firmware's settingsDirty echo does)
        if self.confirm_delay_s is None:
            self.on_off = v
            return
        old = self.on_off if self.confirm is None else self.confirm[1]
        self.confirm = (time.time() + self.confirm_delay_s, v)
        self.on_off = old
        if self.onoff_csv3_echo:
            self.on_off_c3 = v
            self.csv3_dirty.set()
        else:
            self.on_off_c3 = old

    def requests(self, path):
        with self.lock:
            return [r for r in self.log if r['path'] == path]

    def gets(self):
        # /get requests addressed to this unit (a write carrying another uid is refused 409)
        return [r for r in self.requests('/get') if r['q'].get('uid', '').upper() in ('', self.uid)]

    # ---- frames ----

    def frame(self, ev, t):
        names = FIELDS[ev]
        vals = dict.fromkeys(names, 0)
        if ev == 'CSVData':
            vals.update(MeasuredAmps=5000 + int(100 * math.sin(t)), IBV=1380, BatteryV=1385, Bcur=2500, iiout=350,
                        dutyCycle=4500, pidAltPV=4900, RPM=1800, AlternatorTemperatureF=15000, Ignition=1,
                        OnOff=self.on_off, fieldActiveStatus=self.field_status, fieldEventReason=self.off_reason, vvout=1200, voltageTarget=1420,
                        setpointLimited=6000)
        elif ev == 'CSVData2':
            up, lo = int(self.uid_in_stream[:8], 16), int(self.uid_in_stream[8:], 16)
            vals.update(SOC_percent=8500, battTempActiveF=770, baroPressure=10132, deviceIdUpper=signed32(up),
                        deviceIdLower=signed32(lo), LatitudeNMEA=41500000, LongitudeNMEA=-71250000)
            vals.update({k: v for k, v in self.dvcc.items() if k in vals})
        elif ev == 'CSVData3':
            vals.update(self.csv3)
            if self.clock_fields:
                vals['OnOff'] = self.on_off if self.on_off_c3 is None else self.on_off_c3
        elif ev == 'CSVData4':
            vals.update(chargeStage=1, ctrlLimiter=0, TrueWindSpeedNMEA=1200, TrueWindAngleNMEA=-170 if int(t) % 2
                        else 170, ApparentWindSpeedNMEA=1500, ApparentWindAngleNMEA=350 if int(t) % 2 else 10)
            vals.update({k: v for k, v in self.flow.items() if k in vals})
            for k in ('demoShoreA', 'demoFridgeA', 'demoOrionA', 'demoInverterA'):
                if k in vals:
                    vals[k] = self.demo_devs.get(k, -2000000000)
            vals.update({k: v for k, v in self.csv4_extra.items() if k in vals})
            if self.wind_demo_only and self.csv3['hardwarePresent'] != 0:
                vals.update(dict.fromkeys(WIND_COLS, -2000000000))
        if self.clock_fields and ev in ('CSVData', 'CSVData3'):
            vals['sendMs'] = int((time.time() - self.started) * 1000) % 2 ** 32
            vals['sessionId'] = self.session
        out = [vals[n] for n in names]
        if ev == 'CSVData' and self.extra_csv1_field:
            out.append(7)
        return '%d,' % len(out) + ','.join(str(v) for v in out)

    def schema_frame(self, ev, t):
        if ev == 'AltLive':
            v = dict.fromkeys(ALT_LIVE, 0.0)
            v.update(latchPct=93.0, pct=95.5, valid=1.0)
            return ','.join('%.3f' % v[n] for n in ALT_LIVE)
        if ev == 'PerfLive':
            v = dict.fromkeys(PERF_LIVE, 0.0)
            v.update(valid=1.0, pct=87.25, spd=6.1, best=7.0, ws=12.0, wa=45.0)
            return ','.join('%.3f' % v[n] for n in PERF_LIVE)
        v = dict.fromkeys(MOTOR_LIVE, 0.0)
        v.update(valid=1.0, pct=91.0, rpm=1800.0)
        return ','.join('%.3f' % v[n] for n in MOTOR_LIVE)

    # ---- HTTP ----

    def _handler(self):
        reg = self

        class H(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.0'

            def log_message(self, *a):
                pass

            def _send(self, code, body=b'', ctype='text/plain', headers=None):
                if isinstance(body, str):
                    body = body.encode()
                self.send_response(code)
                self.send_header('Content-Type', ctype)
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Connection', 'close')
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                if body:
                    self.wfile.write(body)

            def _parse(self):
                u = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(u.query, keep_blank_values=True))
                body = {}
                if self.command == 'POST':
                    n = int(self.headers.get('Content-Length') or 0)
                    raw = self.rfile.read(n).decode() if n else ''
                    body = dict(urllib.parse.parse_qsl(raw, keep_blank_values=True))
                with reg.lock:
                    reg.log.append({'method': self.command, 'path': u.path, 'q': q, 'body': body, 't': time.time(),
                                    'raw': self.path})
                return u.path, q, body

            def do_POST(self):
                self.do_GET()

            def do_GET(self):
                path, q, body = self._parse()
                if path == '/identify':
                    ident = {'device': 'xreg-010', 'uid': reg.uid, 'fw': reg.fw, 'name': reg.name,
                             'pwreq': int(reg.password), 'paired': [{'uid': u, 'name': ''} for u in reg.paired]}
                    if reg.gx:
                        ident['gx'] = reg.gx
                        ident['demo'] = int(reg.csv3['hardwarePresent'] == 0)
                    return self._send(200, json.dumps(ident), 'application/json')
                if path == '/script.js':
                    if self.headers.get('If-None-Match') == SCRIPT_ETAG:
                        return self._send(304, headers={'ETag': SCRIPT_ETAG})
                    reg.script_downloads += 1
                    return self._send(200, SCRIPT_GZ, 'application/javascript',
                                      {'Content-Encoding': 'gzip', 'ETag': SCRIPT_ETAG})
                if path == '/altschema':
                    return self._send(200, json.dumps({'live': ALT_LIVE, 'settings': ['a', 'b']}), 'application/json')
                if path == '/perfschema':
                    return self._send(200, json.dumps({'live': PERF_LIVE, 'motorLive': MOTOR_LIVE,
                                                       'settings': ['x']}), 'application/json')
                if path in ('/events', '/gxevents'):
                    if path == '/gxevents' and not reg.gx:
                        return self._send(404, 'Not Found')
                    if path == '/gxevents' and reg.gxevents_refuse:
                        return self._send(503, 'busy')
                    return self._stream(path == '/gxevents')
                if path == '/baroHistory.bin':
                    return self._send(200, make_baro(), 'application/octet-stream')
                if path == '/longTermPlots.bin':
                    return self._send(200, reg.lt_bin, 'application/octet-stream')
                if path == '/alttrend.csv':
                    # (toggle lane) the rows say which window was asked for: the last ?hours engine-hours, else all
                    hours = int(q.get('hours') or 0)
                    first = 600.0 - hours if hours else 1.0
                    return self._send(200, 'engHours,worstPct,overallPct\n%.3f,92.0,95.0\n%.3f,91.5,94.8\n'
                                      % (first, first + 1), 'text/csv')
                if path == '/altsess.csv':
                    return self._send(200, 'sec,pct\n10,95.0\n20,94.0\n', 'text/csv')
                if path == '/perfcurve.csv':
                    return self._send(200, 'BEFRONT1,SAIL\naws,awa,sea,spd\n10,45,0,6.5\n', 'text/csv')
                if path == '/tempSensors':
                    role = next((i for i, k in enumerate(OW_ASSIGN) if reg.roles[k] == PROBE), -1)
                    return self._send(200, json.dumps({'count': 1, 'unassigned': int(role < 0), 'probes': [
                        {'id': PROBE, 'role': role, 'tempF': 150.0, 'ageS': 2, 'ok': 10, 'fail': 0, 'present': 1}],
                        'roles': {'alt': reg.roles['owAssignAlt'], 'batt': reg.roles['owAssignBatt'],
                                  'extra': reg.roles['owAssignExtra']},
                        'enabled': {'batt': 0, 'extra': 0}, 'scanning': 0}), 'application/json')
                if path == '/kneeLearnState':
                    return self._send(200, json.dumps({'enable': 1, 'marginPct': 2.5, 'onsetA': 1.2, 'reArmA': 3,
                                                       'dwellSec': 4, 'stepPct': 0.25, 'tempComp': 0,
                                                       'tempRefF': 140, 'maxFloorPct': 20, 'rpmTolPct': 5,
                                                       'tempTolF': 5, 'dutyTolPct': 1}), 'application/json')
                if path == '/zerofitstate':
                    return self._send(200, json.dumps({'phase': 0, 'rej': 0}), 'application/json')
                if path == '/armSettings':
                    return self._arm(q, body)
                if path == '/demoBackfill':
                    if reg.csv3['hardwarePresent'] != 0 or reg.backfill_refuse > 0:
                        reg.backfill_refuse -= 1
                        return self._send(404, 'Demo mode is off')
                    return self._send(200, json.dumps(make_backfill(time.time())), 'application/json')
                if path == '/gxLimits':
                    return self._limits(q)
                if path == '/gxDevices':
                    return self._devices(q, body)
                if path == '/gxrate':
                    if not reg.gx:
                        return self._send(404, 'Not Found')
                    if q.get('uid') and q['uid'].upper() != reg.uid:
                        return self._send(409, reg.uid)
                    if 'fast' in q:
                        reg.fast.append(q.get('fast'))
                        lvl = int(q.get('fast') or 0)
                        lvl = 0 if lvl == 0 else 2 if lvl >= 2 and reg.live_ms is not None else 1
                        if lvl == 2 and reg.base_csv1_s is None:
                            reg.base_csv1_s = reg.gx_csv1_s
                            reg.gx_csv1_s = reg.live_ms / 1000.0
                        elif lvl != 2 and reg.base_csv1_s is not None:
                            reg.gx_csv1_s, reg.base_csv1_s = reg.base_csv1_s, None
                        if lvl != reg.fast_level:
                            reg.fast_level = lvl
                            reg.rate_push.set()
                    if 'flow' in q:
                        reg.flow_holds.append(q.get('flow'))
                    return self._send(200, 'OK')
                if path == '/getAuthToken':
                    reg.token_requests += 1
                    return self._send(200, json.dumps({'registered': True, 'token': reg.token} if reg.token
                                                      else {'registered': False}), 'application/json')
                if path == '/get' or path == '/resetThermalPID':
                    return self._get(q, body)
                return self._send(404, 'Not Found')

            def _arm(self, q, body):
                if self.command == 'GET' and 'arm' not in body:
                    return self._send(200, json.dumps({'armed': int(reg.armed), 'pwreq': int(reg.password)}),
                                      'application/json')
                if body.get('arm') == '1':
                    if reg.password:
                        return self._send(403, '{"err":"password"}', 'application/json')
                    reg.armed = True
                elif body.get('arm') == '0':
                    reg.armed = False
                return self._send(200, json.dumps({'armed': int(reg.armed)}), 'application/json')

            def _limits(self, q):
                if reg.gx < 3:
                    return self._send(404, 'Not Found')
                if reg.limits_page is not None:
                    return self._send(200, reg.limits_page, 'text/html')
                own = reg.limits_uid or reg.uid
                if q.get('uid') and q['uid'].upper() != own:
                    return self._send(409, own)
                if q.get('gx') != '1':
                    return self._send(400, 'gx=1 required')
                for k in ('cvl', 'ccl'):
                    if k in q:
                        try:
                            if not math.isfinite(float(q[k])):
                                raise ValueError
                        except ValueError:
                            return self._send(400, 'cvl and ccl must be numbers')
                with reg.lock:
                    reg.limits.append({k: q[k] for k in ('cvl', 'ccl') if k in q})
                return self._send(200, json.dumps({'ok': 1, 'follow': 1, 'source': 0, 'state': reg.dvcc['dvccState']}),
                                  'application/json')

            def _devices(self, q, body):
                # the firmware's contract (claude_docs/firmware-internals.md, "GX device readings over WiFi")
                if reg.devices_page is not None:
                    return self._send(200, reg.devices_page, 'text/html')
                if reg.gx < 4 or self.command != 'POST':
                    return self._send(404, 'Not Found')
                if q.get('uid') and q['uid'].upper() != reg.uid:
                    return self._send(409, reg.uid)
                if q.get('gx') != '1':
                    return self._send(400, 'gx=1 required')
                try:
                    j = json.loads(body.get('j', ''))
                    assert isinstance(j, dict) and j.get('v') == 1 and isinstance(j.get('d'), list)
                except (ValueError, AssertionError):
                    return self._send(400, 'j must be {"v":1,"d":[...]}')
                with reg.lock:
                    reg.devposts.append(j)
                return self._send(200, json.dumps({'ok': 1, 'everyS': reg.dev_every, 'kept': min(16, len(j['d']))}),
                                  'application/json')

            def _get(self, q, body):
                params = dict(q)
                params.update(body)
                if params.get('uid') and params['uid'].upper() != reg.uid:
                    return self._send(409, reg.uid)
                if params.get('OnOff') in ('0', '1') and reg.ignore_onoff:
                    return self._send(200, '')
                if params.get('OnOff') == '0':
                    reg.set_on_off(0)
                    return self._send(200, '0')
                if 'SilenceAlarm' in params:
                    return self._send(200, 'OK')
                gx_ok = reg.gx >= 2 and params.get('gx') == '1'
                if not gx_ok and not reg.armed:
                    return self._send(403, 'Settings not armed')
                if gx_ok and DENIED & set(params):
                    return self._send(403, 'Not available from a Victron GX')
                if params.get('OnOff') == '1':
                    if reg.delay_onoff_s:
                        time.sleep(reg.delay_onoff_s)
                    src, n = params.get('src'), int(params.get('n', 0))
                    with reg.lock:
                        if src and n <= reg.n_track.get(src, 0):
                            return self._send(200, '')
                        reg.n_track[src] = n
                    if reg.refuse_onoff:
                        return self._send(200, reg.refuse_onoff)
                    reg.set_on_off(1)
                    return self._send(200, '1')
                found, msg = False, ''
                for r, k in enumerate(OW_ASSIGN):   # the firmware's role loop, in role order
                    if k not in params:
                        continue
                    found = True
                    v = params[k].strip().lower()
                    if v in ('', 'none'):
                        reg.roles[k] = ''
                    elif not re.fullmatch(r'[0-9a-f]{16}', v):
                        msg = 'Rejected: probe id must be 16 hex characters or none'
                    elif any(reg.roles[o] == v for o in OW_ASSIGN if o != k):
                        msg = 'Rejected: that probe is already assigned to another role'
                    else:
                        reg.roles[k] = v
                for k, v in params.items():
                    if k in ('uid', 'src', 'n', 'gx', 'tok') or k in OW_ASSIGN:
                        continue
                    if k in reg.unknown_keys:
                        continue
                    found = True
                    m = re.fullmatch(r'rpmCapKW(\d)', k)
                    if m:
                        reg.csv3['rpmCapPowerTable' + m.group(1)] = int(round(float(v) * 1000))
                    elif k in reg.csv3:
                        reg.csv3[k] = int(round(float(v) * CSV3_SCALE.get(k, 1.0)))
                if reg.csv3_echo_delay_s:
                    threading.Timer(reg.csv3_echo_delay_s, reg.csv3_dirty.set).start()
                else:
                    reg.csv3_dirty.set()
                if not found:
                    return self._send(200, 'No message sent, the request_hasParam found no match')
                return self._send(200, msg or 'OK')

            def _stream(self, gx):
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Cache-Control', 'no-cache')
                self.end_headers()
                reg.streams += 1
                gen = reg.stream_gen
                iv = {'CSVData': reg.gx_csv1_s, 'CSVData4': 1.0, 'CSVData2': 2.0, 'TimestampData': 3.0} if gx else \
                     {'CSVData': 0.1, 'CSVData4': 0.5, 'CSVData2': 2.0, 'TimestampData': 3.0}
                last = dict.fromkeys(list(iv) + ['CSVData3', 'schema'], 0.0)
                try:
                    self.wfile.write(b'retry: 10000\nid: 1\ndata: hello!\n\n')
                    def rate():
                        r = {'tier': 'run', 'fast': reg.fast_level, 'csv1': int(reg.gx_csv1_s * 1000), 'csv4': 1000, 'csv2': 10000, 'csv3': 60000,
                             'ts': 6000}
                        if reg.dev_post_ms is not None:
                            r['devPost'] = reg.dev_post_ms
                        return 'event: GxRate\ndata: %s\n\n' % json.dumps(r, separators=(',', ':'))
                    if gx:
                        reg.rate_push.clear()
                        self.wfile.write(rate().encode())
                    while not reg.stop.is_set() and reg.stream_gen == gen:
                        t = time.time()
                        out = []
                        if gx and reg.rate_push.is_set():
                            reg.rate_push.clear()
                            self.wfile.write(rate().encode())
                        if gx:
                            iv['CSVData'] = reg.gx_csv1_s   # a /gxrate hold changes it on the open stream
                        c = reg.confirm
                        if c is not None and t >= c[0]:
                            reg.confirm, reg.on_off, reg.on_off_c3 = None, c[1], None
                            last['CSVData'] = 0.0   # the confirming CSV1 goes now
                        for ev, every in iv.items():
                            if ev == 'CSVData' and reg.confirm is not None:
                                continue           # no CSV1 between the write and its confirmation
                            if t - last[ev] >= every:
                                last[ev] = t
                                out.append((ev, self_frame(ev, t)))
                        if reg.csv3_dirty.is_set() or t - last['CSVData3'] >= reg.csv3_every_s:
                            reg.csv3_dirty.clear()
                            last['CSVData3'] = t
                            out.append(('CSVData3', reg.frame('CSVData3', t)))
                        if (not gx or reg.schema_events_on_gx) and t - last['schema'] >= 1.0:
                            last['schema'] = t
                            for ev in ('AltLive', 'PerfLive', 'MotorLive'):
                                out.append((ev, reg.schema_frame(ev, t)))
                        if not gx and t - last.get('console', 0) >= 2.0:
                            last['console'] = t
                            out.append(('console', 'Fake console line'))
                        for ev, data in out:
                            self.wfile.write(('event: %s\ndata: %s\n\n' % (ev, data)).encode())
                        self.wfile.flush()
                        time.sleep(0.05)
                except (BrokenPipeError, ConnectionResetError, OSError):
                    pass
                finally:
                    reg.streams -= 1

        def self_frame(ev, t):
            if ev == 'TimestampData':
                names = FIELDS['TimestampData']
                return '%d,' % len(names) + ','.join(str(reg.ts.get(k, 0)) for k in names)
            return reg.frame(ev, t)
        return H


def swap(a, b):
    # Two regulators trade addresses (a DHCP swap): each server now answers as the other unit, with its settings
    with a.lock, b.lock:
        for k in ('uid', 'uid_in_stream', 'name', 'fw', 'gx', 'paired', 'csv3', 'on_off', 'roles', 'password'):
            va, vb = getattr(a, k), getattr(b, k)
            setattr(a, k, vb)
            setattr(b, k, va)
        a.stream_gen += 1
        b.stream_gen += 1


if __name__ == '__main__':
    import sys
    r = FakeReg(gx=0 if '--legacy' in sys.argv else 2)
    print('fake regulator on', r.host, 'fields', {k: len(v) for k, v in FIELDS.items()}, 'alt', len(ALT_LIVE),
          'perf', len(PERF_LIVE), 'motor', len(MOTOR_LIVE))
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        r.close()
