# Scenario /Flow/Json payloads for the offline tile renders, built by the bridge's own flow_model (no hand-made JSON).
import json, os, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'xregulator', 'bridge'))
import xreg_core as xc

UID = '0000FEEDC0DE1234'
UID_B = '00005EEDC0DE5678'
BATT = 'com.victronenergy.battery.ttyO1'


def dev(cls, key, amps, name='', product='', mode=None, pv=None, in_amps=None, serial=''):
    return {'service': 'com.victronenergy.%s.%s' % (cls, key.replace(':', '_')), 'cls': cls, 'key': key, 'name': name,
            'product': product, 'serial': serial, 'mode': mode, 'amps': amps, 'pv': pv, 'age': 0.3, 'in_amps': in_amps,
            'in_age': 0.3 if in_amps is not None else None}


CHARGER_MEM = {'charger:280': {'k': 'charger', 'n': '', 'p': 'Phoenix Smart IP43 Charger 12/30', 'r': 's'}}


def build(reg, devices=(), memory=None, batt_gx=None, pv=None, peers=(), lim=None, fs=120.0, st=None):
    gx = {'batt': batt_gx, 'batt_service': BATT, 'pv': pv, 'devices': list(devices), 'at': 0}
    devs = xc.gx_devices(gx, memory or {}, UID, [UID] + [p['uid'] for p in peers if p['uid'] != UID], peers)
    m = xc.flow_model(reg, gx, devs, UID, [UID], memory or {})
    b = m['batt']
    if st is None:
        st = None if b is None else 1 if b > 0.3 else -1 if b < -0.3 else 0
    r2 = xc._r2
    return {'b': {'a': r2(b), 'st': st, 'from': m['batt_from'], 'lim': lim}, 'rows': [dict(r, a=r2(r['a'])) for r in m['rows']],
            'house': r2(m['house']), 'hm': m['hm'], 'why': m['why'], 'x': r2(m['x']), 'fs': fs}


def reg(alt, solar, batt, src=11):
    return {'alt': alt, 'solar': solar, 'batt': batt, 'src': src, 'ser': 0}


S = {}
S['motoring'] = (build(reg(60.0, 8.0, 54.0), memory=CHARGER_MEM), True, 62)
S['anchor'] = (build(reg(0.0, 18.0, 9.0), memory=CHARGER_MEM), False, None)
S['shore'] = (build(reg(0.0, 3.0, 22.0), [dev('charger', 'charger:280', 25.0, '', 'Phoenix Smart IP43 Charger 12/30')]),
              False, None)
S['evening'] = (build(reg(0.0, 0.0, -11.0), memory=CHARGER_MEM), False, None)
S['resting'] = (build(reg(0.0, 0.2, 0.1), memory=CHARGER_MEM), False, None)
S['limit'] = (build(reg(56.0, 8.0, 50.0), memory=CHARGER_MEM, lim=50.0), True, 56)
S['noshunt'] = (build(reg(60.0, 8.0, None, src=1), memory=CHARGER_MEM), True, 62)
S['nosolar'] = (build(reg(40.0, None, 30.0, src=13), memory=CHARGER_MEM), True, 42)
S['noalt'] = (build(reg(None, 6.0, 39.0), memory=CHARGER_MEM), True, 62)
S['unmeasured'] = (build(reg(0.0, 10.0, 16.0), memory=CHARGER_MEM), False, None)
multi = dev('vebus', 'vebus:276', -12.0, '', 'MultiPlus-II 12/3000/120-32')
gen = dev('dcgenset', 'dcgenset:290', 0.0, '', 'Fischer Panda AGT-DC')
S['full'] = (build(reg(60.0, 8.0, 40.0), [multi, gen], memory=CHARGER_MEM), True, 62)
peers = [{'uid': UID, 'name': 'Port engine', 'amps': 60.0, 'live': True},
         {'uid': UID_B, 'name': 'Stbd engine', 'amps': 45.0, 'live': True}]
many = [dev('charger', 'charger:280', 0.0, '', 'Phoenix Smart IP43 Charger 12/30'), multi, gen,
        dev('fuelcell', 'fuelcell:294', 3.5, '', 'EFOY Comfort 210'),
        dev('dcsource', 'dcsource:289', 7.5, 'Wind charger', 'SmartShunt IP65 500A/50mV', mode=-8),
        dev('dcdc', 'dcdc:293', 18.0, 'Orion XS Charger (DCDC)', 'Orion XS 12/12-50A Charger')]
S['many'] = (build(reg(60.0, 8.0, 110.0), many, peers=peers), True, 62)
S['overfull'] = (build(reg(0.0, 3.0, 125.0), [dev('charger', 'charger:280', 140.0, 'Dock charger', 'Skylla-i 12/100')]),
                 False, None)
S['twochargers'] = (build(reg(0.0, 3.0, 30.0), [dev('charger', 'charger:280', 25.0, 'Bow', 'Phoenix Smart IP43 Charger 12/30'),
                                                 dev('charger', 'charger:282', 12.0, 'Stern', 'Blue Smart IP22 12/15')]),
                    False, None)
# the Orion both ways, measured DC loads, both in one picture (2026-10-28 Orion switch and DC loads)
ORION = dev('dcdc', 'dcdc:293', 20.0, 'Start charger', 'Orion XS 12/12-50A Charger', in_amps=23.0)
ORION_MEM = {'dcdc:293': {'k': 'dcdc', 'n': 'Start charger', 'p': 'Orion XS 12/12-50A Charger', 'r': 's'}}
DRAWS = {'dcdc:293': dict(ORION_MEM['dcdc:293'], h=1)}
FRIDGE = dev('dcload', 'dcload:279', -6.0, 'Fridge', 'SmartShunt 500A/50mV')
PUMP = dev('dcload', 'dcload:283', -3.5, 'Watermaker', 'SmartShunt 300A/50mV')
S['orion_src'] = (build(reg(60.0, 8.0, 54.0), [ORION], memory=dict(CHARGER_MEM, **ORION_MEM)), True, 62)
S['orion_load'] = (build(reg(60.0, 8.0, 21.0), [ORION], memory=dict(CHARGER_MEM, **DRAWS)), True, 62)
S['dcloads'] = (build(reg(0.0, 18.0, 4.0), [FRIDGE, PUMP], memory=CHARGER_MEM), False, None)
S['loads_orion'] = (build(reg(60.0, 8.0, 18.0), [ORION, FRIDGE, PUMP], memory=dict(CHARGER_MEM, **DRAWS)), True, 62)
S['loads_orion_inv'] = (build(reg(60.0, 8.0, 10.0), [ORION, FRIDGE, dev('inverter', 'inverter:281', -9.0, 'Galley',
                                                                         'Phoenix Inverter 12/800 Smart')],
                              memory=DRAWS), True, 62)
S['loads_over'] = (build(reg(0.0, 12.0, -14.0), [dev('dcload', 'dcload:284', -40.0, 'Windlass', 'SmartShunt 500A/50mV')]),
                   False, None)
# the footnote's sum with each kind of measured load, at 4 rows (two-line on 5-inch)
S['sum_inv'] = (build(reg(50.0, None, 30.0, src=10), [dev('charger', 'charger:280', 12.0, '', 'Phoenix Smart IP43 Charger 12/30'),
                                                     dev('inverter', 'inverter:281', -9.0, 'Galley', 'Phoenix Inverter 12/800 Smart')]),
                True, 52)
S['sum_dcload'] = (build(reg(50.0, None, 30.0, src=10), [dev('charger', 'charger:280', 12.0, '', 'Phoenix Smart IP43 Charger 12/30'),
                                                        FRIDGE]), True, 52)
S['sum_orion'] = (build(reg(50.0, None, 20.0, src=10), [ORION, FRIDGE], memory=DRAWS), True, 52)
# the 5-inch cut-over, one row at a time: alternator and House, then a source more each step
ladder = [dev('charger', 'charger:280', 12.0, '', 'Phoenix Smart IP43 Charger 12/30'), FRIDGE, ORION,
          dev('dcsource', 'dcsource:289', 4.0, 'Wind charger', 'SmartShunt IP65 500A/50mV', mode=-8)]
for n in range(2, 7):
    batt = 40.0 if n <= 4 else 20.0
    S['rows%d' % n] = (build(reg(50.0, None, batt, src=10), ladder[:n - 2], memory=DRAWS if n >= 5 else {},
                             lim=batt if n in (4, 5) else None), True, 52)
# demo: the made-up GX around a simulated motoring boat
f = xc.FakeGx(7)
# Agent A's FakeGx (2026-10-29): the shore charger, fridge and Orion come from the simulation (CSV4 demoShoreA,
# demoFridgeA, demoOrionA); a motoring boat just after an engine start, its Orion charging the start battery
for k in range(680):
    g = f.step(1000.0 + k * 0.5, {'alt': 48.0, 'solar': 21.0, 'batt': 48.0 + 21.0 - 15.0 - 4.2 - 12.0, 'shore': 0.0,
                                  'fridge': 4.2, 'orion': 12.0}, heading=200.0)
devs = xc.gx_devices(g, None, UID, [UID])
m = xc.flow_model({'alt': 48.0, 'batt': None, 'solar': None, 'src': 3, 'ser': 0}, g, devs, UID, [UID])
S['demo'] = ({'b': {'a': xc._r2(m['batt']), 'st': 1, 'from': m['batt_from'], 'lim': None},
              'rows': [dict(r, a=xc._r2(r['a'])) for r in m['rows']], 'house': xc._r2(m['house']), 'hm': m['hm'],
              'why': m['why'],
              'x': xc._r2(m['x']), 'fs': 120.0}, True, 50)

# 2026-10-29: every footnote reason at every kind (the long form where it fits, else shorter), and the DC system meter
WHY = {'charger': dev('charger', 'charger:280', None, '', 'Phoenix Smart IP43 Charger 12/30'),
       'multi': dev('vebus', 'vebus:276', None, '', 'MultiPlus-II 12/3000/120-32'),
       'dcgenset': dev('dcgenset', 'dcgenset:290', None, '', 'Fischer Panda AGT-DC'),
       'fuelcell': dev('fuelcell', 'fuelcell:294', None, '', 'EFOY Comfort 210'),
       'altx': dev('alternator', 'alternator:288', None, 'Stbd engine', 'WS500', serial='1F2E3D4C'),
       'dcdc': dev('dcdc', 'dcdc:293', None, 'Start charger', 'Orion XS 12/12-50A Charger'),
       'wind': dev('dcsource', 'dcsource:289', None, '', 'SmartShunt IP65 500A/50mV', mode=-8),
       'water': dev('dcsource', 'dcsource:291', None, '', 'SmartShunt 500A/50mV', mode=-4),
       'shaft': dev('dcsource', 'dcsource:292', None, '', 'SmartShunt 500A/50mV', mode=-7),
       'source': dev('dcsource', 'dcsource:295', None, '', 'SmartShunt 300A/50mV', mode=-1),
       'inverter': dev('inverter', 'inverter:281', None, 'Galley', 'Phoenix Inverter 12/800 Smart'),
       'dcdcload': dev('dcdc', 'dcdc:293', None, 'Start charger', 'Orion XS 12/12-50A Charger'),
       'dcload': dev('dcload', 'dcload:279', None, '', 'SmartShunt 500A/50mV', mode=3)}
S['why_alt'] = (build(reg(None, None, 30.0, src=10), [dev('charger', 'charger:280', 12.0, '', 'Phoenix Smart IP43 Charger 12/30')]), False, None)
S['why_solar'] = (build(reg(50.0, None, 30.0, src=13)), True, 52)
for k, d in WHY.items():
    S['why_' + k] = (build(reg(50.0, None, 30.0, src=10), [d], memory=DRAWS if k == 'dcdcload' else None), True, 52)
S['why_batt'] = (build(reg(50.0, None, None, src=10), [dev('charger', 'charger:280', 12.0, '', 'Phoenix Smart IP43 Charger 12/30')]), True, 52)
S['why_unmeasured_big'] = (build(reg(0.0, 2.0, 125.4, src=11), [dev('charger', 'charger:280', 0.0, '', 'Phoenix Smart IP43 Charger 12/30')]), False, None)
METER = dev('dcsystem', 'dcsystem:300', -30.0, 'House panel', 'SmartShunt 500A/50mV')
S['meter_agree'] = (build(reg(60.0, 22.0, 52.0), [dict(METER, amps=-30.4)]), True, 62)
S['meter_gap'] = (build(reg(60.0, 22.0, 52.0), [dict(METER, amps=-18.0)]), True, 62)
S['meter_gap_big'] = (build(reg(140.0, 22.0, 20.0), [dict(METER, amps=-18.6)]), True, 142)
S['meter_source'] = (build(reg(60.0, 22.0, 52.0), [dict(METER, amps=-41.0)]), True, 62)
S['meter_inside'] = (build(reg(60.0, 8.0, 18.0), [dict(METER, amps=-50.0), ORION, FRIDGE], memory=DRAWS), True, 62)
S['meter_inside_one'] = (build(reg(60.0, 8.0, 32.0), [dict(METER, amps=-36.0), FRIDGE]), False, None)
S['meter_nobatt'] = (build(reg(60.0, 8.0, None, src=1), [METER]), True, 62)
# a SmartShunt in DC-DC mode drawing (a load by its sign); DC loads named by their monitor mode, one in inverter mode
S['dcdc_meter_load'] = (build(reg(60.0, 8.0, 40.0), [dev('dcsource', 'dcsource:301', -12.0, '', 'SmartShunt 300A/50mV', mode=-3)]), True, 62)
S['dcload_modes'] = (build(reg(0.0, 18.0, 2.0), [dev('dcload', 'dcload:310', -3.0, '', 'SmartShunt 500A/50mV', mode=4),
                                                 dev('dcload', 'dcload:311', -4.0, '', 'SmartShunt 500A/50mV', mode=3),
                                                 dev('dcload', 'dcload:312', -6.0, '', 'SmartShunt 500A/50mV', mode=7)]), False, None)
# the demo's Orion drawing from the house bank (marked "h" by FakeGx) and its Fridge, as FakeGx hands them over
DEMO_ORION = dict(dev('dcdc', 'dcdc:282', 14.0, 'Start battery charger', 'Orion XS 12/12-50A', in_amps=15.6), h=1)
DEMO_FRIDGE = dev('dcload', 'dcload:283', -4.2, 'Fridge', 'SmartShunt 500A/50mV', mode=3)
S['demo_orion_fridge'] = (build(reg(48.0, 21.0, 30.0), [DEMO_ORION, DEMO_FRIDGE]), True, 50)

# 2026-10-29, approved proposals: the owner's word on what passes through the DC system meter
ALT_IN = {'xreg:' + UID: {'k': 'reg', 'm': 1}}
S['meter_alt_in'] = (build(reg(60.0, 8.0, 30.0), [dict(METER, amps=22.0)], memory=ALT_IN), False, None)
S['meter_alt_in_loads'] = (build(reg(60.0, 8.0, 24.0), [dict(METER, amps=16.0), FRIDGE],
                                 memory=dict(ALT_IN, **{'dcload:279': {'k': 'dcload', 'n': 'Fridge', 'p': 'S', 'r': 'l', 'm': 1}})),
                           False, None)
S['meter_load_out'] = (build(reg(60.0, 8.0, 30.0), [dict(METER, amps=-32.0), FRIDGE],
                             memory={'dcload:279': {'k': 'dcload', 'n': 'Fridge', 'p': 'S', 'r': 'l', 'm': 0}}), False, None)
out = {k: {'json': json.dumps(v[0], separators=(',', ':')), 'active': v[1], 'target': v[2]} for k, v in S.items()}
json.dump(out, open(sys.argv[1], 'w'), indent=1)
for k, v in S.items():
    print(k, [(r['k'], r['a']) for r in v[0]['rows']], 'house', v[0]['house'], v[0]['why'])
