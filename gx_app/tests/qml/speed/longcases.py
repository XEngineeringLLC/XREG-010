# Long Term data sets for long_test: python3 longcases.py <out.json>   (each set's file beside it, in a cases/ folder)
import sys, json, subprocess, os
here = os.path.dirname(os.path.abspath(__file__))
tmp = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), 'cases')
os.makedirs(tmp, exist_ok=True)
def gen(seed, rng):
    p = os.path.join(tmp, 'case_%d_%s.json' % (seed, rng))
    subprocess.check_call([sys.executable, '-B', os.path.join(here, 'longdata.py'), p, str(seed), rng], stdout=subprocess.DEVNULL)
    return json.load(open(p))
cases = []
a = gen(11, '30d'); cases.append({'name': '30d, 300 points', 'data': a})
b = gen(12, '24h'); cases.append({'name': '24h, a new range on the open page', 'data': b})
cases.append({'name': 'the same 24h data again', 'data': b})
c = dict(b); c['Plot/Long/altTempMin'] = ''; c['Plot/Long/battCurrOnAvg'] = ''; c['Plot/Long/sog'] = ''
c['Plot/Long/Fields'] = ','.join(f for f in b['Plot/Long/Fields'].split(',') if f != 'soc')
cases.append({'name': 'series missing (altTempMin, battCurrOnAvg, sog) and soc not in Fields', 'data': c})
d = gen(13, '1h'); cases.append({'name': '1h', 'data': d})
e = dict(d)
for k in list(e):
    if k.startswith('Plot/Long/altTemp'):
        e[k.replace('altTemp', 'altTempC')] = e.pop(k)
e['Plot/Long/Fields'] = e['Plot/Long/Fields'].replace('altTemp', 'altTempC')
cases.append({'name': 'a temperature published with a C suffix (altTempC)', 'data': e})
f = dict(d); f['Plot/Long/Last'] = '{bad'; f['Plot/Long/chargeStage'] = ''
cases.append({'name': 'Last malformed, no charge stages', 'data': f})
g = dict(d); g['Plot/Long/Last'] = ''
cases.append({'name': 'Last empty', 'data': g})
h = dict(d); h['Plot/Long/StepS'] = 0
cases.append({'name': 'step 0 (no data)', 'data': h})
i = dict(d); i['Plot/Long/Fields'] = ''
cases.append({'name': 'no Fields', 'data': i})
j = gen(14, '7d'); cases.append({'name': '7d after no data', 'data': j})
k = dict(j); k['Plot/Long/battVolt'] = j['Plot/Long/battVolt'].replace(',', ',,', 3)
cases.append({'name': 'one series longer than the rest (n from the longest)', 'data': k})
l = gen(15, '6h'); cases.append({'name': '6h', 'data': l})
json.dump(cases, open(sys.argv[1], 'w'))
print(len(cases), 'cases')
