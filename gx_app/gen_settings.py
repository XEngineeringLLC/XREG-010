#!/usr/bin/env python3
"""Generate gx_app/xregulator/bridge/settings_schema.json from gx_app/settings_keys.txt.

Every label, option, range, tooltip, echo field, scale and feature gate is read from web_src/index.html and
web_src/script.js (web_src/styles.css only to confirm what each gate greys and which note it shows);
the firmware .ino files are read only for what the web app does not show
(the CSV3 wire scale of a field, whether a key is parsed as an integer, clamps, keyless keys,
keys a GX may not send).

  python3 gx_app/gen_settings.py           write the schema (refuses if anything is unresolved)
  python3 gx_app/gen_settings.py --check   resolve everything and compare with the schema on disk;
                                           exit 1 on an unresolved key or a stale schema

Standard library only, so it runs anywhere compress_web.sh runs.
"""
import hashlib
import html
import json
import os
import re
import sys
from html.parser import HTMLParser

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
KEYS_FILE = os.path.join(HERE, 'settings_keys.txt')
SCHEMA_FILE = os.path.join(HERE, 'xregulator', 'bridge', 'settings_schema.json')
INDEX_HTML = os.path.join(REPO, 'web_src', 'index.html')
SCRIPT_JS = os.path.join(REPO, 'web_src', 'script.js')
STYLES_CSS = os.path.join(REPO, 'web_src', 'styles.css')

# Trailing "(x)" in an app label that is a unit, so the GX can show it beside the value (and swap °F for °C).
UNIT_WORDS = {'%', 'V', 'A', 'Hz', '°F', '°C', 's', 'sec', 'min', 'hrs', 'h', 'ms', 'RPM', 'W', 'kW', 'kWh', 'Ω',
              'A/s', '% per day', '°'}
# onsubmit handlers whose effect on the submitted value is known. Anything else fails: it might rewrite the value.
SUBMIT_HANDLERS_PLAIN = ('validateAndSubmitBatteryNumber(', 'rpmScaleFormSubmit(', 'handleAlarmTest(',
                         'handleResetLatch(', 'window.gpsManualOverride = true')
SUBMIT_REGEX_GUARD = re.compile(r"^return /\^\[[0-9]+\]\$/\.test\(this\.elements\.\w+\.value\)$")
SUBMIT_TEMP = re.compile(r"^return convertTempFormIfNeeded\(this,\s*'(\w+)',\s*(true|false)(?:,\s*(true|false))?\)$")
# The app's "greyed while it does not apply" gates. script.js sets body class `name` from echoes (kind 'body'), or
# toggles .is-gated on one container (kind 'is-gated'); styles.css then greys every setting inside the container. A key
# whose control sits inside a container gets that gate. `when` mirrors the script.js lines in `js`, which must stay verbatim.
# Conditions: {key, eq|ne|le: number}, {any: [...]}, {all: [...]}; a key not yet echoed never holds.
SHUNT_ABSENT = {'any': [{'key': 'BatteryShuntPresent', 'eq': 0}, {'key': 'ShuntResistanceMicroOhm', 'le': 0}]}
GATES = [
    {'name': 'no-batt-current', 'kind': 'body', 'container': '.gate-batt-current', 'note': 'gate-note-shunt',
     'when': SHUNT_ABSENT,
     'js': ("const noShunt = String(data.BatteryShuntPresent) === '0' || (parseFloat(data.ShuntResistanceMicroOhm) || 0) <= 0;",
            "document.body.classList.toggle('no-batt-current', noShunt);")},
    {'name': 'no-soc-src', 'kind': 'body', 'container': '.gate-soc-src', 'note': 'gate-note-soc-src',
     'when': {'all': [SHUNT_ABSENT, {'key': 'BattMonSrc', 'eq': 0}]},
     'js': ("document.body.classList.toggle('no-soc-src', document.body.classList.contains('no-batt-current') "
            "&& Number(data.BattMonSrc) === 0);",)},
    {'name': 'no-extra-temp', 'kind': 'body', 'container': '.gate-extra-temp', 'note': 'gate-note-extra',
     'when': {'key': 'extraTempProbeEnable', 'eq': 0},
     'js': ("document.body.classList.toggle('no-extra-temp', String(data.extraTempProbeEnable) === '0');",)},
    {'name': 'float-gated-fields', 'kind': 'is-gated', 'container': '#float-gated-fields', 'note': 'gate-note',
     'when': {'key': 'UseFloat', 'ne': 1},
     'js': ("const sel = document.querySelector('select[name=\"UseFloat\"]');",
            "const el = document.getElementById('float-gated-fields');",
            "if (el) el.classList.toggle('is-gated', v !== 1);")},
]
GATE_BY_NAME = {g['name']: g for g in GATES}
# Select options script.js disables while a gate holds: (select key, option value, gate name, script.js lines).
OPTION_GATES = [
    ('UseFloat', 2, 'no-batt-current', ("const zc = document.querySelector('select[name=\"UseFloat\"] option[value=\"2\"]');",
                                         "if (zc) zc.disabled = noShunt;")),
]
VOID_TAGS = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source',
             'track', 'wbr'}
AUTO_CLOSE = {'p', 'li', 'option', 'tr', 'td', 'th', 'dt', 'dd'}


class CheckError(Exception):
    pass


# ---------------------------------------------------------------- HTML

class Node:
    __slots__ = ('tag', 'attrs', 'children', 'parent', 'line')

    def __init__(self, tag, attrs, parent, line):
        self.tag, self.attrs, self.children, self.parent, self.line = tag, attrs, [], parent, line

    def get(self, k, d=None):
        return self.attrs.get(k, d)

    def classes(self):
        return (self.attrs.get('class') or '').split()

    def elements(self):
        return [c for c in self.children if isinstance(c, Node)]

    def walk(self):
        for c in self.children:
            if isinstance(c, Node):
                yield c
                yield from c.walk()

    def text(self, skip=None):
        out = []
        for c in self.children:
            if isinstance(c, Node):
                if c.tag == 'br':
                    out.append(' ')
                    continue
                if (skip and skip(c)) or re.search(r'display:\s*none', c.attrs.get('style') or ''):
                    continue
                out.append(c.text(skip))
            else:
                out.append(c)
        return ''.join(out)

    def ancestors(self):
        p = self.parent
        while p is not None:
            yield p
            p = p.parent


class TreeBuilder(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node('#root', {}, None, 0)
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        if tag in AUTO_CLOSE and self.stack[-1].tag == tag:
            self.stack.pop()
        n = Node(tag, {k: (v if v is not None else '') for k, v in attrs}, self.stack[-1], self.getpos()[0])
        self.stack[-1].children.append(n)
        if tag not in VOID_TAGS:
            self.stack.append(n)

    def handle_startendtag(self, tag, attrs):
        n = Node(tag, {k: (v if v is not None else '') for k, v in attrs}, self.stack[-1], self.getpos()[0])
        self.stack[-1].children.append(n)

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i].tag == tag:
                del self.stack[i:]
                return

    def handle_data(self, data):
        self.stack[-1].children.append(data)


EMOJI = re.compile('[\u2139\u2600-\u27bf\ufe0f\u200d\U0001f000-\U0001faff]')


def clean(s):
    return ' '.join(EMOJI.sub('', html.unescape(s)).split())   # str.split() also splits on NBSP


class Html:
    def __init__(self, path):
        with open(path, encoding='utf-8') as f:
            self.src = f.read()
        tb = TreeBuilder()
        tb.feed(self.src)
        tb.close()
        self.root = tb.root
        self.all = list(self.root.walk())
        self.by_id = {}
        for n in self.all:
            i = n.get('id')
            if i and i not in self.by_id:
                self.by_id[i] = n
        self.plain_text = clean(self.root.text())

    def named(self, name):
        return [n for n in self.all if n.tag in ('input', 'select') and n.get('name') == name]


def form_of(n):
    for a in n.ancestors():
        if a.tag == 'form':
            return a
    return None


def row_of(n):
    for a in n.ancestors():
        if 'form-row' in a.classes():
            return a
    return None


def is_tooltip(n):
    return 'tooltip' in n.classes() or 'tooltip-box' in n.classes()


def tooltip_text(row):
    if row is None:
        return ''
    for n in row.walk():
        if 'tooltip-box' in n.classes():
            return clean(n.text())
    return ''


def label_parts(row, key):
    """App label of a form row -> (label, unit, unit_is_temp). Drops the echo span and the tooltip."""
    fl = None
    for n in row.walk():
        if 'form-label' in n.classes():
            fl = n
            break
    if fl is None:
        return None, None, False
    temp = any('temp-unit-label' in n.classes() for n in fl.walk())

    def skip(n):
        return is_tooltip(n) or (n.get('id') or '').startswith(key + '_echo') or (n.get('id') or '').endswith('_echo')
    t = clean(fl.text(skip))
    t = re.sub(r'\((?:\s|,|\?|%\s*now|now)*\)', '', t)      # the emptied echo parentheses "( ? )", "( ? , ? % now)"
    t = re.sub(r'\s*:\s*$', '', clean(t))
    t = re.sub(r'\s+limiting$', '', t)                        # the live "limiting" badge next to some labels
    unit = None
    m = re.match(r'^(.*?)\s*\(\s*([^()]+?)\s*\)$', t)
    if m and m.group(2) in UNIT_WORDS:
        t, unit = m.group(1).strip(), m.group(2)
    return t, unit, temp


def group_of(n):
    for a in n.ancestors():
        if a.tag == 'details':
            for c in a.elements():
                if c.tag == 'summary':
                    # an Integrations summary carries a badge and a live state word after its title
                    return clean(c.text(lambda x: bool({'integ-badge', 'integ-state'} & set(x.classes()))))
    return None


def num(s):
    if s is None or s == '':
        return None
    v = float(s)
    return int(v) if v == int(v) and 'e' not in s.lower() and '.' not in s else v


def step_decimals(step):
    if step is None:
        return 0
    s = ('%.10f' % step).rstrip('0')
    return len(s.split('.')[1]) if '.' in s else 0


# ---------------------------------------------------------------- JS

def js_string_array(js, name):
    m = re.search(r'const\s+' + name + r'\s*=\s*\[(.*?)\];', js, re.S)
    if not m:
        return None
    body = re.sub(r'//[^\n]*', '', m.group(1))
    return [json_str(s) for s in re.findall(r"'(?:[^'\\]|\\.)*'|\"(?:[^\"\\]|\\.)*\"", body)]


def json_str(lit):
    """A JS single- or double-quoted literal -> Python str."""
    body = lit[1:-1]
    body = re.sub(r'\\u([0-9a-fA-F]{4})', lambda m: chr(int(m.group(1), 16)), body)
    out, i = [], 0
    esc = {'n': '\n', 't': '\t', "'": "'", '"': '"', '\\': '\\'}
    while i < len(body):
        ch = body[i]
        if ch == '\\' and i + 1 < len(body):
            out.append(esc.get(body[i + 1], body[i + 1]))
            i += 2
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


def match_close(s, i, open_ch, close_ch):
    """Index of the bracket closing s[i] (== open_ch), skipping JS strings and comments."""
    depth, j, n = 0, i, len(s)
    while j < n:
        c = s[j]
        if c in '\'"`':
            j = skip_string(s, j)
            continue
        if c == '/' and j + 1 < n and s[j + 1] == '/':
            j = s.find('\n', j)
            j = n if j < 0 else j
            continue
        if c == '/' and j + 1 < n and s[j + 1] == '*':
            j = s.find('*/', j + 2) + 2
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return -1


def skip_string(s, j):
    q = s[j]
    j += 1
    while j < len(s):
        if s[j] == '\\':
            j += 2
            continue
        if s[j] == q:
            return j + 1
        j += 1
    return j


def literal_concat(expr):
    """Concatenated JS string literals at the start of expr; stops at the first non-literal term."""
    parts, i = [], 0
    while True:
        while i < len(expr) and expr[i] in ' \t\r\n+':
            i += 1
        if i < len(expr) and expr[i] in '\'"':
            j = skip_string(expr, i)
            parts.append(json_str(expr[i:j]))
            i = j
        else:
            break
    return ''.join(parts) if parts else None


class Js:
    def __init__(self, path):
        with open(path, encoding='utf-8') as f:
            self.src = f.read()
        self.functions = {}
        for m in re.finditer(r'(?:async\s+)?function\s+(\w+)\s*\(', self.src):
            b = self.src.find('{', m.end())
            e = match_close(self.src, b, '{', '}')
            if b > 0 and e > 0 and m.group(1) not in self.functions:
                self.functions[m.group(1)] = (m.start(), b, e)
        self.fields = {s: js_string_array(self.src, s + '_FIELDS') or [] for s in ('CSV1', 'CSV2', 'CSV3', 'CSV4')}
        self.registry = self._registry()

    def body(self, name):
        f = self.functions.get(name)
        return self.src[f[1]:f[2] + 1] if f else None

    def enclosing_function(self, pos):
        best = None
        for name, (s, b, e) in self.functions.items():
            if b <= pos <= e and (best is None or b > self.functions[best][1]):
                best = name
        return best

    def _registry(self):
        i = self.src.find('updateAllEchosOptimized._echoUpdates = [')
        if i < 0:
            raise CheckError('script.js: echo registry updateAllEchosOptimized._echoUpdates not found')
        b = self.src.find('[', i)
        e = match_close(self.src, b, '[', ']')
        block = self.src[b + 1:e]
        out, j = {}, 0
        while True:
            j = block.find('{', j)
            if j < 0:
                break
            k = match_close(block, j, '{', '}')
            ent = block[j + 1:k]
            j = k + 1
            mk = re.search(r"\bkey:\s*'(\w+)'", ent)
            mi = re.search(r"\bid:\s*'([\w-]+)'", ent)
            mt = re.search(r'\btransform:\s*(.*)$', ent, re.S)
            if mk and mi and mt:
                out.setdefault(mi.group(1), (mk.group(1), clean(mt.group(1)).rstrip(',').strip()))
        return out

    def stream_of(self, field):
        for s in ('CSV3', 'CSV2', 'CSV4', 'CSV1'):
            if field in self.fields[s]:
                return s
        return None


def parse_transform(t):
    """Echo transform text -> dict(scale, decimals, labels, temp, zeroOff). None if not reducible to a scale."""
    t = t.strip()
    r = {'scale': 1, 'decimals': None, 'labels': None, 'temp': None, 'zeroOff': False}
    if not t.startswith('v =>'):
        return None
    e = t[4:].strip()
    m = re.match(r"^Number\(v\) === 0 \? '([^']*)' : (.*)$", e)
    if m:
        r['zeroOff'] = m.group(1)
        e = m.group(2).strip()
    labels = re.findall(r"v == (\d+) \? '([^']*)' :", e)
    if labels:
        rest = re.sub(r"v == (\d+) \? '([^']*)' :\s*", '', e).strip()
        mm = re.match(r"^'([^']*)'$", rest)
        if not mm:
            return None
        vals = {int(a): b for a, b in labels}
        others = sorted({0, 1, 2} - set(vals)) if len(vals) >= 2 else sorted({0, 1} - set(vals))
        if len(others) != 1:
            return None
        vals[others[0]] = mm.group(1)
        r['labels'] = [{'value': k, 'label': vals[k]} for k in sorted(vals)]
        return r
    m = re.match(r"^\(\[(.*)\]\[v\] \?\? v\)$", e)
    if m:
        lits = re.findall(r"'([^']*)'", m.group(1))
        r['labels'] = [{'value': i, 'label': s} for i, s in enumerate(lits)]
        return r
    pats = [
        (r'^v$', lambda m: (1, None, None)),
        (r'^v / (\d+)$', lambda m: (int(m.group(1)), None, None)),
        (r'^\(v / (\d+)\)\.toFixed\((\d)\)$', lambda m: (int(m.group(1)), int(m.group(2)), None)),
        (r"^\(v / (\d+)\)\.toFixed\((\d)\) \+ '([^']*)'$", lambda m: (int(m.group(1)), int(m.group(2)), None)),
        (r'^Math\.round\(v / (\d+)\)$', lambda m: (int(m.group(1)), 0, None)),
        (r'^Math\.round\(toDisplayTemp\(v\)\)$', lambda m: (1, 0, 'abs')),
        (r'^Math\.round\(toDisplayTempDelta\(v\)\)$', lambda m: (1, 0, 'delta')),
        (r'^toDisplayTempDelta\(v / (\d+)\)\.toFixed\((\d)\)$', lambda m: (int(m.group(1)), int(m.group(2)), 'delta')),
        (r'^toDisplayTemp\(v / (\d+)\)\.toFixed\((\d)\)$', lambda m: (int(m.group(1)), int(m.group(2)), 'abs')),
    ]
    for p, f in pats:
        m = re.match(p, e)
        if m:
            r['scale'], r['decimals'], r['temp'] = f(m)
            return r
    return None


# ---------------------------------------------------------------- firmware

class Firmware:
    def __init__(self, repo):
        parts = []
        for fn in sorted(os.listdir(repo)):
            if fn.endswith('.ino'):
                with open(os.path.join(repo, fn), encoding='utf-8', errors='replace') as f:
                    parts.append(f.read())
        self.src = '\n'.join(parts)
        self.csv3_scale = {}
        i = self.src.find('#define CSV3_LIST(X)')
        for line in (self.src[i:].split('\n') if i >= 0 else []):
            body = re.sub(r'/\*.*?\*/', '', line).strip()
            x = re.match(r'^X\((\w+),\s*"[^"]*",\s*(.*)\)\s*\\?$', body)
            if x:
                sm = re.search(r'SafeInt\(.*,\s*(\d+)\)\s*$', x.group(2))
                self.csv3_scale[x.group(1)] = int(sm.group(1)) if sm else 1
            if not body.endswith('\\'):
                if not line.lstrip().startswith('#define'):
                    break
        g = self.src.find('server.on("/get"')
        a = self.src.find('settingsArmActive(request)', g)
        self.keyless = set(re.findall(r'hasParam\("(\w+)"\)', self.src[g:a])) - {'OnOff'} if g >= 0 and a > g else set()
        self.gx_denied = set()
        dm = re.search(r'bool gxParamDenied\(AsyncWebServerRequest \*request\) \{(.*?)\n\}', self.src, re.S)
        if dm:
            deny = re.search(r'DENY\[\]\s*=\s*\{(.*?)\};', dm.group(1), re.S)
            if deny:
                self.gx_denied = set(re.findall(r'"(\w+)"', deny.group(1)))
        self.handlers = self._handlers()

    def _handlers(self):
        out = {}
        ms = list(re.finditer(r'(?:paramOk\(request,\s*|request->hasParam\()"(\w+?)(%d)?"\)|snprintf\(\w+, sizeof\(\w+\), "(\w+?)%d"', self.src))
        for i, m in enumerate(ms):
            key = m.group(1) or m.group(3)
            if m.group(2) or m.group(3):
                key += '{i}'
            eol = self.src.find('\n', m.end())
            end = next((x.start() for x in ms[i + 1:] if x.start() > eol), m.end() + 400)
            out.setdefault(key, self.src[m.start():min(end, m.end() + 1500)])
        return out

    def handler(self, key):
        if key in self.handlers:
            return self.handlers[key]
        k2 = re.sub(r'\d+$', '{i}', key)
        return self.handlers.get(k2)

    def parse_kind(self, key):
        h = self.handler(key)
        if h is None:
            return None, None
        if re.search(r'\.toInt\(\)', h) and not re.search(r'\.to(Float|Double)\(\)', h):
            kind = 'int'
        elif re.search(r'\.to(Float|Double)\(\)', h):
            kind = 'float'
        else:
            kind = 'flag'
        clamp = None
        cm = re.search(r'constrain\(\s*(?:inputMessage|request->getParam\("\w+"\)->value\(\))\.to(?:Int|Float)\(\),\s*'
                       r'(-?[\d.]+)f?,\s*(-?[\d.]+)f?\s*\)', h)
        if cm:
            clamp = (float(cm.group(1)), float(cm.group(2)))
        return kind, clamp


# ---------------------------------------------------------------- keys file

def parse_keys(path):
    sections, sec, group = [], None, None
    with open(path, encoding='utf-8') as f:
        for ln, raw in enumerate(f, 1):
            line = raw.rstrip('\n')
            s = line.strip()
            if not s:
                continue
            m = re.match(r'^#\s*section:\s*(.+?)\s*$', s)
            if m:
                title = m.group(1)
                sec = {'id': re.sub(r'[^a-z0-9]+', '_', title.lower()).strip('_'), 'title': title, 'lines': []}
                sections.append(sec)
                group = None
                continue
            m = re.match(r'^#\s*group:\s*(.*?)\s*$', s)
            if m:
                group = m.group(1) or None
                continue
            if s.startswith('#'):
                continue
            if sec is None:
                raise CheckError('settings_keys.txt:%d: key before any "# section:" header' % ln)
            toks = s.split()
            opts = {}
            for t in toks[1:]:
                if '=' in t:
                    k, v = t.split('=', 1)
                    opts[k] = v
                else:
                    opts[t] = True
            sec['lines'].append({'head': toks[0], 'opts': opts, 'group': group, 'line': ln})
    return sections


# ---------------------------------------------------------------- resolver

class Gen:
    def __init__(self):
        self.errors = []
        self.html = Html(INDEX_HTML)
        self.js = Js(SCRIPT_JS)
        self.fw = Firmware(REPO)
        with open(STYLES_CSS, encoding='utf-8') as f:
            self.css = f.read()
        self.endpoints = {}
        self.pending_checks = []
        self.built = {}
        self.gate_uses = {}

    def err(self, ln, key, msg):
        self.errors.append('settings_keys.txt:%d %s: %s' % (ln, key, msg))

    # -- echo -------------------------------------------------------------
    def echo_for(self, key, ln):
        """-> (echo dict, transform info) or (None, None); errors recorded by caller."""
        ent = self.js.registry.get(key + '_echo')
        if not ent:
            # an echo span named otherwise (HardwarePresent_echo) that is the only one showing this field
            same = [v for v in self.js.registry.values() if v[0] == key]
            ent = same[0] if len(same) == 1 else None
        if ent:
            field, tr = ent
            info = parse_transform(tr)
            if info is None:
                self.err(ln, key, 'echo transform not reducible to a scale: %s' % tr)
                return None, None
            stream = self.js.stream_of(field)
            if stream is None:
                self.err(ln, key, 'echo field %s is in no CSV field list' % field)
                return None, None
            if stream == 'CSV3':
                fws = self.fw.csv3_scale.get(field)
                if fws is None:
                    self.err(ln, key, 'CSV3 field %s not in firmware CSV3_LIST' % field)
                    return None, None
                if info['scale'] % fws:
                    self.err(ln, key, 'echo scale %d does not fit firmware CSV3 scale %d of %s' % (info['scale'], fws, field))
                    return None, None
            return {'stream': stream, 'field': field, 'scale': info['scale']}, info
        e = self.http_echo(key, ln)
        if e:
            return e
        e = self.direct_echo(key)
        if e:
            return e
        return None, None

    def http_echo(self, key, ln):
        src = self.js.src
        lit = "'%s_echo'" % key
        for m in re.finditer(re.escape(lit), src):
            fn = self.js.enclosing_function(m.start())
            if not fn or fn == 'updateAllEchosOptimized':
                continue
            body = self.js.body(fn)
            ep = re.search(r"fetch\(buildURL\('(/\w+)'\)", body)
            if not ep:
                continue
            k = m.end()
            while src[k] in ' ,':
                k += 1
            depth, j = 0, k
            while j < len(src):
                c = src[j]
                if c in '\'"':
                    j = skip_string(src, j)
                    continue
                if c == '(':
                    depth += 1
                elif c == ')':
                    if depth == 0:
                        break
                    depth -= 1
                j += 1
            expr = src[k:j].strip()
            info = {'scale': 1, 'decimals': None, 'labels': None, 'temp': None, 'zeroOff': False}
            fm = re.search(r'\bj\.(\w+)', expr)
            if not fm:
                vm = re.match(r"^(\w+) \? '([^']*)' : '([^']*)'$", expr)
                if not vm:
                    self.err(ln, key, 'HTTP echo expression not understood: %s' % expr)
                    return None
                dm = re.search(r'const ' + vm.group(1) + r' = \(Number\(j\.(\w+)\) === 1\);', body)
                if not dm:
                    self.err(ln, key, 'HTTP echo flag %s not traced to a j.<field>' % vm.group(1))
                    return None
                field = dm.group(1)
                info['labels'] = [{'value': 0, 'label': vm.group(3)}, {'value': 1, 'label': vm.group(2)}]
            else:
                field = fm.group(1)
                inner = re.sub(r'\b(f1|f2)\(', '(', expr)
                dmatch = re.match(r'^(f1|f2)\(', expr)
                if dmatch:
                    info['decimals'] = 1 if dmatch.group(1) == 'f1' else 2
                if 'toDisplayTempDelta(' in inner:
                    info['temp'] = 'delta'
                elif 'toDisplayTemp(' in inner:
                    info['temp'] = 'abs'
                rest = re.sub(r'toDisplayTemp(Delta)?\(|\(|\)|\s', '', inner)
                if rest != 'j.' + field:
                    self.err(ln, key, 'HTTP echo expression carries arithmetic: %s' % expr)
                    return None
            name = ep.group(1).lstrip('/')
            self.add_endpoint(name, ep.group(1), fn)
            return {'stream': 'HTTP', 'endpoint': name, 'field': field, 'scale': 1}, info
        return None

    def direct_echo(self, key):
        """A same-name CSV3 field the app reads raw (data.KEY) -> firmware CSV3_LIST scale."""
        if key not in self.js.fields['CSV3'] or key not in self.fw.csv3_scale:
            return None
        if not re.search(r'\bdata\.' + key + r'\b', self.js.src):
            return None
        s = self.fw.csv3_scale[key]
        return {'stream': 'CSV3', 'field': key, 'scale': s}, {'scale': s, 'decimals': None, 'labels': None,
                                                              'temp': None, 'zeroOff': False}

    def add_endpoint(self, name, path, fetch_fn):
        poll = None
        m = re.search(r'pollWhenVisible\(' + fetch_fn + r",\s*'[^']*',\s*(\d+)\)", self.js.src)
        if m:
            poll = int(m.group(1)) / 1000.0
        if poll is None:
            m = re.search(fetch_fn + r'[^;\n]{0,80}\},\s*(\d+)\)', self.js.src)
            if m:
                poll = int(m.group(1)) / 1000.0
        self.endpoints[name] = {'path': path, 'pollS': poll if poll else 5}

    # -- per-item ---------------------------------------------------------
    def pick_control(self, key):
        ctrls = [c for c in self.html.named(key) if form_of(c) is not None]
        if not ctrls:
            return None
        for c in ctrls:
            r = row_of(c)
            if r is not None and any((n.get('id') or '') == key + '_echo' for n in r.walk()):
                return c
        for c in ctrls:
            if c.get('type') != 'hidden':
                return c
        return ctrls[0]

    def check_submit(self, form, key, ln, item):
        os_ = (form.get('onsubmit') or '').strip()
        if not os_:
            return True
        if any(h in os_ for h in SUBMIT_HANDLERS_PLAIN) or SUBMIT_REGEX_GUARD.match(os_):
            return True
        m = SUBMIT_TEMP.match(os_)
        if m and m.group(1) == key:
            item['temp'] = 'delta' if m.group(2) == 'true' else 'abs'
            if m.group(3) == 'true':
                item['zeroOff'] = True
            return True
        self.err(ln, key, 'form onsubmit "%s" is not known to leave the value alone' % os_)
        return False

    def fw_send(self, key, ln, item):
        kind, clamp = self.fw.parse_kind(key)
        if kind is None:
            self.err(ln, key, 'no /get handler for this key in the firmware')
            return
        item['send'] = {'scale': 1, 'integer': kind in ('int', 'flag')}
        if clamp and item.get('type') == 'number':
            lo, hi = clamp
            if item.get('min') is not None and lo > item['min']:
                item['min'] = num(repr(lo)) if lo != int(lo) else int(lo)
            if item.get('max') is not None and hi < item['max']:
                item['max'] = num(repr(hi)) if hi != int(hi) else int(hi)

    def arm_for(self, key):
        return key not in self.fw.keyless

    def apply_echo(self, item, key, ln, need):
        n_err = len(self.errors)
        echo, info = self.echo_for(key, ln)
        if echo is None:
            if need and len(self.errors) == n_err:
                self.err(ln, key, 'no echo found (registry %s_echo, an HTTP echo, or a raw CSV3 field)' % key)
            item['echo'] = None
            return None
        item['echo'] = echo
        if info.get('temp') and not item.get('temp'):
            item['temp'] = info['temp']
        if info.get('zeroOff'):
            item['zeroOff'] = info['zeroOff']
        return info

    def build_item(self, ent, sec):
        key, opts, ln = ent['head'], ent['opts'], ent['line']
        if key == 'table':
            return self.build_table(ent)
        if key.startswith('@'):
            return self.build_widget(ent)
        if key in self.fw.gx_denied:
            self.err(ln, key, 'firmware gxParamDenied refuses this key from a GX')
        if 'buttons' in opts:
            return self.build_button_select(ent)
        c = self.pick_control(key)
        if c is None:
            return self.build_js_action(ent)
        form = form_of(c)
        row = row_of(c)
        item = {'key': key}
        if c.get('type') == 'hidden':
            item['type'] = 'action'
            if c.get('value') != '1':
                self.err(ln, key, 'action form sends %s=%s; the bridge fires actions as =1' % (key, c.get('value')))
            btn = next((n for n in form.walk() if (n.tag == 'input' and n.get('type') == 'submit') or
                        (n.tag == 'button' and n.get('type', 'submit') == 'submit')), None)
            item['buttonText'] = clean(btn.get('value') if btn is not None and btn.tag == 'input' else
                                       (btn.text() if btn is not None else key))
            lab = label_parts(row, key)[0] if row is not None else None
            item['label'] = lab or item['buttonText']
            self.check_submit(form, key, ln, item)
            info = self.apply_echo(item, key, ln, need=False)
            if info and info.get('labels'):
                item['options'] = info['labels']
            self.action_confirm(item, form, btn, ln)
        else:
            lab, unit, temp_lbl = label_parts(row, key) if row is not None else (None, None, False)
            if not lab:
                self.err(ln, key, 'no form-row label found')
                return None
            item['label'] = lab
            if c.tag == 'select':
                item['type'] = 'select'
                item['options'] = [dict({'value': num(o.get('value')), 'label': clean(o.get('label') or o.text())},
                                        **({'disabled': True} if 'disabled' in o.attrs else {}))
                                   for o in c.walk() if o.tag == 'option']
                self.check_submit(form, key, ln, item)
                info = self.apply_echo(item, key, ln, need=True)
            elif self.toggle_checkbox(form, key):
                cb = self.toggle_checkbox(form, key)
                item['type'] = 'toggle'
                inverted = self.check_toggle_handler(cb, key, ln)
                info = self.apply_echo(item, key, ln, need=True)
                seg = next((n for n in form.walk() if 'ab-seg' in n.classes()), None)
                if seg is not None:
                    item['type'] = 'select'
                    opts_ = []
                    for b in seg.walk():
                        if b.tag == 'button' and b.get('data-checked') in ('0', '1'):
                            checked = b.get('data-checked') == '1'
                            wire = (0 if checked else 1) if inverted else (1 if checked else 0)
                            opts_.append({'value': wire, 'label': clean(b.text())})
                    item['options'] = sorted(opts_, key=lambda o: o['value'])
                elif info and info.get('labels'):
                    item['options'] = info['labels']
                else:
                    self.err(ln, key, 'toggle without option labels')
            else:
                item['type'] = 'number'
                item['min'], item['max'], item['step'] = num(c.get('min')), num(c.get('max')), num(c.get('step'))
                if unit:
                    item['unit'] = unit
                self.check_submit(form, key, ln, item)
                if temp_lbl and not item.get('temp'):
                    item['temp'] = 'delta' if c.get('data-tdelta') == '1' else 'abs'
                info = self.apply_echo(item, key, ln, need=True)
                dec = info.get('decimals') if info else None
                item['decimals'] = dec if dec is not None else step_decimals(item['step'])
                if item.get('temp') and not item.get('unit'):
                    item['unit'] = '°F'
            self.fw_send(key, ln, item)
            conf = opts.get('confirm')
            if conf:
                self.item_confirm(item, conf, ln)
        if 'send' not in item:
            kind, _ = self.fw.parse_kind(key)
            if kind is None:
                self.err(ln, key, 'no /get handler for this key in the firmware')
        item['arm'] = self.arm_for(key)
        if self.tagged(key):
            item['tagged'] = True
        item['help'] = tooltip_text(row)
        self.finish(item, ent, c)
        return item

    def finish(self, item, ent, node):
        opts = ent['opts']
        item['group'] = ent['group'] or (group_of(node) if node is not None else None)
        if ent['group'] and ent['group'] not in self.html.plain_text:
            self.err(ent['line'], item.get('key', ent['head']), 'group "%s" is not a heading in index.html' % ent['group'])
        if 'hidden' in opts:
            item['hidden'] = True
        if 'enabledWhen' in opts:
            src, v = opts['enabledWhen'].rsplit(':', 1)
            ep, field = src.split('.', 1)
            item['enabledWhen'] = {'endpoint': ep, 'field': field, 'value': int(v), '_line': ent['line']}
        if 'disabledWhen' in opts:
            k, v = opts['disabledWhen'].split(':')
            item['disabledWhen'] = {'key': k, 'value': int(v)}
            note = self.gate_note(node)
            if note:
                item['disabledWhen']['note'] = note
        if node is None or 'hidden' in opts:
            return
        gates = [self.gate_entry(g, node) for g in GATES
                 if any(self.in_container(a, g['container']) for a in node.ancestors())]
        if gates and item.get('type') in ('table', 'info'):
            self.err(ent['line'], item['key'], 'inside gate %s, which XregCfgSection.qml does not draw on a %s'
                     % (gates[0]['name'], item['type']))
        elif gates:
            item['gates'] = gates
            for g in gates:
                self.gate_uses.setdefault(g['name'], (ent['line'], item['key']))
        for sk, val, gname, _ in OPTION_GATES:
            if sk != item['key']:
                continue
            o = next((o for o in item.get('options') or [] if o['value'] == val), None)
            if o is None:
                self.err(ent['line'], sk, 'script.js disables option %s while %s, but the select has no such option'
                         % (val, gname))
                continue
            o['gates'] = [self.gate_entry(GATE_BY_NAME[gname], node)]
            self.gate_uses.setdefault(gname, (ent['line'], sk))

    @staticmethod
    def in_container(n, sel):
        return n.get('id') == sel[1:] if sel.startswith('#') else sel[1:] in n.classes()

    def gate_entry(self, gate, node):
        g = {'name': gate['name'], 'when': gate['when']}
        note = None
        if gate['kind'] == 'is-gated':    # .is-gated > .gate-note: the container's own note
            cont = next(a for a in node.ancestors() if self.in_container(a, gate['container']))
            n = next((c for c in cont.elements() if gate['note'] in c.classes()), None)
            note = clean(n.text()) if n is not None else None
        else:                             # body.<gate> shows every .gate-note-<x>; the row's is the nearest in its card
            for a in node.ancestors():
                n = next((x for x in a.walk() if gate['note'] in x.classes()), None)
                if n is not None:
                    note = clean(n.text())
                    break
                if 'settings-card' in a.classes():
                    break
        if note:
            g['note'] = note
        return g

    def gate_note(self, node):
        for a in (node.ancestors() if node is not None else []):
            for n in a.walk():
                if 'gate-note' in n.classes():
                    return clean(n.text())
        return None

    def tagged(self, key):
        return bool(re.search(r"'/get\?" + key + r"=' \+ [\w ]+\+ clientWriteTag\(\)", self.js.src))

    def toggle_checkbox(self, form, key):
        # <key>_checkbox, or a checkbox named otherwise whose handleUserToggle names key (HardwarePresent_checkbox)
        for n in form.walk():
            if n.tag == 'input' and n.get('type') == 'checkbox' and (
                    n.get('id') == key + '_checkbox' or clean(n.get('onchange') or '').startswith(
                        "if(handleUserToggle('%s', '%s', '%s'))" % (n.get('id'), key, key))):
                return n
        return None

    def check_toggle_handler(self, cb, key, ln):
        oc = clean(cb.get('onchange') or '')
        if oc.startswith("if(handleUserToggle('%s', '%s', '%s'))" % (cb.get('id'), key, key)):
            return False
        if oc.startswith("document.getElementById('%s').value = this.checked ? '1' : '0'" % key):
            return False
        if oc == 'manualFieldToggleChanged(this)' and key == 'ManualFieldToggle':
            body = self.js.body('handleManualFieldToggle') or ''
            if "const invertedVal = cb.checked ? '0' : '1';" not in body:
                self.err(ln, key, 'handleManualFieldToggle no longer inverts as expected')
            return True
        wrapped = "handleUserToggle('%s_checkbox', '%s', '%s')" % (key, key, key)
        if oc.startswith("const cb=this; validateAndSubmitBatteryToggle(cb, '%s')" % key) and wrapped in oc:
            return False
        fm = re.match(r'^(\w+)\(this\)$', oc)
        if fm:
            body = self.js.body(fm.group(1)) or ''
            if wrapped in body and not re.search(r"getElementById\('%s'\)\.value\s*=" % key, body):
                return False
        self.err(ln, key, 'checkbox onchange "%s" not known' % oc)
        return False

    def item_confirm(self, item, conf, ln):
        when = None
        if '@' in conf:
            conf, when = conf.split('@')
        kind, name = conf.split(':', 1)
        text = None
        if kind == 'fn':
            body = self.js.body(name)
            if body:
                rm = re.search(r'\breturn\s', body)
                text = literal_concat(body[rm.end():]) if rm else None
        elif kind == 'const':
            m = re.search(r'const\s+' + name + r'\s*=\s*', self.js.src)
            text = literal_concat(self.js.src[m.end():]) if m else None
        if not text:
            self.err(ln, item['key'], 'confirm text %s not found as string literals' % conf)
            return
        item['confirm'] = text
        if when is not None:
            item['confirmWhen'] = int(when)

    def fn_confirm(self, fn):
        body = self.js.body(fn) or ''
        m = re.search(r'xConfirm\(\s*', body)
        return literal_concat(body[m.end():]) if m else None

    def action_confirm(self, item, form, btn, ln):
        for src in (btn.get('onclick') if btn is not None else None, form.get('onsubmit')):
            fm = re.match(r'^(?:return\s+)?(?:await\s+)?(\w+)\(', (src or '').strip())
            if fm:
                t = self.fn_confirm(fm.group(1))
                if t:
                    item['confirm'] = t
                    return

    def build_js_action(self, ent):
        key, ln = ent['head'], ent['line']
        js = self.js.src
        hits = [m.start() for m in re.finditer(r"/get\?" + key + r"=1'|\{\s*" + key + r":\s*'1'\s*\}|\"" + key + r"\",\s*\"1\"", js)]
        fns = {self.js.enclosing_function(p) for p in hits} - {None}
        btns = [n for n in self.html.all if n.tag == 'button' and any(
            re.match(r'^\s*' + f + r'\(', n.get('onclick') or '') for f in fns)]
        if not btns:
            self.err(ln, key, 'not a form control in index.html and no button sends it')
            return None
        b = btns[0]
        fn = re.match(r'^\s*(\w+)\(', b.get('onclick')).group(1)
        row = row_of(b)
        item = {'key': key, 'type': 'action', 'buttonText': clean(b.text())}
        lab = label_parts(row, key)[0] if row is not None else None
        item['label'] = lab or item['buttonText']
        kind, _ = self.fw.parse_kind(key)
        if kind is None:
            self.err(ln, key, 'no /get handler for this key in the firmware')
        item['echo'] = None
        t = self.fn_confirm(fn)
        if t:
            item['confirm'] = t
        item['arm'] = self.arm_for(key)
        item['help'] = tooltip_text(row)
        self.finish(item, ent, b)
        return item

    def build_button_select(self, ent):
        key, opts, ln = ent['head'], ent['opts'], ent['line']
        item = {'key': key, 'type': 'select', 'options': []}
        first = None
        for bid in opts['buttons'].split(','):
            b = self.html.by_id.get(bid)
            if b is None:
                self.err(ln, key, 'button id %s not in index.html' % bid)
                return None
            first = first or b
            fm = re.match(r"^\s*(\w+)\('(\w+)'\)", b.get('onclick') or '')
            body = self.js.body(fm.group(1)) if fm else None
            vm = re.search(r"const desiredValue = \(mode === '(\w+)'\) \? (\d+) : (\d+);", body or '')
            if not (fm and vm):
                self.err(ln, key, 'button %s handler does not map its mode to a value' % bid)
                return None
            if not re.search(r"(params\.set\('%s'|/get\?%s=)" % (key, key), self.js.src):
                self.err(ln, key, 'no request in script.js sends %s' % key)
            v = int(vm.group(2)) if fm.group(2) == vm.group(1) else int(vm.group(3))
            item['options'].append({'value': v, 'label': clean(b.text())})
        item['options'].sort(key=lambda o: o['value'])
        lab = None
        par = first.parent
        sibs = par.parent.elements() if par is not None and par.parent is not None else []
        if par in sibs:
            i = sibs.index(par)
            if i > 0 and 'cap-mode-label' in sibs[i - 1].classes():
                lab = re.sub(r'\s*:\s*$', '', clean(sibs[i - 1].text()))
        if not lab:
            self.err(ln, key, 'no cap-mode-label before the buttons')
            return None
        item['label'] = lab
        self.apply_echo(item, key, ln, need=True)
        kind, _ = self.fw.parse_kind(key)
        if kind is None:
            self.err(ln, key, 'no /get handler for this key in the firmware')
        item['send'] = {'scale': 1, 'integer': True}
        item['arm'] = self.arm_for(key)
        item['help'] = ''
        self.finish(item, ent, first)
        return item

    def build_table(self, ent):
        opts, ln = ent['opts'], ent['line']
        toks = [k for k, v in opts.items() if v is True]
        if len(toks) != 2:
            self.err(ln, 'table', 'expected "table <tableId> <key in the form>"')
            return None
        tid, anchor = toks
        c = self.pick_control(anchor)
        if c is None:
            self.err(ln, tid, 'anchor key %s not in a form' % anchor)
            return None
        form = form_of(c)
        self.check_submit(form, anchor, ln, {})
        cols, include, seen = [], [], set()
        for n in form.walk():
            if n.tag != 'input' or not n.get('name') or n.get('type') == 'submit':
                continue
            name = n.get('name')
            if n.get('type') == 'hidden':
                include.append(name)
                continue
            m = re.match(r'^(\w*?[A-Za-z_])(\d+)$', name)
            pat = m.group(1) + '{i}' if m else name
            if pat in seen:
                continue
            seen.add(pat)
            cols.append((pat, n))
        rows = max(sum(1 for n in form.walk() if n.tag == 'input' and re.match('^' + re.escape(p.replace('{i}', '')) + r'\d+$', n.get('name') or ''))
                   for p, _ in cols if '{i}' in p) if any('{i}' in p for p, _ in cols) else 1
        table = form.parent
        heads = []
        for a in [form] + list(form.ancestors()):
            th = [h for h in a.walk() if h.tag == 'th']
            if th:
                heads = th
                break
        item = {'key': tid, 'type': 'table', 'tableId': tid, 'rows': rows, 'columns': [], 'label': None}
        for ci, (pat, n) in enumerate(cols):
            col = {'keyPattern': pat}
            if '{i}' in pat and len(heads) == len([p for p, _ in cols]):
                col['label'] = clean(heads[ci].text(is_tooltip))
                col['help'] = tooltip_text(heads[ci]) if any('tooltip-box' in x.classes() for x in heads[ci].walk()) else ''
                for x in heads[ci].walk():
                    if 'tooltip-box' in x.classes():
                        col['help'] = clean(x.text())
            else:
                col['label'] = clean(n.get('placeholder') or pat)
                col['help'] = ''
            lm = re.match(r'^(.*?)\s*\(([^()]+)\)$', col['label'])
            if lm and lm.group(2) in UNIT_WORDS:
                col['label'], col['unit'] = lm.group(1).strip(), lm.group(2)
            col['type'] = 'number'
            col['min'], col['max'], col['step'] = num(n.get('min')), num(n.get('max')), num(n.get('step'))
            echo = self.table_echo(pat, ln)
            col['echo'] = echo[0] if echo else None
            if echo is None and '{i}' in pat:
                self.err(ln, pat, 'no per-row echo found in script.js')
            if echo is None and '{i}' not in pat:
                e2 = self.row_echo(pat)
                col['echo'] = e2
                if e2 is None:
                    self.err(ln, pat, 'no echo found')
            dec = echo[1] if echo else None
            col['decimals'] = dec if dec is not None else step_decimals(col['step'])
            fk = pat.replace('{i}', '0')
            kind, clamp = self.fw.parse_kind(fk)
            if kind is None:
                self.err(ln, pat, 'no /get handler in the firmware')
                kind = 'float'
            col['send'] = {'scale': 1, 'integer': kind in ('int', 'flag')}
            if n.get('data-cap-col'):
                col['capCol'] = n.get('data-cap-col')
            item['columns'].append(col)
        item['include'] = include
        for k in include:
            if not self.fw.parse_kind(k)[0]:
                self.err(ln, k, 'table extra key has no /get handler in the firmware')
        row = row_of(c)
        item['label'] = opts.get('label') if isinstance(opts.get('label'), str) else None
        if not item['label']:
            sb = next((n for n in form.walk() if n.tag == 'input' and n.get('type') == 'submit'), None)
            item['label'] = clean(self.panel_title(form) or group_of(c) or (sb.get('value') if sb is not None else tid))
        sb = next((n for n in form.walk() if n.tag == 'input' and n.get('type') == 'submit'), None)
        item['buttonText'] = clean(sb.get('value')) if sb is not None else 'Save'
        if 'confirmIfChanged' in opts:
            t = self.submit_listener_confirm(form.get('id'))
            if not t:
                self.err(ln, tid, 'submit-listener confirm text not found for form %s' % form.get('id'))
            else:
                item['confirmIfChanged'] = {'keyPattern': opts['confirmIfChanged'] + '{i}', 'text': t}
        if 'readOnlyWhen' in opts:
            pat, k, v = opts['readOnlyWhen'].split(':')
            for col in item['columns']:
                if col['keyPattern'] == pat + '{i}':
                    col['readOnlyWhen'] = {'key': k, 'value': int(v), 'omit': True}
        if 'validate' in opts:
            rule, pat = opts['validate'].split(':')
            if rule != 'ascending' or pat + '{i}' not in [c['keyPattern'] for c in item['columns']]:
                self.err(ln, tid, 'validate=%s: only ascending:<column key prefix> is understood' % opts['validate'])
            elif not self.ascending_rule_present():
                self.err(ln, tid, 'validateLearningTable in script.js no longer checks strictly increasing values')
            item['validate'] = {'rule': rule, 'keyPattern': pat + '{i}'}
        if 'capMode' in opts:
            mode = self.built.get(opts['capMode'])
            kw = next((o['value'] for o in (mode or {}).get('options', []) if o['label'] == 'kW'), None)
            if kw is None:
                self.err(ln, tid, 'capMode=%s: no earlier select with a "kW" option' % opts['capMode'])
            elif not any(c.get('capCol') for c in item['columns']):
                self.err(ln, tid, 'capMode given but no column carries data-cap-col')
            else:
                item['capMode'] = {'key': opts['capMode'], 'kw': kw}
        item['arm'] = True
        item['help'] = tooltip_text(row)
        self.finish(item, ent, c)
        item['echo'] = None
        item['send'] = None
        return item

    def ascending_rule_present(self):
        body = self.js.body('validateLearningTable') or ''
        return 'rpmValues[i] <= rpmValues[i - 1]' in body and 'Zero RPM values are only allowed at the end' in body

    def panel_title(self, form):
        for a in form.ancestors():
            i = a.get('id') or ''
            if i.startswith('alt-panel-'):
                b = next((n for n in self.html.all if n.tag == 'button' and n.get('data-alt-panel') == i), None)
                return clean(b.text()) if b is not None else None
        return None

    def submit_listener_confirm(self, form_id):
        m = re.search(r"getElementById\('%s'\)\.addEventListener\('submit'" % re.escape(form_id or ''), self.js.src)
        if not m:
            return None
        b = self.js.src.find('{', m.end())
        e = match_close(self.js.src, b, '{', '}')
        body = self.js.src[b:e]
        xm = re.search(r'xConfirm\(\s*', body)
        return literal_concat(body[xm.end():]) if xm else None

    def table_echo(self, pat, ln):
        if '{i}' not in pat:
            return None
        prefix = pat.replace('{i}', '')
        src = self.js.src
        for am in re.finditer(r'const (\w+) = document\.getElementById\(`' + prefix + r'\$\{i\}_input`\);', src):
            v = am.group(1)
            nx = re.search(r'const ' + v + r' = ', src[am.end():])
            region = src[am.end():am.end() + (nx.start() if nx else 4000)]
            for m in re.finditer(r'\b' + v + r'\.value = (.+?);\n', region):
                expr = m.group(1)
                fm = re.search(r'data\[`(\w+?)\$\{i\}`\]', expr)
                if not fm:
                    continue
                field = fm.group(1)
                if field + '0' not in self.js.fields['CSV3']:
                    continue
                e = re.sub(r'data\[`\w+?\$\{i\}`\]', 'v', expr).replace(' ', '')
                for p, f in ((r'^v$', lambda m: (1, None)), (r'^Math\.round\(v/(\d+)\)$', lambda m: (int(m.group(1)), 0)),
                             (r'^\(v/(\d+)\)\.toFixed\((\d)\)$', lambda m: (int(m.group(1)), int(m.group(2))))):
                    mm = re.match(p, e)
                    if mm:
                        sc, dec = f(mm)
                        fws = self.fw.csv3_scale.get(field + '0')
                        if fws is None or sc % fws:
                            self.err(ln, pat, 'table echo scale %d does not fit firmware scale %s' % (sc, fws))
                        return {'stream': 'CSV3', 'field': field + '{i}', 'scale': sc}, dec
        return None

    def row_echo(self, name):
        m = re.search(r'const \w+ = data\.' + name + r' / (\d+);', self.js.src)
        if m:
            st = self.js.stream_of(name)
            if st:
                return {'stream': st, 'field': name, 'scale': int(m.group(1))}
        return None

    def build_widget(self, ent):
        name, opts, ln = ent['head'][1:], ent['opts'], ent['line']
        item = {'key': '@' + name, 'type': 'info', 'widget': name, 'label': None}
        src = opts.get('source')
        if src:
            if not re.search(r"buildURL\('%s'\)" % re.escape(src), self.js.src):
                self.err(ln, name, 'source %s is not fetched by script.js' % src)
            if not re.search(r'server\.on\("%s"' % re.escape(src), self.fw.src):
                self.err(ln, name, 'source %s is not served by the firmware' % src)
            ep = src.lstrip('/')
            if ep not in self.endpoints:
                fn = self.js.enclosing_function(self.js.src.find("buildURL('%s')" % src))
                self.add_endpoint(ep, src, fn or '')
            item['source'] = ep
        strings = {}
        for s in (opts.get('strings') or '').split(','):
            if not s:
                continue
            arr = js_string_array(self.js.src, s)
            if arr is None:
                self.err(ln, name, 'string array %s not in script.js' % s)
            else:
                strings[s] = arr
        if strings:
            item['strings'] = strings
        anchor = None
        if opts.get('anchor'):
            anchor = self.html.by_id.get(opts['anchor'])
            if anchor is None:
                self.err(ln, name, 'anchor id %s not in index.html' % opts['anchor'])
        if anchor is not None:
            row = row_of(anchor)
            item['help'] = tooltip_text(row) if row is not None else ''
            if anchor.tag == 'table':
                item['headings'] = [clean(th.text(is_tooltip)) for th in anchor.walk() if th.tag == 'th']
            lab = ''
            for a in anchor.ancestors():
                if a.tag == 'details':
                    copy = next((x for x in a.walk() if 'integ-copy' in x.classes()), None)
                    if copy is not None and not item['help']:
                        item['help'] = clean(copy.text())
                    break
                lbl = next((x for x in a.walk() if 'learning-info-label' in x.classes() and x is not anchor
                            and anchor not in list(x.walk())), None)
                if lbl is not None:
                    lab = clean(lbl.text(is_tooltip))
                    if not item['help']:
                        item['help'] = tooltip_text(a)
                    break
            item['label'] = lab
        else:
            item['help'] = ''
        if item['label'] is None:
            item['label'] = ''
        item['arm'] = True
        item['echo'] = None
        self.finish(item, ent, anchor)
        return item

    def check_gates(self):
        """Each gate must still be in the app as GATES describes it, and the GX needs every key a used gate reads."""
        js, css = ' '.join(self.js.src.split()), ' '.join(self.css.split())
        for gate in GATES:
            name, sel = gate['name'], gate['container']
            if not any(self.in_container(n, sel) for n in self.html.all):
                self.errors.append('gate %s: no %s in index.html' % (name, sel))
            if gate['kind'] == 'body':
                rules = ('body.%s %s' % (name, sel), 'body.%s .%s' % (name, gate['note']))
            else:
                rules = ('.is-gated > *:not(.gate-note)', '.is-gated > .%s' % gate['note'])
            for r in rules:
                if r not in css:
                    self.errors.append('gate %s: styles.css no longer has a "%s" rule' % (name, r))
            lines = list(gate['js']) + [l for sk, v, g, ls in OPTION_GATES if g == name for l in ls]
            for line in lines:
                if ' '.join(line.split()) not in js:
                    self.errors.append('gate %s: script.js no longer has: %s' % (name, line))
            if name not in self.gate_uses:
                continue
            ln, user = self.gate_uses[name]
            for k in cond_keys(gate['when']):
                g = self.built.get(k)
                if g is None:
                    self.err(ln, user, 'gate %s reads %s, which is not in settings_keys.txt; list it as "%s hidden"'
                             % (name, k, k))
                elif g.get('type') in ('action', 'table', 'info') or not g.get('echo'):
                    self.err(ln, user, 'gate %s reads %s, which is not a setting with an echo' % (name, k))

    def run(self):
        secs = parse_keys(KEYS_FILE)
        out = []
        keys_seen = set()
        for s in secs:
            items = []
            grouped = False
            for ent in s['lines']:
                it = self.build_item(ent, s)
                if it is None:
                    continue
                if it['key'] in keys_seen:
                    self.err(ent['line'], it['key'], 'listed twice')
                keys_seen.add(it['key'])
                self.built[it['key']] = it
                ew = it.get('enabledWhen')
                if ew:
                    ln = ew.pop('_line')
                    self.pending_checks.append((ln, it['key'], ew))
                if not it.get('hidden'):
                    if it.get('group'):
                        grouped = True
                    elif grouped:
                        self.err(ent['line'], it['key'], 'ungrouped after a grouped key; add a "# group:" line')
                items.append(order(it))
            out.append({'id': s['id'], 'title': s['title'], 'items': items})
        for ln, key, ew in self.pending_checks:
            if ew['endpoint'] not in self.endpoints:
                self.err(ln, key, 'enabledWhen endpoint %s is not polled by any item' % ew['endpoint'])
            if not re.search(r'\bj\.' + ew['field'] + r'\b', self.js.src):
                self.err(ln, key, 'enabledWhen field %s is never read from a JSON reply in script.js' % ew['field'])
        # The GX greys from /Settings/Value/<K>, which the bridge publishes only for listed keys with an echo.
        for s in secs:
            for ent in s['lines']:
                for opt, pos in (('disabledWhen', 0), ('readOnlyWhen', 1)):
                    dw = ent['opts'].get(opt)
                    if isinstance(dw, str) and len(dw.split(':')) > pos:
                        gk = dw.split(':')[pos]
                        g = self.built.get(gk)
                        if g is None or g.get('type') in ('action', 'table', 'info') or not g.get('echo'):
                            self.err(ent['line'], ent['head'], '%s key %s is not a listed setting with an echo' % (opt, gk))
        self.check_gates()
        hashes = []
        for p in (INDEX_HTML, SCRIPT_JS, KEYS_FILE):
            with open(p, 'rb') as f:
                hashes.append('%s:%s' % (os.path.relpath(p, REPO), hashlib.sha256(f.read()).hexdigest()[:12]))
        return {'version': 1, 'generatedFrom': ' '.join(hashes), 'endpoints': self.endpoints, 'sections': out}


def cond_keys(c):
    if 'any' in c or 'all' in c:
        return [k for x in c.get('any') or c.get('all') for k in cond_keys(x)]
    return [c['key']]


ORDER = ['key', 'label', 'type', 'group', 'options', 'min', 'max', 'step', 'decimals', 'unit', 'temp', 'zeroOff',
         'echo', 'send', 'arm', 'tagged', 'endpoint', 'method', 'buttonText', 'confirm', 'confirmWhen', 'disabledWhen',
         'gates', 'enabledWhen', 'hidden', 'tableId', 'rows', 'columns', 'include', 'capMode', 'confirmIfChanged', 'validate',
         'widget',
         'source', 'headings', 'strings', 'help']


def order(d):
    out = {k: d[k] for k in ORDER if k in d and d[k] is not None}
    for k in d:
        if k not in out and d[k] is not None:
            out[k] = d[k]
    if 'columns' in out:
        out['columns'] = [order(c) for c in out['columns']]
    return out


def dump(schema):
    return json.dumps(schema, ensure_ascii=False, indent=1) + '\n'


def main():
    check = '--check' in sys.argv[1:]
    try:
        g = Gen()
        schema = g.run()
    except CheckError as e:
        print('gen_settings: FAIL: %s' % e)
        return 1
    if g.errors:
        print('gen_settings: FAIL: %d unresolved item(s):' % len(g.errors))
        for e in g.errors:
            print('  ' + e)
        return 1
    text = dump(schema)
    n_items = sum(len(s['items']) for s in schema['sections'])
    if check:
        try:
            with open(SCHEMA_FILE, encoding='utf-8') as f:
                disk = json.load(f)
        except (OSError, ValueError):
            disk = None
        fresh = json.loads(text)
        if disk is not None:
            disk.pop('generatedFrom', None)
        fresh.pop('generatedFrom', None)
        if disk != fresh:
            print('gen_settings: FAIL: %s is stale or missing; run: python3 gx_app/gen_settings.py'
                  % os.path.relpath(SCHEMA_FILE, REPO))
            return 1
        print('gen_settings: OK: %d sections, %d items, schema up to date' % (len(schema['sections']), n_items))
        return 0
    with open(SCHEMA_FILE, 'w', encoding='utf-8') as f:
        f.write(text)
    print('gen_settings: wrote %s (%d sections, %d items, %d bytes)'
          % (os.path.relpath(SCHEMA_FILE, REPO), len(schema['sections']), n_items, len(text.encode('utf-8'))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
