#!/usr/bin/env python3
# GX app 2026-10-28: the power flow with the GX's own devices (one row per device, kinds, MPPT de-duplication by serial,
# remembered devices and forgetting them, the other pinned regulators), the demo's made-up GX (FakeGx), the D-Bus reader
# (GxReader), the device readings posted to the regulator (/gxDevices), the wind header after a demo switch and the perf
# settings the plugin's curve needs. Uses test_bridge.py's fake regulator and harness. Run: python3 -B test_flow_merge.py
import gzip, hashlib, json, math, os, re, sys, time, types, unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
SUITE = HERE
sys.path.insert(0, SUITE)
import test_bridge as tb   # imported as a module, so its own TestCases do not run here
xc, fr, Harness, Store = tb.xc, tb.fr, tb.Harness, tb.Store

UID_B = tb.UID_B
EMPTY = -2000000000
BATT = 'com.victronenergy.battery.ttyO1'


def dev(cls, key, amps, name='', product='', serial='', mode=None, pv=None, svc=None, age=0.3):
    return {'service': svc or 'com.victronenergy.%s.%s' % (cls, key.replace(':', '_')), 'cls': cls, 'key': key,
            'name': name, 'product': product, 'serial': serial, 'mode': mode, 'amps': amps, 'pv': pv, 'age': age}


def gx(batt=None, batt_service=BATT, pv=None, devices=(), at=None):
    return {'batt': batt, 'batt_service': batt_service, 'pv': pv, 'devices': [dict(d) for d in devices],
            'at': time.monotonic() if at is None else at}


def reg(batt=50.0, alt=68.0, solar=22.0, src=11, ser=0):
    return {'batt': batt, 'alt': alt, 'solar': solar, 'src': src, 'ser': ser}


def model(r, g, memory=None, uid=None, uids=(), peers=()):
    uid = uid or fr.UID
    devs = xc.gx_devices(g, memory, uid, uids, peers)
    return xc.flow_model(r, g, devs, uid, uids), devs


def rows(m):
    return {x['id']: x for x in m['rows']}


MPPT_A = dev('solarcharger', 'solarcharger:278', 15.1, 'Bimini', 'SmartSolar MPPT 100/30', 'HQ2203ABCDE',
             svc='com.victronenergy.solarcharger.socketcan_can1_vi0_uc1')
MPPT_B = dev('solarcharger', 'solarcharger:279', 9.9, '', 'SmartSolar MPPT 75/15', 'HQ1929X7KQZ',
             svc='com.victronenergy.solarcharger.ttyS3')
CHARGER = dev('charger', 'charger:280', 20.0, '', 'Phoenix Smart IP43 Charger 12/30')


class TestSerialHash(unittest.TestCase):
    def test_matches_firmware(self):
        # values printed by the loop in 5_functions.ino ReadVEData, compiled natively (lanes/gx_1028/serhash.c)
        self.assertEqual([xc.ser_hash(s) for s in ('HQ2203ABCDE', 'HQ1929X7KQZ', ' HQ2241DMO1A\r')],
                         [195406954, 362715510, 1044371889])
        self.assertTrue(all(0 < xc.ser_hash('HQ%07d' % i) < 2 ** 31 for i in range(2000)))


class TestModel(unittest.TestCase):
    def test_regulator_wins_battery(self):
        m, _ = model(reg(), gx(batt=31.0))
        self.assertEqual((m['batt'], m['batt_from']), (50.0, 'Shunt'))
        for src, name in ((21, 'NMEA 2000'), (31, 'VE.Direct')):
            self.assertEqual(model(reg(src=src), gx(batt=31.0))[0]['batt_from'], name)

    def test_gx_battery_fills(self):
        m, _ = model(reg(batt=None, src=1), gx(batt=31.0))
        self.assertEqual((m['batt'], m['batt_from'], m['house'], m['why']), (31.0, 'GX', 59.0, ''))

    def test_gx_battery_only_from_a_battery_monitor(self):
        # systemcalc estimates a battery current from a Multi and the solar chargers when no monitor is selected, which
        # leaves every alternator out; the demo battery service is this bridge's own; no service: no monitor
        for svc in ('com.victronenergy.vebus.ttyO1', 'com.victronenergy.multi.socketcan_can1_vi0_uc1', '',
                    xc.batt_service(fr.UID)):
            with self.subTest(service=svc):
                m, _ = model(reg(batt=None, src=1), gx(batt=31.0, batt_service=svc), uids=[fr.UID])
                self.assertEqual((m['batt'], m['why'], m['batt_from'], m['house']), (None, 'batt', '', None))

    def test_mppt_on_both_counted_once(self):
        # a VE.Can SmartSolar wired to the regulator's VE.Direct port and to the GX, plus one on the GX only
        m, _ = model(reg(solar=15.0, ser=xc.ser_hash('HQ2203ABCDE')), gx(pv=25.0, devices=[MPPT_A, MPPT_B]))
        r = rows(m)
        self.assertAlmostEqual(r['solar']['a'], 15.0 + 25.0 - 15.1)
        self.assertEqual(r['solar']['c'], 'VE.Direct + GX')
        self.assertAlmostEqual(m['house'], 68.0 + 24.9 - 50.0)
        # only the regulator's own MPPT on the GX: nothing to add; the MPPTs are never rows of their own
        m, _ = model(reg(solar=15.0, ser=xc.ser_hash('HQ2203ABCDE')), gx(pv=15.1, devices=[MPPT_A]))
        self.assertEqual(([x['id'] for x in m['rows']], rows(m)['solar']['c'], rows(m)['solar']['a']),
                         (['alt', 'solar'], 'VE.Direct', 15.0))

    def test_without_the_serial_the_regulators_mppt_stands_alone(self):
        # firmware before flowSolarSer (32 CSV4 fields), or a port with no SER# yet: no double count, ever
        for r in (reg(solar=15.0, ser=0), {k: v for k, v in reg(solar=15.0).items() if k != 'ser'}):
            m, _ = model(r, gx(pv=15.1, devices=[MPPT_A]))
            self.assertEqual((rows(m)['solar']['a'], rows(m)['solar']['c']), (15.0, 'VE.Direct'))

    def test_has_solar(self):
        # SolarWatts 0 (none configured, reads 0 A) and an array the regulator cannot see: the GX's MPPT fills
        for src, solar in ((10, 0.0), (13, None)):
            m, _ = model(reg(solar=solar, src=src), gx(pv=12.0, devices=[dict(MPPT_B, amps=12.0)]))
            self.assertEqual((rows(m)['solar']['a'], rows(m)['solar']['c'], m['why']), (12.0, 'GX', ''))
        # an array set and nothing reads it: Solar not measured, House unknown
        m, _ = model(reg(solar=None, src=13), gx())
        self.assertEqual((rows(m)['solar']['a'], m['why'], m['house']), (None, 'solar', None))
        # no array set and no MPPT: no Solar row at all
        m, _ = model(reg(solar=0.0, src=10), gx())
        self.assertNotIn('solar', rows(m))
        # an MPPT the GX remembers (gone from D-Bus) keeps the row, at the regulator's 0.0
        mem = {'solarcharger:279': {'k': 'solar', 'n': '', 'p': 'SmartSolar MPPT 75/15', 'r': 'p'}}
        m, _ = model(reg(solar=0.0, src=10), gx(), mem)
        self.assertEqual(rows(m)['solar']['a'], 0.0)
        # a Multi RS Solar (its PV is in systemcalc's /Dc/Pv/Current) counts as solar on the boat
        multi = dev('multi', 'multi:0', 30.0, 'Multi RS', 'Multi RS Solar 48V/6000VA/100A', pv=10.0)
        m, devs = model(reg(solar=0.0, src=10), gx(pv=10.0, devices=[multi]))
        self.assertEqual((rows(m)['solar']['a'], rows(m)['multi:0']['a']), (10.0, 20.0))
        self.assertEqual(devs[0]['roles'], 'slp')

    def test_rows_per_device_in_fixed_order(self):
        gen = dev('dcgenset', 'dcgenset:290', 30.0, '', 'Fischer Panda DC')
        wind = dev('dcsource', 'dcsource:289', 7.5, 'Wind charger', 'SmartShunt IP65 500A/50mV', mode=-8)
        water = dev('dcsource', 'dcsource:291', 3.0, '', 'SmartShunt 500A/50mV', mode=-4)
        generic = dev('dcsource', 'dcsource:292', 1.0, '', 'SmartShunt 300A/50mV', mode=-1)
        orion = dev('dcdc', 'dcdc:293', 18.0, 'Orion XS Charger (DCDC)', 'Orion XS 12/12-50A Charger')
        orion_alt = dev('alternator', 'alternator:288', 25.0, 'Orion XS Charger (Alternator)', 'Orion XS 12/12-50A Charger',
                        'HQ2245ABCDE')   # an alternator is listed only once its serial is read
        fuel = dev('fuelcell', 'fuelcell:294', 4.0, '', 'EFOY Comfort 80')
        vebus = dev('vebus', 'vebus:276', -30.0, '', 'MultiPlus 12/3000/120')
        phoenix = dev('inverter', 'inverter:281', -8.0, 'Galley', 'Phoenix Inverter 12/800 Smart')
        devices = [phoenix, water, orion, gen, wind, fuel, vebus, orion_alt, CHARGER, generic]
        m, _ = model(reg(batt=100.0), gx(devices=devices))
        self.assertEqual([(x['k'], x['id']) for x in m['rows']],
                         [('alt', 'alt'), ('solar', 'solar'), ('charger', 'charger:280'), ('multi', 'vebus:276'),
                          ('dcgenset', 'dcgenset:290'), ('fuelcell', 'fuelcell:294'), ('altx', 'alternator:288'),
                          ('dcdc', 'dcdc:293'), ('wind', 'dcsource:289'), ('water', 'dcsource:291'),
                          ('source', 'dcsource:292'), ('inverter', 'inverter:281'), ('inverter', 'vebus:276/load')])
        r = rows(m)
        # the inverting Multi is 0 A as a source and 30 A as a load, never clipped into House
        self.assertEqual((r['vebus:276']['a'], r['vebus:276/load']['a'], r['inverter:281']['a']), (0.0, 30.0, 8.0))
        self.assertEqual((r['charger:280']['c'], r['dcsource:289']['c'], r['inverter:281']['c']),
                         ('Phoenix Smart IP43 Charger 12/30', 'Wind charger', 'Galley'))
        sources = 68 + 22 + 20 + 30 + 4 + 25 + 18 + 7.5 + 3 + 1
        self.assertAlmostEqual(m['house'], sources - 100.0 - 38.0)
        # a monitor mode gui-v2 does not name is a generic source
        self.assertEqual(xc.gx_kind(dict(generic, mode=-5)), 'source')
        self.assertEqual(xc.gx_kind(dict(generic, mode=None)), 'source')
        self.assertIsNone(xc.gx_kind(dev('temperature', 'temperature:1', None)))

    def test_charging_multi_and_its_pv(self):
        multi = dev('multi', 'multi:0', 30.0, 'Multi RS', 'Multi RS Solar 48V/6000VA/100A', pv=10.0)
        m, _ = model(reg(batt=60.0), gx(pv=10.0, devices=[multi]))
        r = rows(m)
        self.assertEqual((r['multi:0']['a'], r['multi:0/load']['a']), (20.0, 0.0))   # its PV is in Solar already
        self.assertAlmostEqual(m['house'], 68.0 + 22.0 + 20.0 - 60.0)

    def test_inverter_sign_and_deadband(self):
        m, _ = model(reg(), gx(devices=[dev('inverter', 'inverter:281', 0.3, '', 'Phoenix')]))
        self.assertEqual(rows(m)['inverter:281']['a'], 0.0)
        m, _ = model(reg(), gx(devices=[dev('inverter', 'inverter:281', -0.4, '', 'Phoenix')]))
        self.assertEqual(rows(m)['inverter:281']['a'], 0.0)

    def test_a_device_with_no_reading_keeps_house_unknown(self):
        m, _ = model(reg(), gx(devices=[dict(CHARGER, amps=None)]))
        self.assertEqual((rows(m)['charger:280']['a'], m['why'], m['house']), (None, 'charger', None))
        # the battery reading missing is named first
        m, _ = model(reg(batt=None, src=1), gx(devices=[dict(CHARGER, amps=None)]))
        self.assertEqual(m['why'], 'batt')

    def test_remembered_device_at_zero(self):
        mem = {'charger:280': {'k': 'charger', 'n': '', 'p': 'Phoenix Smart IP43 Charger 12/30', 'r': 's'},
               'inverter:281': {'k': 'inverter', 'n': 'Galley', 'p': 'Phoenix', 'r': 'l'},
               'battery:1': {'k': 'battery', 'n': '', 'p': 'x', 'r': ''}}    # never a row
        m, devs = model(reg(), gx(), mem)
        r = rows(m)
        self.assertEqual((r['charger:280']['a'], r['charger:280']['c'], r['inverter:281']['a']),
                         (0.0, 'Phoenix Smart IP43 Charger 12/30', 0.0))
        self.assertEqual(m['house'], 40.0)
        self.assertEqual({d['key']: d['on'] for d in devs}, {'charger:280': 0, 'inverter:281': 0})
        # no fresh GX reading at all: whether it is on is not known, so neither is House
        m, devs = model(reg(), None, mem)
        self.assertEqual((rows(m)['charger:280']['a'], m['why'], m['house']), (None, 'charger', None))
        # on D-Bus again: its reading, once
        m, devs = model(reg(), gx(devices=[CHARGER]), mem)
        self.assertEqual((rows(m)['charger:280']['a'], [d['on'] for d in devs if d['key'] == 'charger:280']),
                         (20.0, [1]))

    def test_own_services_twins_and_other_regulators(self):
        own_twin = dev('alternator', 'alternator:300', 66.0, '', 'XREG-010', fr.UID,
                       svc='com.victronenergy.alternator.socketcan_vecan0_vi0_uc1684700')
        other_twin = dev('alternator', 'alternator:301', 41.0, '', 'XREG-010', UID_B,
                         svc='com.victronenergy.alternator.socketcan_vecan0_vi0_uc1684701')
        own = dev('alternator', 'alternator:100', 68.0, '', 'Alternator', fr.UID, svc=xc.alt_service(fr.UID))
        other = dev('alternator', 'alternator:101', 41.0, '', 'Alternator', UID_B, svc=xc.alt_service(UID_B))
        demo_batt = dev('battery', 'battery:100', 50.0, '', 'XREG-010 battery monitor', svc=xc.batt_service(fr.UID))
        peers = [{'uid': fr.UID, 'name': 'Port engine', 'amps': 68.0, 'live': True},
                 {'uid': UID_B, 'name': 'Stbd engine', 'amps': 41.0, 'live': True}]
        uids = [fr.UID, UID_B]
        m, devs = model(reg(batt=100.0), gx(devices=[own_twin, other_twin, own, other, demo_batt]), uids=uids,
                        peers=peers)
        # the other regulator once, from its own link; never its twin or its alternator service, never this one's
        self.assertEqual([(x['id'], x['a'], x['c']) for x in m['rows'] if x['k'] == 'altx'],
                         [('xreg:' + UID_B, 41.0, 'Stbd engine')])
        self.assertEqual([d['key'] for d in devs], ['xreg:' + UID_B])
        self.assertAlmostEqual(m['house'], 68.0 + 22.0 + 41.0 - 100.0)
        # the other regulator not streaming: 0 A, still listed
        peers[1].update(live=False, amps=None)
        m, _ = model(reg(batt=60.0), gx(), uids=uids, peers=peers)
        self.assertEqual(rows(m)['xreg:' + UID_B]['a'], 0.0)
        # streaming but not measuring its alternator: not measured
        peers[1].update(live=True, amps=None)
        m, _ = model(reg(batt=60.0), gx(), uids=uids, peers=peers)
        self.assertEqual((rows(m)['xreg:' + UID_B]['a'], m['why']), (None, 'altx'))
        # a remembered twin is never a row either
        mem = {'alternator:301': {'k': 'altx', 'n': '', 'p': 'XREG-010', 'r': 's', 's': UID_B}}
        _, devs = model(reg(), gx(), mem, uids=uids)
        self.assertEqual(devs, [])

    def test_post_rows(self):
        bm = dev('battery', 'battery:512', -12.34, '', 'SmartShunt 500A/50mV', svc=BATT)
        other_bm = dev('battery', 'battery:513', 0.0, 'Start', 'BMV-712 Smart', svc='com.victronenergy.battery.ttyS7')
        mem = {'inverter:281': {'k': 'inverter', 'n': 'Galley', 'p': 'Phoenix Inverter 12/800 Smart', 'r': 'l'}}
        g = gx(batt=-12.34, devices=[CHARGER, MPPT_B, other_bm, bm, dict(MPPT_A, age=None, amps=None)])
        devs = xc.gx_devices(g, mem, fr.UID)
        post = xc.device_post(devs, g)
        self.assertEqual(post, [
            ['battery', 'SmartShunt 500A/50mV', '', -12.34, 0.3, 1],          # systemcalc's monitor first
            ['battery', 'Start', 'BMV-712 Smart', 0.0, 0.3, 1],
            ['solar', 'Bimini', 'SmartSolar MPPT 100/30', None, None, 1],
            ['solar', 'SmartSolar MPPT 75/15', '', 9.9, 0.3, 1],
            ['charger', 'Phoenix Smart IP43 Charger 12/30', '', 20.0, 0.3, 1],
            ['inverter', 'Galley', 'Phoenix Inverter 12/800 Smart', 0.0, None, 0]])
        many = [dev('charger', 'charger:%d' % i, 1.0, '', 'C%02d' % i) for i in range(20)]
        self.assertEqual(len(xc.device_post(xc.gx_devices(gx(devices=many), None, fr.UID), None)), xc.DEVPOST_MAX)


ORION = dict(dev('dcdc', 'dcdc:293', 20.0, 'Start charger', 'Orion XS 12/12-50A Charger'), in_amps=23.0, in_age=0.2)
ORION_MEM = {'dcdc:293': {'k': 'dcdc', 'n': 'Start charger', 'p': 'Orion XS 12/12-50A Charger', 'r': 's'}}
DRAWS = {'dcdc:293': dict(ORION_MEM['dcdc:293'], h=1)}
FRIDGE = dev('dcload', 'dcload:279', -6.0, 'Fridge', 'SmartShunt 500A/50mV')   # snapshot convention: drawn = negative
PUMP = dev('dcload', 'dcload:283', -2.5, '', 'SmartShunt 300A/50mV')


class TestOrionDrawsHouse(unittest.TestCase):
    # An Orion DC-DC that charges another battery from the house bank: the owner's switch (memory "h") makes it a load
    def test_a_source_by_default(self):
        for mem in (None, ORION_MEM):
            m, devs = model(reg(batt=60.0), gx(devices=[ORION]), mem)
            r = rows(m)
            self.assertEqual((r['dcdc:293']['k'], r['dcdc:293']['a'], r['dcdc:293']['c']), ('dcdc', 20.0, 'Start charger'))
            self.assertAlmostEqual(m['house'], 68.0 + 22.0 + 20.0 - 60.0)
            self.assertEqual(xc.device_post(devs, None),
                             [['dcdc', 'Start charger', 'Orion XS 12/12-50A Charger', 20.0, 0.3, 1]])

    def test_set_to_draw_is_a_load_row(self):
        m, devs = model(reg(batt=40.0), gx(devices=[ORION, CHARGER]), DRAWS)
        self.assertEqual([(x['k'], x['id']) for x in m['rows']],
                         [('alt', 'alt'), ('solar', 'solar'), ('charger', 'charger:280'), ('dcdcload', 'dcdc:293')])
        r = rows(m)
        # its draw is the input current (the house side), captioned with its name
        self.assertEqual((r['dcdc:293']['a'], r['dcdc:293']['c']), (23.0, 'Start charger'))
        self.assertAlmostEqual(m['house'], 68.0 + 22.0 + 20.0 - 40.0 - 23.0)
        self.assertEqual(m['why'], '')
        post = xc.device_post(devs, None)
        self.assertEqual(post[-1], ['dcdc', 'Start charger', 'Orion XS 12/12-50A Charger', -23.0, 0.2, 1])
        # no input reading: its output current
        m, devs = model(reg(batt=40.0), gx(devices=[dict(ORION, in_amps=None, in_age=None)]), DRAWS)
        self.assertEqual(rows(m)['dcdc:293']['a'], 20.0)
        self.assertEqual(xc.device_post(devs, None)[0][3:5], [-20.0, 0.3])
        # no reading at all keeps House unknown, named by the load row's kind
        m, devs = model(reg(batt=40.0), gx(devices=[dict(ORION, amps=None, age=None, in_amps=None, in_age=None)]),
                        DRAWS)
        self.assertEqual((rows(m)['dcdc:293']['a'], m['why'], m['house']), (None, 'dcdcload', None))
        # idle: 0.0, never -0.0
        _, devs = model(reg(), gx(devices=[dict(ORION, amps=0.0, in_amps=0.0)]), DRAWS)
        self.assertEqual(json.dumps(xc.device_post(devs, None)[0][3]), '0.0')
        # a flag on anything but a DC-DC charger does nothing
        mem = {'charger:280': {'k': 'charger', 'n': '', 'p': 'x', 'r': 's', 'h': 1}}
        m, _ = model(reg(), gx(devices=[CHARGER]), mem)
        self.assertEqual(rows(m)['charger:280']['k'], 'charger')

    def test_more_than_the_sources_is_an_unmeasured_source(self):
        m, _ = model(reg(batt=80.0, alt=68.0, solar=22.0), gx(devices=[dict(ORION, in_amps=30.0)]), DRAWS)
        self.assertEqual((m['why'], m['house']), ('unmeasured', None))
        self.assertAlmostEqual(m['x'], 80.0 + 30.0 - 90.0)

    def test_set_while_absent(self):
        m, devs = model(reg(batt=50.0), gx(), DRAWS)
        r = rows(m)
        self.assertEqual((r['dcdc:293']['k'], r['dcdc:293']['a']), ('dcdcload', 0.0))
        self.assertEqual(m['house'], 40.0)
        self.assertEqual(xc.device_post(devs, None), [['dcdc', 'Start charger', 'Orion XS 12/12-50A Charger', 0.0, None, 0]])
        # no fresh GX reading at all: not known
        m, _ = model(reg(batt=50.0), None, DRAWS)
        self.assertEqual((rows(m)['dcdc:293']['a'], m['why']), (None, 'dcdcload'))

    def test_two_orions_one_each_way(self):
        feeds = dev('dcdc', 'dcdc:294', 15.0, 'From start battery', 'Orion XS 12/12-50A Charger')
        m, _ = model(reg(batt=40.0), gx(devices=[ORION, feeds]), DRAWS)
        self.assertEqual([(x['k'], x['id'], x['a']) for x in m['rows'][2:]],
                         [('dcdc', 'dcdc:294', 15.0), ('dcdcload', 'dcdc:293', 23.0)])
        self.assertAlmostEqual(m['house'], 68.0 + 22.0 + 15.0 - 40.0 - 23.0)


class TestDcLoads(unittest.TestCase):
    # A GX DC meter on a load (com.victronenergy.dcload): its own Loads row; House is the unmeasured rest
    def test_one_load(self):
        m, devs = model(reg(batt=50.0), gx(devices=[FRIDGE]))
        r = rows(m)
        self.assertEqual((r['dcload:279']['k'], r['dcload:279']['a'], r['dcload:279']['c']), ('dcload', 6.0, 'Fridge'))
        self.assertAlmostEqual(m['house'], 68.0 + 22.0 - 50.0 - 6.0)
        self.assertEqual(xc.device_post(devs, None), [['dcload', 'Fridge', 'SmartShunt 500A/50mV', -6.0, 0.3, 1]])

    def test_several_loads(self):
        phoenix = dev('inverter', 'inverter:281', -8.0, 'Galley', 'Phoenix Inverter 12/800 Smart')
        m, devs = model(reg(batt=30.0), gx(devices=[PUMP, FRIDGE, ORION, phoenix, CHARGER]), DRAWS)
        self.assertEqual([(x['k'], x['id'], x['a']) for x in m['rows'] if x['k'] in xc.FLOW_LOAD_KINDS],
                         [('inverter', 'inverter:281', 8.0), ('dcdcload', 'dcdc:293', 23.0), ('dcload', 'dcload:279', 6.0),
                          ('dcload', 'dcload:283', 2.5)])
        self.assertAlmostEqual(m['house'], 68.0 + 22.0 + 20.0 - 30.0 - 8.0 - 23.0 - 6.0 - 2.5)
        self.assertEqual([(p[0], p[1], p[3]) for p in xc.device_post(devs, None)],
                         [('charger', 'Phoenix Smart IP43 Charger 12/30', 20.0),
                          ('dcdc', 'Start charger', -23.0), ('inverter', 'Galley', -8.0),
                          ('dcload', 'Fridge', -6.0), ('dcload', 'SmartShunt 300A/50mV', -2.5)])

    def test_a_load_with_no_reading(self):
        m, devs = model(reg(batt=50.0), gx(devices=[FRIDGE, dict(PUMP, amps=None, age=None)]))
        self.assertEqual((rows(m)['dcload:283']['a'], m['why'], m['house']), (None, 'dcload', None))
        self.assertEqual(xc.device_post(devs, None)[1][3:5], [None, None])

    def test_loads_larger_than_the_sources(self):
        big = dev('dcload', 'dcload:284', -60.0, 'Windlass', 'SmartShunt 500A/50mV')
        m, _ = model(reg(batt=-10.0, alt=0.0, solar=22.0), gx(devices=[big, FRIDGE]))
        self.assertEqual((m['why'], m['house']), ('unmeasured', None))
        self.assertAlmostEqual(m['x'], 60.0 + 6.0 - 22.0 - 10.0)
        # within the offset allowance House reads 0, never negative
        m, _ = model(reg(batt=-10.0, alt=0.0, solar=22.0), gx(devices=[dict(big, amps=-32.5)]))
        self.assertEqual((m['why'], m['house']), ('', 0.0))

    def test_negative_reading_and_deadband(self):
        # a meter reading the other way (wired backwards) adds nothing to the loads; under 0.5 A reads 0
        m, _ = model(reg(batt=50.0), gx(devices=[dict(FRIDGE, amps=3.0), dict(PUMP, amps=-0.4)]))
        r = rows(m)
        self.assertEqual((r['dcload:279']['a'], r['dcload:283']['a'], m['house']), (0.0, 0.0, 40.0))

    def test_remembered_load_at_zero(self):
        mem = {'dcload:279': {'k': 'dcload', 'n': 'Fridge', 'p': 'SmartShunt 500A/50mV', 'r': 'l'}}
        m, devs = model(reg(batt=50.0), gx(), mem)
        self.assertEqual((rows(m)['dcload:279']['a'], m['house']), (0.0, 40.0))
        self.assertEqual(devs[0]['on'], 0)


class TestFakeGx(unittest.TestCase):
    def run_hour(self, seed, heading=lambda t: 180.0 + 40.0 * math.sin(t / 300.0), sim=True):
        # a simulated boat: 1 h at 0.5 s, cumulus over 30 A of sun, loads cycling, the alternator in the first 30 min (an
        # Orion drawing after the start), the shore charger in the last 15 (the regulator's CSV4 demo*A; sim=False: firmware
        # before them), the battery the sum of them all
        f, rows_ = xc.FakeGx(seed), []
        for k in range(7200):
            t = k * 0.5
            cloud = 0.45 if (t % 230.0) < 50.0 else 1.0
            solar = 30.0 * cloud
            alt = 60.0 if t < 1800 else 0.0
            fridge = 4.2 if (t % 900) < 420 else 0.0
            orion = 0.3 + 6.0 * math.exp(-t / 150.0) if alt else 0.0
            shore = 25.0 if t >= 2700 and sim else 0.0
            inverter = 20.0 if 1800 <= t < 2400 else 0.0
            house = 9.0 + (6.0 if (t % 600) < 240 else 0.0) + fridge + orion + inverter
            r = {'alt': alt, 'solar': solar, 'batt': alt + solar + shore - house}
            if sim:
                r.update(shore=shore, fridge=fridge, orion=orion, inverter=inverter)
            g = f.step(1000.0 + t, r, heading=heading(t))
            rows_.append((t, r, house, g))
        return rows_

    @staticmethod
    def by(g):
        return {d['service']: d for d in g['devices']}

    def test_consistent_with_the_simulated_boat(self):
        for t, r, house, g in self.run_hour(7):
            d = self.by(g)
            s1, s2 = d['demo.mppt1']['amps'], d['demo.mppt2']['amps']
            self.assertAlmostEqual(s1 + s2, r['solar'], delta=0.11)        # both arrays share the simulated sun
            self.assertAlmostEqual(g['pv'], s1 + s2, delta=1e-9)
            m, _ = model({'alt': r['alt'], 'batt': None, 'solar': None, 'src': 3, 'ser': 0}, g)
            # the tile's battery is the regulator's simulated one, to the monitor's error, the shore charger included
            self.assertAlmostEqual(m['batt'], r['batt'], delta=0.005 * abs(r['batt']) + 0.2, msg='t=%s' % t)
            # battery = sources - loads: House plus every load row is the simulated house
            rs = rows(m)
            loads = rs['inverter:281']['a'] + rs['dcdc:282']['a'] + rs['dcload:283']['a']
            self.assertEqual(rs['inverter:281']['a'], r['inverter'], msg='t=%s' % t)   # the simulation's own inverter
            self.assertAlmostEqual(m['house'] + loads, house, delta=0.005 * abs(g['batt']) + 0.4, msg='t=%s' % t)
            self.assertGreaterEqual(m['house'], 9.0 - 0.6)                  # the loads no device row covers
            self.assertEqual((m['batt_from'], rs['solar']['c']), ('GX', 'GX'))

    def test_named_devices(self):
        g = self.run_hour(5)[-1][3]
        m, devs = model({'alt': 0.0, 'batt': None, 'solar': None, 'src': 3, 'ser': 0}, g)
        self.assertEqual([(x['k'], x['c']) for x in m['rows']],
                         [('alt', ''), ('solar', 'GX'), ('charger', 'Phoenix Smart IP43 Charger 12/30'),
                          ('inverter', 'Phoenix Inverter 12/800 Smart'), ('dcdcload', 'Start battery charger'),
                          ('dcload', 'Fridge')])
        self.assertEqual([p[:3] for p in xc.device_post(devs, g)],
                         [['battery', 'SmartShunt 500A/50mV', ''], ['solar', 'Bimini array', 'SmartSolar MPPT 100/30'],
                          ['solar', 'Deck array', 'SmartSolar MPPT 75/15'],
                          ['charger', 'Phoenix Smart IP43 Charger 12/30', ''],
                          ['dcdc', 'Start battery charger', 'Orion XS 12/12-50A'],
                          ['inverter', 'Phoenix Inverter 12/800 Smart', ''], ['dcload', 'Fridge', 'SmartShunt 500A/50mV']])

    def test_charger_fridge_and_orion_are_the_regulators(self):
        rows_ = self.run_hour(11)
        for t, r, _, g in rows_[::37]:
            d = self.by(g)
            self.assertEqual(d['demo.charger']['amps'], round(r['shore'], 1), msg='t=%s' % t)
            self.assertEqual(d['demo.fridge']['amps'], -round(r['fridge'], 1) or 0.0)
            self.assertEqual(d['demo.inverter']['amps'], -round(r['inverter'], 1) or 0.0)
            self.assertEqual((d['demo.orion']['in_amps'], d['demo.orion']['h']), (round(r['orion'], 1), 1))
            self.assertEqual(d['demo.fridge']['mode'], 3)
            m, devs = model({'alt': r['alt'], 'batt': None, 'solar': None, 'src': 3, 'ser': 0}, g)
            post = {p[1]: p for p in xc.device_post(devs, g)}
            # posted the way the web app reads them: + into the battery, - drawn from it
            self.assertEqual(post['Start battery charger'][3], -round(r['orion'], 1) or 0.0)
            self.assertEqual(post['Fridge'][3], -round(r['fridge'], 1) or 0.0)
        # the charger never comes on by itself: off until the regulator's reading says otherwise
        self.assertEqual({self.by(g)['demo.charger']['amps'] for t, _, _, g in rows_ if t < 2700}, {0.0})

    def test_firmware_without_the_demo_fields(self):
        # older firmware: the charger, the fridge and the Orion read 0, and the battery is still the regulator's
        for t, r, house, g in self.run_hour(4, sim=False)[::50]:
            d = self.by(g)
            self.assertEqual((d['demo.charger']['amps'], d['demo.fridge']['amps'], d['demo.orion']['in_amps'],
                              d['demo.inverter']['amps']), (0.0, 0.0, 0.0, 0.0))
            self.assertAlmostEqual(g['batt'], r['batt'], delta=0.005 * abs(r['batt']) + 0.2, msg='t=%s' % t)

    def test_arrays_split_by_how_the_boat_lies(self):
        a = self.run_hour(3, heading=lambda t: 290.0)
        b = self.run_hour(3, heading=lambda t: 110.0)
        share = lambda rr: sum(self.by(g)['demo.mppt1']['amps'] for *_, g in rr) / sum(r['solar'] for _, r, _, _ in rr)
        self.assertGreater(share(a), share(b) + 0.15)

    def test_seeds_differ(self):
        a = [self.by(g)['demo.mppt1']['amps'] for *_, g in self.run_hour(1)]
        b = [self.by(g)['demo.mppt1']['amps'] for *_, g in self.run_hour(2)]
        self.assertNotEqual(a, b)


class TestCoreFlow(unittest.TestCase):
    def setUp(self):
        self.reg = fr.FakeReg(gx=3)
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def flow(self):
        t = self.h.pub['/Flow/Json']
        return json.loads(t) if t else None

    def wait(self, what, pred, tick=None):
        def ok():
            if tick:
                tick()
            f = self.flow()
            return f is not None and pred(f)
        self.h.until(ok, 8, what)
        return self.flow()

    def feed(self, snap):
        # what xregulator_bridge.py does once a second while the tile shows
        def tick():
            snap['at'] = time.monotonic()
            self.h.mgr.set_gx_side(snap)
        return tick

    def test_gx_fills_the_battery_and_the_limit_line_stays_off(self):
        h, r = self.h, self.reg
        h.want('Flow')
        self.assertEqual(h.mgr.wants_gx(), xc.GX_POLL_TILE_S)
        r.csv3.update(BatteryShuntPresent=1, BattLimitEnable=1, BattCurrentLimitA=500)
        r.csv3_dirty.set()
        r.flow.update(flowBattA=EMPTY, flowSrc=1, ctrlLimiter=4)
        f = self.wait('no battery', lambda f: f['why'] == 'batt')
        self.assertEqual(f['b']['from'], '')
        tick = self.feed(gx(batt=47.5, pv=0.0))
        f = self.wait('GX battery', lambda f: f['b']['a'] == 47.5, tick)
        self.assertEqual((f['b']['from'], f['house'], f['b']['lim'], f['why']), ('GX', 42.5, None, ''))
        # its own battery reading back: the regulator's wins, and the limit line with it
        r.flow.update(flowBattA=5000, flowSrc=11)
        f = self.wait('regulator battery', lambda f: f['b']['a'] == 50.0 and f['b']['lim'] == 50.0, tick)
        self.assertEqual(f['b']['from'], 'Shunt')

    def test_stale_gx_reading_is_dropped(self):
        h, r = self.h, self.reg
        h.want('Flow')
        r.flow.update(flowBattA=EMPTY, flowSrc=1)
        h.mgr.set_gx_side(gx(batt=47.5, at=time.monotonic() - xc.GX_FRESH_S - 1))
        h.tick_for(1.5)
        self.assertIsNone(self.flow()['b']['a'])

    def test_dedupe_uses_flowSolarSer(self):
        h, r = self.h, self.reg
        h.want('Flow')
        r.flow.update(flowSolarSer=xc.ser_hash('HQ2203ABCDE'))
        tick = self.feed(gx(pv=30.1, devices=[dict(MPPT_A, amps=22.1), dict(MPPT_B, amps=8.0)]))
        f = self.wait('merged solar', lambda f: f['rows'][1]['c'] == 'VE.Direct + GX', tick)
        self.assertEqual((f['rows'][1]['a'], f['house']), (30.0, 48.0))

    def test_firmware_without_flowSolarSer(self):
        # a regulator sending 32 CSV4 fields (no flowSolarSer): served script.js lists 32, the bridge reads it as 0
        old = (fr.SCRIPT, fr.SCRIPT_GZ, fr.SCRIPT_ETAG, fr.FIELDS['CSVData4'])
        newer = ('flowSolarSer', 'demoShoreA', 'demoFridgeA', 'demoOrionA', 'demoInverterA')
        self.tearDown()
        try:
            fr.SCRIPT = re.sub(rb'\n\s*"(flowSolarSer|demoShoreA|demoFridgeA|demoOrionA|demoInverterA)",[^\n]*', b'',
                               fr.SCRIPT)
            fr.SCRIPT_GZ, fr.SCRIPT_ETAG = gzip.compress(fr.SCRIPT, 6), '"old32"'
            fr.FIELDS['CSVData4'] = [n for n in fr.FIELDS['CSVData4'] if n not in newer]
            self.assertEqual(len(fr.FIELDS['CSVData4']), 32)
            self.setUp()
            h = self.h
            h.want('Flow')
            tick = self.feed(gx(pv=22.0, devices=[dict(MPPT_A, amps=22.0)]))
            f = self.wait('flow on 32 fields', lambda f: f['rows'][1]['c'] == 'VE.Direct', tick)
            h.tick_for(1.0)
            self.assertEqual((self.flow()['rows'][1]['a'], self.flow()['why']), (22.0, ''))
            self.assertNotIn('flowSolarSer', h.core.link.snapshot()[0]['CSVData4'])
        finally:
            fr.SCRIPT, fr.SCRIPT_GZ, fr.SCRIPT_ETAG, fr.FIELDS['CSVData4'] = old

    def test_memory_keeps_a_gone_device_until_forgotten(self):
        h = self.h
        h.want('Flow')
        tick = self.feed(gx(devices=[CHARGER, dev('charger', 'com.victronenergy.charger.ttyUSB9', 3.0, '', 'Nameless')]))
        f = self.wait('charger row', lambda f: any(x['id'] == 'charger:280' for x in f['rows']), tick)
        mem = json.loads(h.mgr.store.get('gx_devices'))
        # remembered once its VRM instance is known; a service-name key is not
        self.assertEqual(mem, {'charger:280': {'k': 'charger', 'n': '', 'p': 'Phoenix Smart IP43 Charger 12/30',
                                               'r': 's'}})
        self.assertEqual(json.loads(h.mpub['/GxDevices']),
                         [{'key': 'charger:280', 'k': 'charger', 'n': '', 'p': 'Phoenix Smart IP43 Charger 12/30',
                           'on': 1}])
        # it leaves D-Bus (shore power unplugged): listed at 0.0 A
        tick = self.feed(gx())
        f = self.wait('remembered at 0', lambda f: {x['id']: x['a'] for x in f['rows']}.get('charger:280') == 0.0, tick)
        self.assertEqual(f['house'], 40.0)
        self.assertEqual(json.loads(h.mpub['/GxDevices'])[0]['on'], 0)
        # a bridge restart keeps it (localsettings)
        mgr2 = xc.Manager(Store(gx_devices=h.mgr.store.get('gx_devices')), '', xc.Discovery(browse=False),
                          lambda u, i: Store())
        mgr2.start()
        try:
            self.assertIn('charger:280', mgr2.gx_memory)
        finally:
            mgr2.stop()
        # forgotten: the row goes
        self.assertFalse(h.mwrite('/GxForget', 'charger:999'))
        self.assertTrue(h.mwrite('/GxForget', 'charger:280'))
        f = self.wait('forgotten', lambda f: all(x['id'] != 'charger:280' for x in f['rows']), tick)
        self.assertEqual(json.loads(h.mgr.store.get('gx_devices')), {})
        self.assertEqual(h.mpub['/GxForget'], '')
        self.assertEqual(json.loads(h.mpub['/GxDevices']), [])

    def test_orion_switch_survives_a_restart_and_forget_clears_it(self):
        h = self.h
        h.want('Flow')
        snap = gx(devices=[ORION, CHARGER])
        tick = self.feed(snap)
        f = self.wait('orion as a source', lambda f: ('dcdc', 'dcdc:293') in [(x['k'], x['id']) for x in f['rows']], tick)
        gxd = lambda: {d['key']: d for d in json.loads(h.mpub['/GxDevices'])}
        self.assertEqual(gxd()['dcdc:293'], {'key': 'dcdc:293', 'k': 'dcdc', 'n': 'Start charger',
                                             'p': 'Orion XS 12/12-50A Charger', 'on': 1, 'h': 0})
        self.assertNotIn('h', gxd()['charger:280'])                           # the switch is for DC-DC chargers only
        # refused: not JSON, not remembered, not a DC-DC charger, not 0 or 1
        for bad in ('dcdc:293', '{"key":"dcdc:999","on":1}', '{"key":"charger:280","on":1}',
                    '{"key":"dcdc:293","on":2}', '{"key":"dcdc:293"}', '[1]', 5):
            self.assertFalse(h.mwrite('/GxDrawsHouse', bad), bad)
        self.assertNotIn('h', json.loads(h.mgr.store.get('gx_devices'))['dcdc:293'])
        # on: a Loads row with the input current, saved, and the page's switch shows it
        self.assertTrue(h.mwrite('/GxDrawsHouse', json.dumps({'key': 'dcdc:293', 'on': 1})))
        f = self.wait('orion as a load', lambda f: {x['id']: x['k'] for x in f['rows']}.get('dcdc:293') == 'dcdcload',
                      tick)
        self.assertEqual(({x['id']: x['a'] for x in f['rows']}['dcdc:293'], f['house']), (23.0, 68.0 + 22.0 + 20.0 - 50.0 - 23.0))
        self.assertEqual(json.loads(h.mgr.store.get('gx_devices'))['dcdc:293']['h'], 1)
        self.assertEqual((gxd()['dcdc:293']['h'], h.mpub['/GxDrawsHouse']), (1, ''))
        # renamed on the GX: the new name is remembered, the switch stays on
        snap['devices'] = [dict(ORION, name='Thruster charger'), CHARGER]
        self.wait('renamed', lambda f: {x['id']: x['c'] for x in f['rows']}.get('dcdc:293') == 'Thruster charger', tick)
        self.assertEqual(h.mgr.gx_memory['dcdc:293'], {'k': 'dcdc', 'n': 'Thruster charger',
                                                       'p': 'Orion XS 12/12-50A Charger', 'r': 's', 'h': 1})
        # a bridge restart keeps it (localsettings)
        mgr2 = xc.Manager(Store(gx_devices=h.mgr.store.get('gx_devices')), '', xc.Discovery(browse=False),
                          lambda u, i: Store())
        mgr2.start()
        try:
            self.assertEqual(mgr2.gx_memory['dcdc:293'].get('h'), 1)
            pub = tb.Pub(None, mgr2.static_paths())
            mgr2.tick(pub)
            self.assertEqual({d['key']: d.get('h') for d in json.loads(pub['/GxDevices'])},
                             {'charger:280': None, 'dcdc:293': 1})
            m, _ = model(reg(), gx(), mgr2.gx_memory)
            self.assertEqual(rows(m)['dcdc:293']['k'], 'dcdcload')
        finally:
            mgr2.stop()
        # off the GX: still a load, at 0.0 A
        snap['devices'] = [CHARGER]
        f = self.wait('absent load', lambda f: {x['id']: (x['k'], x['a']) for x in f['rows']}.get('dcdc:293')
                      == ('dcdcload', 0.0), tick)
        # switched while absent: off, then on again
        self.assertTrue(h.mwrite('/GxDrawsHouse', json.dumps({'key': 'dcdc:293', 'on': 0})))
        self.wait('absent source', lambda f: {x['id']: x['k'] for x in f['rows']}.get('dcdc:293') == 'dcdc', tick)
        self.assertEqual(gxd()['dcdc:293']['h'], 0)
        h.tick()
        self.assertTrue(h.mwrite('/GxDrawsHouse', json.dumps({'key': 'dcdc:293', 'on': 1})))
        self.wait('absent load again', lambda f: {x['id']: x['k'] for x in f['rows']}.get('dcdc:293') == 'dcdcload', tick)
        # forgotten: the flag goes with it; seen again, it is a source
        self.assertTrue(h.mwrite('/GxForget', 'dcdc:293'))
        self.wait('forgotten', lambda f: all(x['id'] != 'dcdc:293' for x in f['rows']), tick)
        self.assertNotIn('dcdc:293', json.loads(h.mgr.store.get('gx_devices')))
        snap['devices'] = [ORION, CHARGER]
        f = self.wait('back as a source', lambda f: {x['id']: x['k'] for x in f['rows']}.get('dcdc:293') == 'dcdc', tick)
        h.until(lambda: 'dcdc:293' in gxd(), 5, 'remembered again')               # /GxDevices: the next Manager tick
        self.assertEqual((gxd()['dcdc:293']['h'], h.mgr.gx_memory['dcdc:293'].get('h')), (0, None))

    def test_demo_uses_the_made_up_gx_only(self):
        h, r = self.h, self.reg
        h.want('Flow')
        h.mgr.gx_memory['charger:999'] = {'k': 'charger', 'n': 'Real', 'p': 'Real', 'r': 's'}
        r.csv3['hardwarePresent'] = 0
        r.csv3_dirty.set()
        h.until(lambda: h.core.demo, 10, 'demo')
        self.assertEqual(h.mgr.wants_gx(), 0)                              # no D-Bus reads in demo
        h.mgr.set_gx_side(gx(batt=-99.0, pv=99.0))                          # a real GX's readings: ignored in demo
        f = self.wait('fake battery monitor', lambda f: f['b']['from'] == 'GX')
        # the simulation's shore charger, fridge, Orion and inverter (CSV4 demo*A): alternator 68, solar 22, charger 25,
        # loads 40 of which the fridge 4.2, the Orion 3.0 and the inverter 20, so the regulator's battery is 75
        r.demo_devs.update(demoShoreA=2500, demoFridgeA=420, demoOrionA=300, demoInverterA=2000)
        r.flow.update(flowBattA=7500)
        f = self.wait('charger', lambda f: {x['id']: x['a'] for x in f['rows']}['charger:280'] == 25.0)
        ids = [x['id'] for x in f['rows']]
        self.assertEqual(ids, ['alt', 'solar', 'charger:280', 'inverter:281', 'dcdc:282', 'dcload:283'])   # no real ones
        self.assertEqual((f['rows'][1]['c'], f['why'], f['b']['lim']), ('GX', '', None))
        a = {x['id']: x['a'] for x in f['rows']}
        self.assertEqual((a['dcdc:282'], a['dcload:283'], a['inverter:281']), (3.0, 4.2, 20.0))
        self.assertAlmostEqual(f['house'], 12.8, delta=0.6)                # what no device row covers
        # to the made-up monitor's error: 0.4 % gain and 0.04 A offset on 75 A, plus its noise
        self.assertAlmostEqual(f['house'] + a['inverter:281'] + a['dcdc:282'] + a['dcload:283'], 40.0, delta=0.6)
        self.assertAlmostEqual(f['b']['a'], 75.0, delta=0.6)                # the Dashboard's battery, the tile's
        p = h.pub
        self.assertEqual(set(p), set(p.modes))
        r.csv3['hardwarePresent'] = 1
        r.csv3_dirty.set()
        h.until(lambda: not h.core.demo and h.core.fake_gx is None, 10, 'demo left')
        self.wait('own shunt again', lambda f: f['b']['from'] == 'Shunt')


class TestDevicePost(unittest.TestCase):
    # GX link 4: the GX's device readings to the regulator (/gxDevices), for its web app's Live Data
    def start(self, gx_link=4, **kw):
        self.reg = fr.FakeReg(gx=gx_link, **kw)
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')
        return self.h, self.reg

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def feed(self, snap):
        def tick():
            snap['at'] = time.monotonic()
            self.h.mgr.set_gx_side(snap)
        return tick

    def until(self, pred, what, tick, timeout=10):
        self.h.until(lambda: (tick(), pred())[1], timeout, what)

    def test_posts_and_follows_the_asked_interval(self):
        h, reg = self.start()
        self.assertEqual(h.mgr.wants_gx(), xc.GX_POLL_POST_S)              # no tile shown: reads for the posts
        bm = dev('battery', 'battery:512', -12.3, '', 'SmartShunt 500A/50mV', svc=BATT)
        snap = gx(batt=-12.3, devices=[bm, CHARGER])
        tick = self.feed(snap)
        self.until(lambda: reg.devposts, 'first post', tick)
        p = reg.requests('/gxDevices')[-1]
        self.assertEqual((p['method'], p['q']['gx'], p['q']['uid']), ('POST', '1', reg.uid))
        self.assertEqual(reg.devposts[-1], {'v': 1, 'd': [['battery', 'SmartShunt 500A/50mV', '', -12.3, 0.3, 1],
                                                          ['charger', 'Phoenix Smart IP43 Charger 12/30', '', 20.0, 0.3,
                                                           1]]})
        # the answer's everyS (5) paces the next
        n = len(reg.devposts)
        reg.dev_every = 2
        t0 = time.time()
        self.until(lambda: len(reg.devposts) > n, 'second post', tick)
        self.assertGreater(time.time() - t0, 3.5)
        n = len(reg.devposts)
        t0 = time.time()
        self.until(lambda: len(reg.devposts) > n, 'third post at 2 s', tick)
        self.assertLess(time.time() - t0, 3.0)
        self.assertEqual(h.core.devpost_every, 2.0)
        # a device leaving posts at once
        reg.dev_every = 60
        self.until(lambda: h.core.devpost_every == 60.0, 'slow', tick)
        n = len(reg.devposts)
        snap['devices'] = [bm]
        h.mgr.gx_memory.clear()
        self.until(lambda: len(reg.devposts) > n, 'set changed', tick, timeout=4)
        self.assertEqual([d[0] for d in reg.devposts[-1]['d']], ['battery'])

    def test_orion_drawing_and_dc_loads_post_negative(self):
        h, reg = self.start()
        snap = gx(devices=[ORION, FRIDGE, CHARGER])
        tick = self.feed(snap)
        self.until(lambda: reg.devposts and len(reg.devposts[-1]['d']) == 3, 'first post', tick)
        self.assertEqual([d[0::3] for d in reg.devposts[-1]['d']], [['charger', 20.0], ['dcdc', 20.0], ['dcload', -6.0]])
        h.mgr.on_write('/GxDrawsHouse', '{"key":"dcdc:293","on":1}')
        reg.dev_every = 2
        self.until(lambda: reg.devposts[-1]['d'][1][3] == -23.0, 'drawing: negative, the input current', tick)
        self.assertEqual(reg.devposts[-1]['d'][1], ['dcdc', 'Start charger', 'Orion XS 12/12-50A Charger', -23.0, 0.2, 1])

    def test_older_firmware_gets_no_posts(self):
        h, reg = self.start(gx_link=3)
        self.assertEqual(h.mgr.wants_gx(), 0)
        tick = self.feed(gx(devices=[CHARGER]))
        for _ in range(20):
            tick()
            h.tick()
            time.sleep(0.1)
        self.assertEqual(reg.requests('/gxDevices'), [])

    def test_a_page_is_not_the_route(self):
        # older firmware in AP mode answers any path with a 200 page: stop until the link reconnects
        h, reg = self.start()
        reg.devices_page = '<html>Success</html>'
        tick = self.feed(gx(devices=[CHARGER]))
        self.until(lambda: reg.requests('/gxDevices'), 'one try', tick)
        self.until(lambda: h.core.devpost_hold >= 0, 'held', tick)
        self.assertEqual(h.mgr.wants_gx(), 0)
        n = len(reg.requests('/gxDevices'))
        for _ in range(30):
            tick()
            h.tick()
            time.sleep(0.1)
        self.assertEqual(len(reg.requests('/gxDevices')), n)

    def test_demo_posts_the_made_up_devices(self):
        h, reg = self.start()
        reg.csv3['hardwarePresent'] = 0
        reg.csv3_dirty.set()
        h.until(lambda: h.core.demo, 10, 'demo')
        h.until(lambda: reg.devposts and len(reg.devposts[-1]['d']) == 7, 10, 'fake devices posted')
        self.assertEqual([d[1] for d in reg.devposts[-1]['d']],
                         ['SmartShunt 500A/50mV', 'Bimini array', 'Deck array', 'Phoenix Smart IP43 Charger 12/30',
                          'Start battery charger', 'Phoenix Inverter 12/800 Smart', 'Fridge'])


class TestGxReader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tb.install_stubs()
        sys.path.insert(1, tb.BRIDGE + '/ext/velib_python')
        import importlib
        cls.br = importlib.import_module('xregulator_bridge')

    def bus(self, values, slow=()):
        dbus = sys.modules['dbus']
        calls = []

        class Bus:
            def get_object(self, svc, path, introspect=False):
                if svc not in values:
                    raise dbus.exceptions.DBusException('no owner ' + svc)

                def get(dbus_interface=None, reply_handler=None, error_handler=None, timeout=None):
                    calls.append((svc, path))
                    if (svc, path) in slow:
                        return                                     # no reply yet: nothing waits on it
                    if path in values[svc]:
                        reply_handler(values[svc][path])
                    else:
                        error_handler(dbus.exceptions.DBusException('no such path'))
                return types.SimpleNamespace(GetValue=get)
        return Bus(), calls

    def test_snapshot(self):
        now = [100.0]
        named = lambda inst, product, name='': {'/ProductName': product, '/CustomName': name, '/DeviceInstance': inst}
        values = {
            'com.victronenergy.system': {'/Dc/Battery/Current': 12.5, '/Dc/Battery/BatteryService': BATT,
                                         '/Dc/Pv/Current': 31.0},
            BATT: dict(named(512, 'SmartShunt 500A/50mV'), **{'/Dc/0/Current': 12.5}),
            'com.victronenergy.solarcharger.ttyS3': dict(named(279, 'SmartSolar MPPT 75/15'), **{
                '/Serial': 'HQ1929X7KQZ', '/Dc/0/Current': 20.0, '/Load/I': 1.0}),
            'com.victronenergy.charger.ttyUSB0': dict(named(280, 'Phoenix Smart IP43 Charger 12/30', ' '),
                                                      **{'/Dc/0/Current': 18.0}),
            'com.victronenergy.alternator.socketcan_vecan0_vi0_uc2': dict(named(288, 'Alternator'), **{
                '/Serial': '1234', '/Dc/0/Power': 700.0, '/Dc/0/Voltage': 14.0}),
            'com.victronenergy.multi.socketcan_can1_vi0_uc3': dict(named(0, 'Multi RS Solar', 'Multi RS'), **{
                '/Dc/0/Current': 25.0, '/Dc/0/Voltage': 13.5, '/Yield/Power': 135.0}),
            'com.victronenergy.vebus.ttyS4': dict(named(276, 'MultiPlus 12/3000/120'), **{'/Dc/0/Current': -40.0}),
            'com.victronenergy.inverter.ttyS5': dict(named(281, 'Phoenix Inverter 12/800 Smart'), **{
                '/Dc/0/Voltage': 12.5, '/Ac/Out/L1/V': 120.0, '/Ac/Out/L1/I': 2.5}),
            'com.victronenergy.dcdc.ttyUSB3': dict(named(291, 'Orion XS 12/12-50A Charger', 'Orion XS Charger (DCDC)'),
                                                   **{'/Dc/0/Current': 30.0}),
            'com.victronenergy.dcsource.ttyUSB2': dict(named(289, 'SmartShunt IP65 500A/50mV', 'Wind charger '), **{
                '/Dc/0/Current': 17.29, '/Settings/MonitorMode': -8}),
            'com.victronenergy.alternator.xregulator_feedc0': {'/Dc/0/Current': 66.0},   # ours, hidden: no name
            'com.victronenergy.temperature.ttyS6': {'/Temperature': 20.0},
        }
        bus, calls = self.bus(values)
        r = self.br.GxReader(bus, lambda: now[0])
        r.set_names(list(values) + ['com.victronenergy.settings'])
        r.poll()
        self.assertFalse(any('temperature' in c[0] or 'settings' in c[0] for c in calls))
        s = r.snapshot()
        self.assertEqual((s['batt'], s['batt_service'], s['pv']), (12.5, BATT, 31.0))
        by = {d['cls']: d for d in s['devices']}
        self.assertNotIn('com.victronenergy.alternator.xregulator_feedc0', [d['service'] for d in s['devices']])
        self.assertEqual((by['solarcharger']['amps'], by['solarcharger']['key'], by['solarcharger']['serial']),
                         (21.0, 'solarcharger:279', 'HQ1929X7KQZ'))
        self.assertEqual((by['charger']['amps'], by['charger']['name'], by['charger']['product']),
                         (18.0, '', 'Phoenix Smart IP43 Charger 12/30'))   # a blank CustomName is no name
        self.assertEqual((by['alternator']['amps'], by['alternator']['serial']), (50.0, '1234'))
        self.assertEqual((by['multi']['amps'], by['multi']['pv'], by['multi']['name']), (25.0, 10.0, 'Multi RS'))
        self.assertEqual((by['vebus']['amps'], by['vebus']['key']), (-40.0, 'vebus:276'))
        self.assertEqual(by['inverter']['amps'], -24.0)                      # 120 V x 2.5 A over 12.5 V, inverting
        self.assertEqual((by['dcdc']['amps'], by['dcdc']['name']), (30.0, 'Orion XS Charger (DCDC)'))
        self.assertEqual((by['dcsource']['mode'], by['dcsource']['name'], xc.gx_kind(by['dcsource'])),
                         (-8, 'Wind charger', 'wind'))
        self.assertEqual(by['charger']['age'], 0.0)
        # names are asked for at first sight and then every NAME_EVERY_S, the currents every poll
        n = len(calls)
        r.poll()
        self.assertFalse(any(c[1] == '/ProductName' for c in calls[n:]))
        now[0] += self.br.NAME_EVERY_S
        n = len(calls)
        r.poll()
        self.assertTrue(any(c[1] == '/ProductName' for c in calls[n:]))
        # readings age out; names stay
        now[0] += xc.GX_FRESH_S + 0.5
        s = r.snapshot()
        self.assertEqual((s['batt'], s['pv']), (None, None))
        self.assertTrue(all(d['amps'] is None and d['age'] is None for d in s['devices']))
        self.assertEqual({d['product'] for d in s['devices']} & {'Phoenix Smart IP43 Charger 12/30'},
                         {'Phoenix Smart IP43 Charger 12/30'})

    def test_dc_loads_and_dcdc_input(self):
        named = lambda inst, product, name='': {'/ProductName': product, '/CustomName': name, '/DeviceInstance': inst}
        values = {
            'com.victronenergy.system': {'/Dc/Battery/Current': 1.0},
            'com.victronenergy.dcload.ttyS7': dict(named(279, 'SmartShunt 500A/50mV', 'Fridge'), **{
                '/Dc/0/Current': 6.0, '/Settings/MonitorMode': 3}),
            'com.victronenergy.dcload.ttyS8': dict(named(283, 'SmartShunt 300A/50mV'), **{'/Dc/0/Current': 0.0}),
            'com.victronenergy.dcdc.ttyUSB3': dict(named(293, 'Orion XS 12/12-50A Charger'), **{
                '/Dc/0/Current': 20.0, '/Dc/In/I': 23.0, '/Dc/In/P': 999.0, '/Dc/In/V': 13.0}),
            'com.victronenergy.dcdc.ttyUSB4': dict(named(294, 'Orion XS 12/12-50A Charger'), **{
                '/Dc/0/Current': 20.0, '/Dc/In/P': 312.0, '/Dc/In/V': 13.0}),
            'com.victronenergy.dcdc.ttyUSB5': dict(named(295, 'Orion XS 12/12-50A Charger'), **{'/Dc/0/Current': 20.0}),
        }
        bus, calls = self.bus(values)
        r = self.br.GxReader(bus, lambda: 100.0)
        r.set_names(list(values))
        r.poll()
        by = {d['key']: d for d in r.snapshot()['devices']}
        # a DC load meter reads + for what its load draws: drawn from the battery bus, negative
        self.assertEqual((by['dcload:279']['amps'], by['dcload:279']['name'], xc.gx_kind(by['dcload:279'])),
                         (-6.0, 'Fridge', 'dcload'))
        self.assertEqual(json.dumps(by['dcload:283']['amps']), '0.0')
        self.assertEqual([(by[k]['amps'], by[k]['in_amps']) for k in ('dcdc:293', 'dcdc:294', 'dcdc:295')],
                         [(20.0, 23.0), (20.0, 24.0), (20.0, None)])
        self.assertEqual((by['dcdc:293']['in_age'], by['dcdc:295']['in_age'], by['dcload:279']['in_amps']),
                         (0.0, None, None))

    def test_settled_only_after_the_first_replies(self):
        # the snapshot right after the first poll holds no replies yet: the bridge passes on none until the next
        now = [10.0]
        bus, _ = self.bus({'com.victronenergy.system': {'/Dc/Battery/Current': 1.0}})
        r = self.br.GxReader(bus, lambda: now[0])
        self.assertFalse(r.settled())
        r.poll()
        self.assertFalse(r.settled())
        now[0] += 1.0
        r.poll()
        self.assertTrue(r.settled())
        r.idle()                                                           # reading stopped: start over
        r.poll()
        self.assertFalse(r.settled())

    def test_gone_service_and_slow_service(self):
        values = {'com.victronenergy.system': {'/Dc/Battery/Current': 3.0, '/Dc/Battery/BatteryService': BATT},
                  'com.victronenergy.charger.ttyUSB0': {'/Dc/0/Current': 18.0, '/ProductName': 'Charger',
                                                        '/DeviceInstance': 280}}
        bus, calls = self.bus(values, slow={('com.victronenergy.system', '/Dc/Battery/Current')})
        r = self.br.GxReader(bus, lambda: 50.0)
        r.set_names(['com.victronenergy.charger.ttyUSB0', 'com.victronenergy.solarcharger.gone'])
        r.poll()                                                           # the gone one raises; nothing stops
        s = r.snapshot()
        self.assertIsNone(s['batt'])                                       # still waiting on its reply
        self.assertEqual([(d['key'], d['amps']) for d in s['devices']], [('charger:280', 18.0)])
        r.name_changed('com.victronenergy.charger.ttyUSB0', False)
        self.assertEqual(r.snapshot()['devices'], [])


# ---- 2026-10-29 bridge fixes: twin before its serial, ages by power and voltage, the DC system meter, DC loads named by
# monitor mode, a DC meter in DC-DC mode, the demo's Orion and Fridge ----

TWIN_SVC = 'com.victronenergy.alternator.socketcan_vecan0_vi0_uc1684700'


def twin_dev(serial='', name=''):
    # this regulator's NMEA 2000 twin as vecan-dbus lists it: "Alternator" (ProductId 0xB091), VRM instance 0
    return dev('alternator', 'alternator:0', None, name, 'Alternator', serial, svc=TWIN_SVC, age=None)


def mgr_with(memory, units=''):
    m = xc.Manager(Store(gx_devices=json.dumps(memory), units=units), '', xc.Discovery(browse=False),
                   lambda u, i: Store())
    m.start()
    return m


class TestTwinBeforeSerial(unittest.TestCase):
    # Bench round 7: at a demo exit the twin came back with its name read before its /Serial
    def test_no_serial_no_row_no_post(self):
        g = gx(devices=[twin_dev()])
        m, devs = model(reg(), g, uids=[fr.UID])
        self.assertEqual((devs, [x['id'] for x in m['rows']]), ([], ['alt', 'solar']))
        self.assertEqual(xc.device_post(devs, g), [])
        # serial read: this regulator's, so still nothing
        m, devs = model(reg(), gx(devices=[twin_dev(fr.UID)]), uids=[fr.UID])
        self.assertEqual(devs, [])
        # another make's alternator: listed once its serial reads
        other = dev('alternator', 'alternator:5', 30.0, '', 'Alternator', '1F2E3D4C')
        self.assertEqual([d['key'] for d in model(reg(), gx(devices=[dict(other, serial='')]))[1]], [])
        m, devs = model(reg(), gx(devices=[other]))
        self.assertEqual((rows(m)['alternator:5']['a'], devs[0]['serial']), (30.0, '1F2E3D4C'))

    def test_a_remembered_alternator_waits_as_absent(self):
        # a known alternator back on D-Bus before its serial reads: its remembered row stays, at 0.0 A, not twice
        mem = {'alternator:5': {'k': 'altx', 'n': 'Stbd', 'p': 'WS500', 'r': 's', 's': '1F2E3D4C'}}
        m, devs = model(reg(), gx(devices=[dev('alternator', 'alternator:5', 30.0, 'Stbd', 'WS500')]), mem)
        self.assertEqual([(d['key'], d['on'], d['amps']) for d in devs], [('alternator:5', 0, 0.0)])

    def test_never_shown_posted_or_stored(self):
        reg_ = fr.FakeReg(gx=4)
        h = Harness(reg_)
        try:
            h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
            h.want('Flow')
            snap = gx(devices=[CHARGER, twin_dev()])
            flows, posted = [], []

            def tick():
                snap['at'] = time.monotonic()
                h.mgr.set_gx_side(snap)
                t = h.pub['/Flow/Json']
                if t:
                    flows.append(json.loads(t))
            reg_.dev_every = 2
            h.until(lambda: (tick(), reg_.devposts)[1], 10, 'first post')
            h.tick_for(1.5)
            # the serial reads: the twin
            snap['devices'] = [CHARGER, twin_dev(reg_.uid)]
            for _ in range(25):
                tick()
                h.tick()
                time.sleep(0.1)
            self.assertTrue(flows)
            self.assertFalse([f for f in flows if any(r['k'] == 'altx' for r in f['rows'])])
            self.assertFalse([p for p in reg_.devposts if any(r[0] == 'altx' for r in p['d'])])
            mem = json.loads(h.mgr.store.get('gx_devices'))
            self.assertEqual(sorted(mem), ['charger:280'])
            self.assertEqual([d['key'] for d in json.loads(h.mpub['/GxDevices'])], ['charger:280'])
        finally:
            h.close()
            reg_.close()

    def test_stored_twin_dropped_on_load(self):
        stored = {'alternator:0': {'k': 'altx', 'n': '', 'p': 'Alternator', 'r': 's'},           # bench round 7's entry
                  'alternator:1': {'k': 'altx', 'n': 'XREG-010', 'p': 'Alternator', 'r': 's'},   # named on the GX
                  'alternator:2': {'k': 'altx', 'n': '', 'p': 'XREG-010', 'r': 's', 's': fr.UID},  # this one's serial
                  'alternator:3': {'k': 'altx', 'n': '', 'p': 'Orion XS 12/12-50A', 'r': 's'},   # not the twin's identity
                  'alternator:4': {'k': 'altx', 'n': '', 'p': 'Alternator', 'r': 's', 's': '1F2E3D4C'},
                  'charger:280': {'k': 'charger', 'n': '', 'p': 'Phoenix', 'r': 's'}}
        m = mgr_with(stored, units=fr.UID)
        try:
            self.assertEqual(sorted(m.gx_memory), ['alternator:3', 'alternator:4', 'charger:280'])
            self.assertEqual(sorted(json.loads(m.store.get('gx_devices'))), ['alternator:3', 'alternator:4', 'charger:280'])
        finally:
            m.stop()
        # nothing pinned: the serial-less twin still goes, a serial cannot be matched
        m = mgr_with(stored)
        try:
            self.assertEqual(sorted(m.gx_memory), ['alternator:2', 'alternator:3', 'alternator:4', 'charger:280'])
        finally:
            m.stop()
        # gx_devices alone never lists one either
        _, devs = model(reg(), gx(), {'alternator:0': stored['alternator:0']}, uids=[fr.UID])
        self.assertEqual(devs, [])

    def test_key_shown_to_be_a_twin_is_dropped(self):
        # a stored entry under the key the GX now shows to carry a pinned regulator's serial goes, whatever it says
        m = mgr_with({'alternator:0': {'k': 'altx', 'n': 'Port', 'p': 'Something', 'r': 's'}})
        try:
            m._remember(gx(devices=[twin_dev(fr.UID)]), [fr.UID])
            self.assertEqual(m.gx_memory, {})
            self.assertEqual(json.loads(m.store.get('gx_devices')), {})
        finally:
            m.stop()


class TestGxReaderAges(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tb.install_stubs()
        sys.path.insert(1, tb.BRIDGE + '/ext/velib_python')
        import importlib
        cls.br = importlib.import_module('xregulator_bridge')

    def test_age_from_the_paths_used(self):
        named = lambda inst, product: {'/ProductName': product, '/CustomName': '', '/DeviceInstance': inst}
        values = {
            'com.victronenergy.system': {},
            'com.victronenergy.alternator.ttyS1': dict(named(288, 'Alternator'), **{
                '/Serial': '1F2E3D4C', '/Dc/0/Power': 700.0, '/Dc/0/Voltage': 14.0}),
            'com.victronenergy.dcgenset.ttyS2': dict(named(290, 'Fischer Panda DC'), **{
                '/Dc/0/Power': 1400.0, '/Dc/0/Voltage': 14.0}),
            'com.victronenergy.inverter.ttyS5': dict(named(281, 'Phoenix'), **{
                '/Dc/0/Voltage': 12.5, '/Ac/Out/L1/V': 120.0, '/Ac/Out/L1/I': 2.5}),
            'com.victronenergy.dcdc.ttyS4': dict(named(294, 'Orion XS'), **{
                '/Dc/0/Current': 20.0, '/Dc/In/P': 312.0, '/Dc/In/V': 13.0}),
            'com.victronenergy.solarcharger.ttyS3': dict(named(279, 'SmartSolar'), **{
                '/Serial': 'HQ1929X7KQZ', '/Dc/0/Current': 20.0, '/Load/I': 1.0}),
        }
        now = [100.0]
        bus, _ = TestGxReader.bus(self, values)
        r = self.br.GxReader(bus, lambda: now[0])
        r.set_names(list(values))
        r.poll()
        # every input read at 100.0; then one of each pair older
        for svc, path, t in (('com.victronenergy.alternator.ttyS1', '/Dc/0/Voltage', 98.5),
                             ('com.victronenergy.dcgenset.ttyS2', '/Dc/0/Power', 97.0),
                             ('com.victronenergy.inverter.ttyS5', '/Dc/0/Voltage', 96.5),
                             ('com.victronenergy.dcdc.ttyS4', '/Dc/In/V', 99.0),
                             ('com.victronenergy.solarcharger.ttyS3', '/Load/I', 98.0)):
            v, _ = r.values[(svc, path)]
            r.values[(svc, path)] = (v, t)
        now[0] = 100.4
        by = {d['cls']: d for d in r.snapshot()['devices']}
        self.assertEqual((by['alternator']['amps'], by['alternator']['age']), (50.0, 1.9))
        self.assertEqual((by['dcgenset']['amps'], by['dcgenset']['age']), (100.0, 3.4))
        self.assertEqual((by['inverter']['amps'], by['inverter']['age']), (-24.0, 3.9))
        self.assertEqual((by['dcdc']['in_amps'], by['dcdc']['in_age'], by['dcdc']['age']), (24.0, 1.4, 0.4))
        self.assertEqual((by['solarcharger']['amps'], by['solarcharger']['age']), (21.0, 2.4))
        # the web app's row: real amps with an age, never "no reading" beside them
        devs = xc.gx_devices(r.snapshot(), None, fr.UID)
        post = {p[1]: p for p in xc.device_post(devs, None)}
        self.assertEqual(post['Fischer Panda DC'][3:5], [100.0, 3.4])
        self.assertEqual(post['Alternator'][3:5], [50.0, 1.9])
        # one input past GX_FRESH_S: no reading, no age
        now[0] = 98.5 + xc.GX_FRESH_S + 0.1
        by = {d['cls']: d for d in r.snapshot()['devices']}
        self.assertEqual((by['alternator']['amps'], by['alternator']['age']), (None, None))

    def test_dcsystem_read_and_turned_around(self):
        values = {'com.victronenergy.system': {},
                  'com.victronenergy.dcsystem.ttyS9': {'/ProductName': 'SmartShunt 500A/50mV', '/CustomName': 'House panel',
                                                       '/DeviceInstance': 300, '/Dc/0/Current': 14.5}}
        bus, _ = TestGxReader.bus(self, values)
        r = self.br.GxReader(bus, lambda: 50.0)
        r.set_names(list(values))
        r.poll()
        d = r.snapshot()['devices'][0]
        self.assertEqual((d['key'], d['amps'], d['age'], xc.gx_kind(d)), ('dcsystem:300', -14.5, 0.0, 'dcsystem'))


METER = dev('dcsystem', 'dcsystem:300', -30.0, 'House panel', 'SmartShunt 500A/50mV')   # 30 A drawn


class TestDcSystemMeter(unittest.TestCase):
    # A whole-DC-system meter (com.victronenergy.dcsystem) gives House
    def test_absent_house_is_calculated(self):
        m, _ = model(reg(batt=50.0), gx())
        self.assertEqual((m['hm'], m['house'], m['why']), (0, 40.0, ''))

    def test_present_and_agreeing(self):
        # sources 68 + 22 = 90, battery 60: the rest is 30, and the meter reads 30.4
        m, devs = model(reg(batt=60.0), gx(devices=[dict(METER, amps=-30.4)]))
        self.assertEqual((m['hm'], m['house'], m['why'], m['x']), (1, 30.4, '', 0.0))
        self.assertEqual([x['id'] for x in m['rows']], ['alt', 'solar'])      # no row of its own
        # it reaches the regulator's web app, last, drawn = negative
        self.assertEqual(xc.device_post(devs, None), [['dcsystem', 'House panel', 'SmartShunt 500A/50mV', -30.4, 0.3, 1]])
        # never remembered: gone, House is calculated again
        mgr = mgr_with({})
        try:
            mgr._remember(gx(devices=[METER, CHARGER]), [fr.UID])
            self.assertEqual(sorted(mgr.gx_memory), ['charger:280'])
        finally:
            mgr.stop()

    def test_disagreeing(self):
        # the rest is 30 A; the meter reads 24: 6 A leave the sources that no meter sees, an unmeasured load
        m, _ = model(reg(batt=60.0), gx(devices=[dict(METER, amps=-24.0)]))
        self.assertEqual((m['hm'], m['house'], m['why']), (1, 24.0, 'gap'))
        self.assertAlmostEqual(m['x'], 6.0)
        # the meter reads 37: 7 A more go out than the sources give, an unmeasured source
        m, _ = model(reg(batt=60.0), gx(devices=[dict(METER, amps=-37.0)]))
        self.assertEqual((m['house'], m['why']), (37.0, 'unmeasured'))
        self.assertAlmostEqual(m['x'], 7.0)
        # inside the tolerance: 5 % of the sources (4.5 A of 90) beats 1 A
        for amps in (-25.6, -34.4):
            m, _ = model(reg(batt=60.0), gx(devices=[dict(METER, amps=amps)]))
            self.assertEqual((m['why'], m['house']), ('', -amps))
        # at small currents the 1 A floor
        m, _ = model(reg(batt=1.0, alt=0.0, solar=6.0), gx(devices=[dict(METER, amps=-3.5)]))
        self.assertEqual(m['why'], 'gap')
        m, _ = model(reg(batt=1.0, alt=0.0, solar=6.0), gx(devices=[dict(METER, amps=-4.2)]))
        self.assertEqual(m['why'], '')

    def test_measures_house_with_no_battery_or_a_missing_row(self):
        m, _ = model(reg(batt=None, src=1), gx(devices=[METER]))
        self.assertEqual((m['hm'], m['house'], m['why']), (1, 30.0, ''))
        m, _ = model(reg(solar=None, src=13), gx(devices=[METER]))
        self.assertEqual((m['hm'], m['house'], m['why'], rows(m)['solar']['a']), (1, 30.0, '', None))

    def test_meter_with_no_reading_falls_back(self):
        m, _ = model(reg(batt=50.0), gx(devices=[dict(METER, amps=None, age=None)]))
        self.assertEqual((m['hm'], m['house']), (0, 40.0))

    def test_loads_inside_and_outside(self):
        # DC load meters and an Orion drawing from the house bank are inside the meter (gui-v2 lists them under its DC
        # Loads total); an inverter is not (systemcalc keeps inverters out of the DC system), nor a DC meter in DC-DC mode
        phoenix = dev('inverter', 'inverter:281', -8.0, 'Galley', 'Phoenix Inverter 12/800 Smart')
        shunt_dcdc = dev('dcsource', 'dcsource:301', -4.0, '', 'SmartShunt 300A/50mV', mode=-3)
        g = gx(devices=[METER, FRIDGE, ORION, phoenix, shunt_dcdc])
        m, _ = model(reg(batt=20.0), g, DRAWS)
        r = rows(m)
        self.assertEqual({k: r[k].get('in') for k in ('dcload:279', 'dcdc:293', 'inverter:281', 'dcsource:301')},
                         {'dcload:279': 1, 'dcdc:293': 1, 'inverter:281': None, 'dcsource:301': None})
        self.assertAlmostEqual(m['house'], 30.0 - 6.0 - 23.0)
        # the sum: 90 - 20 - (8 + 23 + 6 + 4) = 29 against the meter's 30 - 6 - 23 = 1: an unmeasured load of 28 A
        self.assertEqual(m['why'], 'gap')
        self.assertAlmostEqual(m['x'], 28.0)
        # balanced: battery 20 + inverter 8 + DC-DC meter 4 + the meter's 30 = 62 of 62 sources
        m, _ = model(reg(batt=20.0, alt=40.0), g, DRAWS)
        self.assertEqual((m['why'], round(m['house'], 2)), ('', 1.0))
        # without the meter none is marked
        m, _ = model(reg(batt=20.0), gx(devices=[FRIDGE]))
        self.assertNotIn('in', rows(m)['dcload:279'])

    def test_two_meters_add_up(self):
        m, _ = model(reg(batt=60.0), gx(devices=[dict(METER, amps=-18.0),
                                                 dev('dcsystem', 'dcsystem:302', -12.0, '', 'SmartShunt 300A/50mV')]))
        self.assertEqual((m['house'], m['why']), (30.0, ''))

    def test_dcload_in_dc_system_mode(self):
        d = dev('dcload', 'dcload:303', -30.0, '', 'SmartShunt 500A/50mV', mode=6)
        self.assertEqual(xc.gx_kind(d), 'dcsystem')
        m, _ = model(reg(batt=60.0), gx(devices=[d]))
        self.assertEqual((m['hm'], m['house'], [x['id'] for x in m['rows']]), (1, 30.0, ['alt', 'solar']))

    def test_core_publishes_hm_and_inside(self):
        reg_ = fr.FakeReg(gx=3)
        h = Harness(reg_)
        try:
            h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
            h.want('Flow')
            snap = gx(devices=[METER, FRIDGE])

            def ok():
                snap['at'] = time.monotonic()
                h.mgr.set_gx_side(snap)
                t = h.pub['/Flow/Json']
                return t and json.loads(t).get('hm') == 1 and json.loads(t)['why'] == 'gap'
            h.until(ok, 8 + xc.FLOW_IMBALANCE_S, 'metered House and its gap, once held')
            f = json.loads(h.pub['/Flow/Json'])
            self.assertEqual((f['house'], f['why'], {x['id']: x.get('in') for x in f['rows']}['dcload:279']),
                             (24.0, 'gap', 1))
            self.assertAlmostEqual(f['x'], 68.0 + 22.0 - 50.0 - 6.0 - 24.0)
        finally:
            h.close()
            reg_.close()


class TestDcLoadModes(unittest.TestCase):
    # A DC load meter's monitor mode names its row when the owner gave none (gui-v2 1.3.22 enums.h/enums.cpp; the
    # Venus OS wiki's D-Bus page for 2 and 6)
    def test_named_by_mode(self):
        want = {1: 'SmartShunt 500A/50mV', 2: 'Electric drive', 3: 'Fridge', 4: 'Water pump', 5: 'Bilge pump',
                8: 'Water heater', 99: 'SmartShunt 500A/50mV', None: 'SmartShunt 500A/50mV'}
        for mode, name in want.items():
            with self.subTest(mode=mode):
                d = dev('dcload', 'dcload:310', -3.0, '', 'SmartShunt 500A/50mV', mode=mode)
                m, devs = model(reg(), gx(devices=[d]))
                self.assertEqual((rows(m)['dcload:310']['k'], rows(m)['dcload:310']['c']), ('dcload', name))
                self.assertEqual(xc.device_post(devs, None)[0][:3],
                                 ['dcload', name, '' if name == 'SmartShunt 500A/50mV' else 'SmartShunt 500A/50mV'])
        # the owner's own name wins
        d = dev('dcload', 'dcload:310', -3.0, 'Galley fridge', 'SmartShunt 500A/50mV', mode=3)
        self.assertEqual(rows(model(reg(), gx(devices=[d]))[0])['dcload:310']['c'], 'Galley fridge')
        # remembered under the mode's name
        mgr = mgr_with({})
        try:
            mgr._remember(gx(devices=[dev('dcload', 'dcload:311', -3.0, '', 'SmartShunt 500A/50mV', mode=4)]), [])
            self.assertEqual(mgr.gx_memory['dcload:311'], {'k': 'dcload', 'n': 'Water pump', 'p': 'SmartShunt 500A/50mV',
                                                           'r': 'l'})
        finally:
            mgr.stop()

    def test_inverter_mode_is_an_inverter_row(self):
        d = dev('dcload', 'dcload:312', -9.0, '', 'SmartShunt 500A/50mV', mode=7)
        m, devs = model(reg(batt=50.0), gx(devices=[d]))
        r = rows(m)['dcload:312']
        self.assertEqual((r['k'], r['a'], r['c']), ('inverter', 9.0, 'SmartShunt 500A/50mV'))
        self.assertAlmostEqual(m['house'], 90.0 - 50.0 - 9.0)
        self.assertEqual(xc.device_post(devs, None), [['inverter', 'SmartShunt 500A/50mV', '', -9.0, 0.3, 1]])
        # inside a DC system meter as every dcload service is
        m, _ = model(reg(batt=50.0), gx(devices=[d, dict(METER, amps=-40.0)]))
        self.assertEqual((rows(m)['dcload:312'].get('in'), m['house']), (1, 31.0))


SHUNT_DCDC = dev('dcsource', 'dcsource:301', 12.0, '', 'SmartShunt 300A/50mV', mode=-3)


class TestDcMeterDcDcMode(unittest.TestCase):
    # A SmartShunt in DC-DC mode reads + into the house bank (a DC source); its sign puts it on a side, no switch
    def test_feeding_is_a_source(self):
        m, devs = model(reg(batt=60.0), gx(devices=[SHUNT_DCDC]))
        self.assertEqual((rows(m)['dcsource:301']['k'], rows(m)['dcsource:301']['a']), ('dcdc', 12.0))
        self.assertAlmostEqual(m['house'], 90.0 + 12.0 - 60.0)
        self.assertEqual(xc.device_post(devs, None)[0][::3], ['dcdc', 12.0])

    def test_drawing_is_a_load(self):
        m, devs = model(reg(batt=60.0), gx(devices=[dict(SHUNT_DCDC, amps=-12.0)]))
        self.assertEqual([(x['k'], x['id'], x['a']) for x in m['rows'][2:]], [('dcdcload', 'dcsource:301', 12.0)])
        self.assertAlmostEqual(m['house'], 90.0 - 60.0 - 12.0)
        self.assertEqual(xc.device_post(devs, None)[0][::3], ['dcdc', -12.0])

    def test_deadband_keeps_the_last_side(self):
        for mem, side in ((None, 'dcdc'), ({'dcsource:301': {'k': 'dcdc', 'n': '', 'p': 'S', 'r': 'l'}}, 'dcdcload'),
                          ({'dcsource:301': {'k': 'dcdc', 'n': '', 'p': 'S', 'r': 's'}}, 'dcdc')):
            for amps in (-0.4, 0.0, 0.3):
                m, _ = model(reg(), gx(devices=[dict(SHUNT_DCDC, amps=amps)]), mem)
                self.assertEqual((rows(m)['dcsource:301']['k'], rows(m)['dcsource:301']['a']), (side, 0.0))
        # remembered while drawing, then gone: a load at 0.0 A
        mgr = mgr_with({})
        try:
            mgr._remember(gx(devices=[dict(SHUNT_DCDC, amps=-12.0)]), [])
            self.assertEqual(mgr.gx_memory['dcsource:301']['r'], 'l')
            mgr._remember(gx(devices=[dict(SHUNT_DCDC, amps=0.2)]), [])
            self.assertEqual(mgr.gx_memory['dcsource:301']['r'], 'l')       # idle keeps its side
            m, _ = model(reg(), gx(), mgr.gx_memory)
            self.assertEqual((rows(m)['dcsource:301']['k'], rows(m)['dcsource:301']['a']), ('dcdcload', 0.0))
        finally:
            mgr.stop()

    def test_no_switch_and_an_old_flag_is_harmless(self):
        # an older bridge offered the switch for it and may have stored "h": ignored, then cleared
        old = {'dcsource:301': {'k': 'dcdc', 'n': '', 'p': 'SmartShunt 300A/50mV', 'r': 's', 'h': 1}}
        m, _ = model(reg(), gx(devices=[SHUNT_DCDC]), old)
        self.assertEqual((rows(m)['dcsource:301']['k'], rows(m)['dcsource:301']['a']), ('dcdc', 12.0))
        m, _ = model(reg(), gx(), old)
        self.assertEqual(rows(m)['dcsource:301']['k'], 'dcdc')
        mgr = mgr_with(dict(old, **ORION_MEM))
        try:
            self.assertEqual(mgr.gx_memory['dcsource:301'], {'k': 'dcdc', 'n': '', 'p': 'SmartShunt 300A/50mV', 'r': 's'})
            pub = tb.Pub(None, mgr.static_paths())
            mgr.tick(pub)
            self.assertEqual({d['key']: d.get('h') for d in json.loads(pub['/GxDevices'])},
                             {'dcdc:293': 0, 'dcsource:301': None})
            self.assertFalse(mgr.on_write('/GxDrawsHouse', json.dumps({'key': 'dcsource:301', 'on': 1})))
            self.assertTrue(mgr.on_write('/GxDrawsHouse', json.dumps({'key': 'dcdc:293', 'on': 1})))
        finally:
            mgr.stop()


class TestDemoOrionAndFridge(unittest.TestCase):
    # FakeGx's Orion XS "Start battery charger" drawing from the house bank (it marks itself, "h": 1, as the demo keeps
    # no memory) and its SmartShunt in DC load mode named "Fridge"
    ORION_FAKE = {'service': 'demo.orion', 'cls': 'dcdc', 'key': 'dcdc:282', 'name': 'Start battery charger',
                  'product': 'Orion XS 12/12-50A', 'serial': '', 'amps': 14.0, 'pv': None, 'age': 0.4, 'h': 1,
                  'in_amps': 15.6, 'in_age': 0.4}
    FRIDGE_FAKE = {'service': 'demo.fridge', 'cls': 'dcload', 'key': 'dcload:283', 'name': 'Fridge',
                   'product': 'SmartShunt 500A/50mV', 'serial': '', 'mode': 3, 'amps': -4.2, 'pv': None, 'age': 0.4}

    def check(self, g):
        devs = xc.gx_devices(g, None, fr.UID, [fr.UID])
        m = xc.flow_model({'alt': 48.0, 'batt': None, 'solar': None, 'src': 3, 'ser': 0}, g, devs, fr.UID, [fr.UID])
        loads = [(x['k'], x['c']) for x in m['rows'] if x['k'] in xc.FLOW_LOAD_KINDS]
        self.assertIn(('dcdcload', 'Start battery charger'), loads)
        self.assertIn(('dcload', 'Fridge'), loads)
        post = {p[1]: p for p in xc.device_post(devs, g)}
        self.assertLess(post['Start battery charger'][3], 0)
        self.assertEqual(post['Start battery charger'][0], 'dcdc')
        self.assertLess(post['Fridge'][3], 0)
        self.assertEqual(post['Fridge'][0], 'dcload')
        return m

    def test_model_handles_them(self):
        g = gx(batt=20.0, devices=[self.ORION_FAKE, self.FRIDGE_FAKE])
        m = self.check(g)
        r = rows(m)
        self.assertEqual((r['dcdc:282']['a'], r['dcload:283']['a']), (15.6, 4.2))
        # named only by its mode, the Fridge reads the same
        g = gx(batt=20.0, devices=[self.ORION_FAKE, dict(self.FRIDGE_FAKE, name='')])
        self.check(g)
        # a real Orion without the owner's flag stays a source: "h" counts only on FakeGx's own device dicts and memory
        g = gx(batt=20.0, devices=[dict(self.ORION_FAKE, h=0)])
        self.assertEqual(rows(model(reg(), g)[0])['dcdc:282']['k'], 'dcdc')

    def test_fakegx_if_it_has_them(self):
        # Agent A's FakeGx (2026-10-29): the shore charger, fridge and Orion come from the regulator's simulation
        f = xc.FakeGx(7)
        g = None
        for k in range(400):
            g = f.step(1000.0 + k * 0.5, {'alt': 48.0, 'solar': 21.0, 'batt': 30.0, 'shore': 0.0, 'fridge': 4.2,
                                          'orion': 15.0}, heading=200.0)
        m = self.check(g)
        r = rows(m)
        self.assertEqual((r['dcdc:282']['a'], r['dcdc:282']['c'], r['dcload:283']['a'], r['dcload:283']['c']),
                         (15.0, 'Start battery charger', 4.2, 'Fridge'))


# ---- 2026-10-29, approved proposals: an alternator with no serial after the wait, and what is wired through the DC
# system meter (the owner's switch) ----

def wait_dev(serial='', since=0.0, n2k=None, key='alternator:7', product='Alternator', svc=None):
    return dict(dev('alternator', key, 25.0, '', product, serial, svc=svc), since=since, n2k=n2k)


class TestAltSerialWait(unittest.TestCase):
    def test_unique_number_is_the_chip_ids_low_21_bits(self):
        # the bench regulator: uid 000088621AD9B4DC, vecan-dbus service ..._uc1684700, /N2kUniqueNumber 1684700
        self.assertEqual(xc.n2k_unique('000088621AD9B4DC'), 1684700)
        self.assertIsNone(xc.n2k_unique('not hex'))

    def test_listed_after_the_wait(self):
        for since, listed in ((0.0, False), (xc.ALT_SERIAL_WAIT_S - 0.1, False), (xc.ALT_SERIAL_WAIT_S, True), (300.0, True)):
            with self.subTest(since=since):
                m, devs = model(reg(), gx(devices=[wait_dev(since=since, n2k=777)]), uids=[fr.UID])
                self.assertEqual('alternator:7' in rows(m), listed)
                self.assertEqual([p[0] for p in xc.device_post(devs, None)], ['altx'] if listed else [])

    def test_the_twin_never_by_its_unique_number(self):
        u = xc.n2k_unique(fr.UID)
        for since in (0.0, 31.0, 3600.0):
            m, devs = model(reg(), gx(devices=[wait_dev(since=since, n2k=u, key='alternator:0')]), uids=[fr.UID])
            self.assertEqual((devs, [x['id'] for x in m['rows']]), ([], ['alt', 'solar']))
        # a remembered device under its key is not listed as absent either: the key is the twin's now
        mem = {'alternator:0': {'k': 'altx', 'n': 'Port', 'p': 'Something', 'r': 's', 's': '1F2E3D4C'}}
        _, devs = model(reg(), gx(devices=[wait_dev(since=31.0, n2k=u, key='alternator:0')]), mem, uids=[fr.UID])
        self.assertEqual(devs, [])
        # once its serial reads, the serial decides
        _, devs = model(reg(), gx(devices=[wait_dev('1F2E3D4C', since=31.0, n2k=u, key='alternator:0')]), uids=[fr.UID])
        self.assertEqual([d['key'] for d in devs], ['alternator:0'])

    def test_remembered_and_dropped(self):
        u = xc.n2k_unique(fr.UID)
        mgr = mgr_with({})
        try:
            mgr._remember(gx(devices=[wait_dev(since=10.0, n2k=777)]), [fr.UID])
            self.assertEqual(mgr.gx_memory, {})                                  # still waiting
            mgr._remember(gx(devices=[wait_dev(since=31.0, n2k=777)]), [fr.UID])
            self.assertEqual(mgr.gx_memory['alternator:7'], {'k': 'altx', 'n': '', 'p': 'Alternator', 'r': 's', 'w': 1,
                                                             'u': 777})
            # not the bench round 7 identity: it stays through the purge, and listed at 0.0 A when gone
            mgr._remember(gx(), [fr.UID])
            self.assertIn('alternator:7', mgr.gx_memory)
            self.assertEqual([(d['key'], d['on']) for d in xc.gx_devices(gx(), mgr.gx_memory, fr.UID, [fr.UID])],
                             [('alternator:7', 0)])
            # its serial reads later: stored with it
            mgr._remember(gx(devices=[wait_dev('1F2E3D4C', since=40.0, n2k=777)]), [fr.UID])
            self.assertEqual(mgr.gx_memory['alternator:7'], {'k': 'altx', 'n': '', 'p': 'Alternator', 'r': 's',
                                                             's': '1F2E3D4C'})
            # the twin by its unique number: never stored, and a stored entry under its key goes
            mgr._remember(gx(devices=[wait_dev(since=99.0, n2k=u, key='alternator:0')]), [fr.UID])
            self.assertNotIn('alternator:0', mgr.gx_memory)
            mgr.gx_memory['alternator:0'] = {'k': 'altx', 'n': '', 'p': 'Orion', 'r': 's'}
            mgr._remember(gx(devices=[wait_dev(since=99.0, n2k=u, key='alternator:0')]), [fr.UID])
            self.assertNotIn('alternator:0', mgr.gx_memory)
        finally:
            mgr.stop()
        # stored with a pinned regulator's unique number and no serial: dropped on load
        stored = {'alternator:8': {'k': 'altx', 'n': '', 'p': 'Alternator', 'r': 's', 'w': 1, 'u': u},
                  'alternator:9': {'k': 'altx', 'n': '', 'p': 'Alternator', 'r': 's', 'w': 1, 'u': 777}}
        m = mgr_with(stored, units=fr.UID)
        try:
            self.assertEqual(sorted(m.gx_memory), ['alternator:9'])
        finally:
            m.stop()

    def test_reader_counts_the_wait_and_reads_the_unique_number(self):
        # TestGxReader installs its own D-Bus stubs later: a reader imported here is dropped again, so it imports one
        # against those
        had = 'xregulator_bridge' in sys.modules
        if not had:
            tb.install_stubs()
        sys.path.insert(1, tb.BRIDGE + '/ext/velib_python')
        import importlib
        br = importlib.import_module('xregulator_bridge')
        if not had:
            self.addCleanup(sys.modules.pop, 'xregulator_bridge', None)
        named = {'/ProductName': 'Alternator', '/CustomName': '', '/DeviceInstance': 0}
        values = {'com.victronenergy.system': {},
                  TWIN_SVC: dict(named, **{'/Dc/0/Current': 1.0}),                                   # no serial, no path
                  'com.victronenergy.alternator.socketcan_vecan0_vi0_uc55': dict(named, **{
                      '/DeviceInstance': 1, '/Dc/0/Current': 2.0, '/N2kUniqueNumber': 99})}
        now = [100.0]
        bus, _ = TestGxReader.bus(self, values)
        r = br.GxReader(bus, lambda: now[0])
        r.set_names(list(values))
        r.poll()
        now[0] = 112.0
        r.poll()
        by = {d['key']: d for d in r.snapshot()['devices']}
        self.assertEqual((by['alternator:0']['n2k'], by['alternator:0']['since']), (1684700, 12.0))   # from the name
        self.assertEqual(by['alternator:1']['n2k'], 99)                                               # from the path
        r.idle()                                                           # reading stopped: the wait starts over
        now[0] = 500.0
        r.poll()
        self.assertEqual(r.snapshot()['devices'][0]['since'], 0.0)
        # the bench regulator pinned: its twin never listed, however long it waits
        now[0] = 600.0
        devs = xc.gx_devices(r.snapshot(), None, '000088621AD9B4DC', ['000088621AD9B4DC'])
        self.assertEqual([d['key'] for d in devs], ['alternator:1'])


def model_m(r, g, memory=None, uid=None, uids=(), peers=()):
    # model() with the memory handed to flow_model too, for this regulator's own alternator ("xreg:<uid>")
    uid = uid or fr.UID
    devs = xc.gx_devices(g, memory, uid, uids, peers)
    return xc.flow_model(r, g, devs, uid, uids, memory), devs


class TestThroughTheMeter(unittest.TestCase):
    # Sources 68 + 22 = 90, battery 50: 40 A of loads. The DC system meter reads what passes through it.
    ALT_IN = {'xreg:' + fr.UID: {'k': 'reg', 'm': 1}}

    def test_alternator_wired_through_the_meter(self):
        # the house uses 40 A and the alternator's 68 A comes in on the meter's side: the meter reads 40 - 68 = -28
        g = gx(devices=[dict(METER, amps=28.0)])
        m, _ = model_m(reg(batt=50.0), g)
        self.assertEqual((m['house'], m['why']), (0.0, 'gap'))                   # without the owner's word
        self.assertAlmostEqual(m['x'], 68.0)
        m, _ = model_m(reg(batt=50.0), g, self.ALT_IN)
        self.assertEqual((m['hm'], m['house'], m['why'], rows(m)['alt'].get('in')), (1, 40.0, '', 1))
        # engine stopped, 0 A: the same meter reads the house alone
        m, _ = model_m(reg(batt=-18.0, alt=0.0), gx(devices=[dict(METER, amps=-40.0)]), self.ALT_IN)
        self.assertEqual((m['house'], m['why']), (40.0, ''))

    def test_dc_load_meter_outside_the_meter(self):
        # the fridge (6 A) wired straight to the battery: the meter reads the other 34 A
        g = gx(devices=[dict(METER, amps=-34.0), FRIDGE])
        m, _ = model_m(reg(batt=50.0), g)
        self.assertEqual((m['house'], m['why']), (28.0, 'gap'))                  # Victron's layout assumed it inside
        out = {'dcload:279': {'k': 'dcload', 'n': 'Fridge', 'p': 'SmartShunt 500A/50mV', 'r': 'l', 'm': 0}}
        m, _ = model_m(reg(batt=50.0), g, out)
        self.assertEqual((m['house'], m['why'], rows(m)['dcload:279'].get('in')), (34.0, '', None))

    def test_any_device_either_way(self):
        # an inverter fed from the DC panel (inside) and an Orion feeding the house through it (inside)
        phoenix = dev('inverter', 'inverter:281', -8.0, 'Galley', 'Phoenix Inverter 12/800 Smart')
        feeds = dev('dcdc', 'dcdc:294', 15.0, 'From start battery', 'Orion XS 12/12-50A Charger')
        mem = {'inverter:281': {'k': 'inverter', 'n': 'Galley', 'p': 'Phoenix', 'r': 'l', 'm': 1},
               'dcdc:294': {'k': 'dcdc', 'n': 'From start battery', 'p': 'Orion', 'r': 's', 'm': 1}}
        # loads 40 + 15 = 55 in all, 8 of them the inverter; the meter sees 55 - 15 = 40, the inverter included
        g = gx(devices=[dict(METER, amps=-40.0), phoenix, feeds])
        m, _ = model_m(reg(batt=50.0), g, mem)
        r = rows(m)
        self.assertEqual((r['inverter:281'].get('in'), r['dcdc:294'].get('in'), m['why']), (1, 1, ''))
        self.assertAlmostEqual(m['house'], 40.0 + 15.0 - 8.0)
        # another pinned regulator's alternator through it
        peers = [{'uid': fr.UID, 'name': 'Port', 'amps': 68.0, 'live': True},
                 {'uid': UID_B, 'name': 'Stbd', 'amps': 30.0, 'live': True}]
        mem = {'xreg:' + UID_B: {'k': 'reg', 'm': 1}}
        m, _ = model_m(reg(batt=50.0), gx(devices=[dict(METER, amps=-40.0)]), mem, uids=[fr.UID, UID_B], peers=peers)
        self.assertEqual((rows(m)['xreg:' + UID_B].get('in'), m['why']), (1, ''))
        self.assertAlmostEqual(m['house'], 70.0)

    def test_the_switch_on_the_bridge(self):
        mem = dict(ORION_MEM, **{'dcload:279': {'k': 'dcload', 'n': 'Fridge', 'p': 'SmartShunt', 'r': 'l'},
                                 'solarcharger:278': {'k': 'solar', 'n': '', 'p': 'SmartSolar', 'r': 'p'}})
        mgr = mgr_with(mem, units=fr.UID)
        try:
            pub = tb.Pub(None, mgr.static_paths())
            mgr.tick(pub)
            gxd = lambda: {d['key']: d for d in json.loads(pub['/GxDevices'])}
            # no DC system meter on the GX: no switch, no regulator rows
            self.assertEqual({k: d.get('m') for k, d in gxd().items()},
                             {'dcdc:293': None, 'dcload:279': None, 'solarcharger:278': None})
            mgr._remember(gx(devices=[METER]), [fr.UID])
            mgr.tick(pub)
            self.assertEqual({k: (d.get('m'), d.get('reg')) for k, d in gxd().items()},
                             {'xreg:' + fr.UID: (0, 1), 'dcdc:293': (0, None), 'dcload:279': (1, None),
                              'solarcharger:278': (None, None)})
            self.assertEqual(list(gxd())[0], 'xreg:' + fr.UID)                  # the regulators first
            for bad in ('x', '{"key":"dcdc:999","on":1}', '{"key":"solarcharger:278","on":1}',
                        '{"key":"dcdc:293","on":2}', '{"key":"xreg:%s","on":1}' % UID_B, '[1]'):
                self.assertFalse(mgr.on_write('/GxMetered', bad), bad)
            self.assertTrue(mgr.on_write('/GxMetered', json.dumps({'key': 'xreg:' + fr.UID, 'on': 1})))
            self.assertTrue(mgr.on_write('/GxMetered', json.dumps({'key': 'dcload:279', 'on': 0})))
            mgr.tick(pub)
            self.assertEqual((gxd()['xreg:' + fr.UID]['m'], gxd()['dcload:279']['m'], pub['/GxMetered']), (1, 0, ''))
            # kept through a reading that rewrites the entry, and a restart
            mgr._remember(gx(devices=[METER, dict(FRIDGE, name='Galley fridge')]), [fr.UID])
            self.assertEqual(mgr.gx_memory['dcload:279'], {'k': 'dcload', 'n': 'Galley fridge',
                                                           'p': 'SmartShunt 500A/50mV', 'r': 'l', 'm': 0})
            saved = mgr.store.get('gx_devices')
        finally:
            mgr.stop()
        m2 = mgr_with(json.loads(saved), units=fr.UID)
        try:
            self.assertEqual((m2.gx_memory['xreg:' + fr.UID], m2.gx_memory['dcload:279']['m']),
                             ({'k': 'reg', 'm': 1}, 0))
            m, _ = model_m(reg(batt=50.0), gx(devices=[dict(METER, amps=28.0)]), m2.gx_memory)
            self.assertEqual(rows(m)['alt'].get('in'), 1)
        finally:
            m2.stop()

    def test_core_hands_its_memory_to_the_model(self):
        reg_ = fr.FakeReg(gx=3)
        h = Harness(reg_)
        try:
            h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
            h.want('Flow')
            h.mgr.gx_memory['xreg:' + reg_.uid] = {'k': 'reg', 'm': 1}
            snap = gx(devices=[dict(METER, amps=-40.0 + 68.0)])

            def ok():
                snap['at'] = time.monotonic()
                h.mgr.set_gx_side(snap)
                t = h.pub['/Flow/Json']
                return t and json.loads(t).get('hm') == 1
            h.until(ok, 8, 'metered House')
            f = json.loads(h.pub['/Flow/Json'])
            self.assertEqual((f['house'], f['why'], f['rows'][0].get('in')), (40.0, '', 1))
        finally:
            h.close()
            reg_.close()


    def test_header_waits_for_the_new_boats_wind(self):
        r = fr.FakeReg(gx=3)
        h = Harness(r)
        try:
            h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
            h.until(lambda: h.pub['/Live/TrueWind'] == 12.0, 8, 'wind')
            seen = []
            orig = r.frame

            def frame(ev, t):
                # CSV4 stops for a while after the switch, as at the bench's demo exit
                if ev == 'CSVData4' and r.csv3['hardwarePresent'] == 0 and time.time() < hold[0]:
                    return None
                return orig(ev, t)
            hold = [0.0]
            r.frame = frame
            hold[0] = time.time() + 3.0
            r.csv3['hardwarePresent'] = 0
            r.csv3_dirty.set()
            h.until(lambda: h.core.demo, 10, 'demo')
            for _ in range(15):
                h.tick()
                seen.append(h.pub['/Live/TrueWind'])
                time.sleep(0.1)
            self.assertTrue(all(v is None for v in seen), seen)   # the other boat's 12.0 kn is not shown
            h.until(lambda: h.pub['/Live/TrueWind'] == 12.0, 8, 'the new boat\'s wind')
        finally:
            h.close()
            r.close()


# ---- bench round 8 (2026-10-29): the Batt chip against the tile, and a one-poll "unmeasured" note ----

class TestBattChipFollowsTile(unittest.TestCase):
    # The Dashboard's Batt chip (/Live/BattA) and the tile's battery are one reading, published in the same tick. The fake
    # regulator sends Bcur 25.00 A on CSV1 and flowBattA 50.00 A on CSV4, as two readings apart in time would differ.
    def setUp(self):
        self.reg = fr.FakeReg(gx=3)
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def chip_and_tile(self):
        t = self.h.pub['/Flow/Json']
        return self.h.pub['/Live/BattA'], (json.loads(t)['b']['a'] if t else None)

    def test_same_reading_every_tick(self):
        h, r = self.h, self.reg
        h.want('Flow')
        h.until(lambda: self.chip_and_tile()[1] == 50.0, 8, 'tile battery')
        pairs = []
        for k in range(30):
            if k == 10:
                r.flow['flowBattA'] = 2650                 # the step: the charger unplugged, 50.0 -> 26.5 A
            h.tick()
            pairs.append(self.chip_and_tile())
            time.sleep(0.1)
        self.assertEqual([p for p in pairs if p[0] != p[1]], [])
        self.assertEqual(pairs[-1], (26.5, 26.5))
        # the tile not shown: the chip still follows the flow group's battery, the tile's next reading
        h.core.want_at.clear()
        h.tick_for(0.5)
        self.assertEqual((h.pub['/Live/BattA'], h.pub['/Flow/Json']), (26.5, ''))

    def test_alt_chip_same_reading_every_tick(self):
        # the Alt chip (/Live/AltAmps) and the tile's alternator row: CSV1 MeasuredAmps is about 50 A here, CSV4
        # flowAltA 68.00 A
        h, r = self.h, self.reg
        h.want('Flow')
        alt = lambda: (h.pub['/Live/AltAmps'], json.loads(h.pub['/Flow/Json'])['rows'][0]['a'] if h.pub['/Flow/Json']
                       else None)
        h.until(lambda: alt()[1] == 68.0, 8, 'tile alternator')
        pairs = []
        for k in range(30):
            if k == 10:
                r.flow['flowAltA'] = 3000                  # the field cut back: 68.0 -> 30.0 A
            if k == 20:
                r.flow['flowAltA'] = 30                    # 0.3 A, inside the tile's 0.5 A deadband
            h.tick()
            pairs.append(alt())
            time.sleep(0.1)
        self.assertEqual([p for p in pairs if p[0] != p[1]], [])
        self.assertIn((30.0, 30.0), pairs)
        self.assertEqual(pairs[-1], (0.0, 0.0))

    def test_no_flow_group_keeps_csv1(self):
        # firmware without the flow group, or no CSV4: the chip reads CSV1's Bcur as before
        h = self.h
        frames, fresh, info = h.core.link.snapshot()
        frames = dict(frames, CSVData4={k: v for k, v in (frames.get('CSVData4') or {}).items() if k != 'flowBattA'})
        pub = h.pubs[h.main]
        try:
            h.core._live(pub, frames, fresh, True, info)
        except TypeError:
            h.core._live(pub, frames, fresh, True)   # before the write-hold change (2026-10-29)
        h.core._flow(pub, time.monotonic(), frames, fresh)
        self.assertEqual((pub['/Live/BattA'], pub['/Flow/Ok']), (25.0, 0))
        self.assertAlmostEqual(pub['/Live/AltAmps'], 50.0, delta=1.0)    # CSV1 MeasuredAmps, 50 A +-1

    def test_demo_chip_is_the_made_up_monitor(self):
        h, r = self.h, self.reg
        h.want('Flow')
        r.csv3['hardwarePresent'] = 0
        r.csv3_dirty.set()
        h.until(lambda: h.core.demo and self.chip_and_tile()[1] is not None
                and json.loads(h.pub['/Flow/Json'])['b']['from'] == 'GX', 10, 'demo tile')
        for _ in range(15):
            h.tick()
            chip, tile = self.chip_and_tile()
            self.assertEqual(chip, tile)
            self.assertNotEqual(chip, 25.0)
            self.assertEqual(h.pub['/Live/AltAmps'], json.loads(h.pub['/Flow/Json'])['rows'][0]['a'])
            time.sleep(0.1)


class TestImbalanceHold(unittest.TestCase):
    # A step in one input seen before the matching step in another is no unmeasured source or load
    def setUp(self):
        self.reg = fr.FakeReg(gx=3)
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')
        self.h.want('Flow')
        # anchored: no alternator, 22 A of sun, 15.5 A into the battery: House 6.5 A
        self.reg.flow.update(flowAltA=0, flowBattA=1550)
        self.snap = gx()
        self.seen = []

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def run_for(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.snap['at'] = time.monotonic()
            self.h.mgr.set_gx_side(self.snap)
            self.h.tick()
            t = self.h.pub['/Flow/Json']
            if t:
                self.seen.append((time.monotonic(), json.loads(t)))
            time.sleep(0.1)

    def notes(self):
        return [(t, f['why'], f['x']) for t, f in self.seen if f['why'] in ('unmeasured', 'gap')]

    def test_fridge_start_seen_before_the_battery(self):
        h, r = self.h, self.reg
        h.until(lambda: h.pub['/Flow/Json'] and json.loads(h.pub['/Flow/Json'])['house'] == 6.5, 8, 'house 6.5')
        # the fridge's 12.4 A start on the GX two seconds before the battery reading takes it (bench round 8, 13:00:37)
        self.snap['devices'] = [dict(FRIDGE, amps=-12.4)]
        self.run_for(2.0)
        r.flow['flowBattA'] = 1550 - 1240
        self.run_for(2.5)
        self.assertEqual(self.notes(), [])
        f = self.seen[-1][1]
        self.assertEqual((f['house'], f['why']), (6.5, ''))
        # meanwhile House read the clamped sum, never "--"
        self.assertTrue(all(f['house'] is not None for _, f in self.seen))

    def test_a_real_unmeasured_source_shows_after_the_hold(self):
        h, r = self.h, self.reg
        h.until(lambda: h.pub['/Flow/Json'] and json.loads(h.pub['/Flow/Json'])['house'] == 6.5, 8, 'house 6.5')
        r.flow["flowBattA"] = 1550 + 1200        # the battery takes 5.5 A more than the measured sources give
        self.run_for(xc.FLOW_IMBALANCE_S + 2.5)
        # timed from the first tile with the new battery reading (the fake sends CSV4 every 1 s)
        t0 = next(t for t, f in self.seen if f['b']['a'] == 27.5)
        notes = self.notes()
        self.assertTrue(notes)
        self.assertGreaterEqual(notes[0][0] - t0, xc.FLOW_IMBALANCE_S - 0.05)
        self.assertLess(notes[0][0] - t0, xc.FLOW_IMBALANCE_S + 0.5)
        self.assertEqual((notes[-1][1], round(notes[-1][2], 1)), ('unmeasured', 5.5))
        # it ends at once when the source is gone
        r.flow['flowBattA'] = 1550
        self.run_for(1.0)
        self.assertEqual(self.seen[-1][1]['why'], '')

    def test_meter_gap_held_too(self):
        h = self.h
        meter = dict(METER, amps=-6.5)
        self.snap['devices'] = [meter]
        self.run_for(1.5)
        self.assertEqual((self.seen[-1][1]['hm'], self.seen[-1][1]['house']), (1, 6.5))
        # the meter takes a 12 A step two seconds before the battery reading
        meter['amps'] = -18.5
        self.run_for(2.0)
        self.reg.flow['flowBattA'] = 1550 - 1200
        self.run_for(2.5)
        self.assertEqual(self.notes(), [])
        # a load no meter sees, for good: named after the hold
        t0 = time.monotonic()
        self.reg.flow['flowBattA'] = 1550 - 1200 - 900
        self.run_for(xc.FLOW_IMBALANCE_S + 1.5)
        notes = [n for n in self.notes() if n[0] >= t0]
        self.assertTrue(notes and notes[0][0] - t0 >= xc.FLOW_IMBALANCE_S)
        self.assertEqual((notes[-1][1], round(notes[-1][2], 1)), ('gap', 9.0))


class TestPerfSettings(unittest.TestCase):
    def test_ridge_and_idw_published(self):
        r = fr.FakeReg(gx=3)
        h = Harness(r)
        try:
            h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
            h.want('Perf')
            core = h.core
            # the PerfSettings frame as the regulator echoes it
            with core.link.lock:
                core.link.frames['PerfSettings'] = {'perfRidgeFrac': 0.25, 'perfIdwPower': 3.0}
            h.tick_for(0.3)
            self.assertEqual((h.pub['/Perf/RidgeFrac'], h.pub['/Perf/IdwPower']), (0.25, 3.0))
        finally:
            h.close()
            r.close()


# ---- fwfix lane 2026-10-29: apparent wind with no reading (TS age gate), the /gxDevices interval from GxRate "devPost"
# (both directions, and firmware without it), FakeGx's charger, fridge and Orion from the regulator's CSV4, and the
# made-up devices never reaching the remembered-device store ----

series = tb.series


class TestApparentWindNoData(unittest.TestCase):
    def setUp(self):
        self.old = xc.WIND_EVERY_S
        xc.WIND_EVERY_S = 1
        self.reg = fr.FakeReg()
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')

    def tearDown(self):
        xc.WIND_EVERY_S = self.old
        self.h.close()
        self.reg.close()

    def app(self):
        return [x for x in series(self.h.pub['/History/AppWind'])[1:] if x is not None]

    def test_never_seen_is_no_data(self):
        # older firmware: CSV4 sends 0 for a wind instrument it never heard, TS says 999999
        h, r = self.h, self.reg
        h.until(lambda: h.pub['/Live/AppWind'] == 15.0, 10, 'apparent wind while fresh')
        r.ts.update(ts_AppWindSpeed=999999, ts_AppWindAngle=999999)
        h.until(lambda: h.pub['/Live/AppWind'] is None, 10, 'never seen: none')
        h.tick_for(3.0)
        n = len(self.app())
        h.tick_for(3.5)
        self.assertEqual(len(self.app()), n)                       # no new apparent means while it reads none
        self.assertEqual(h.pub['/Live/TrueWind'], 12.0)            # true wind has its own marker and is untouched

    def test_stale_is_no_data_and_fresh_comes_back(self):
        h, r = self.h, self.reg
        r.ts.update(ts_AppWindSpeed=xc.WIND_STALE_MS + 1)
        h.until(lambda: h.pub['/Live/AppWind'] is None, 10, 'stale: none')
        r.ts.update(ts_AppWindSpeed=1800)
        h.until(lambda: h.pub['/Live/AppWind'] == 15.0, 10, 'fresh again')

    def test_empty_marker_is_no_data(self):
        # firmware 2026-10-29: the marker itself, whatever TS says
        h, r = self.h, self.reg
        r.wind_demo_only = True
        h.until(lambda: h.pub['/Live/AppWind'] is None and h.pub['/Live/TrueWind'] is None, 10, 'marker')

    def test_plot_skips_stale_apparent(self):
        rec = xc.WindRecorder()
        frame = {'TrueWindSpeedNMEA': 1200, 'TrueWindAngleNMEA': 170, 'ApparentWindSpeedNMEA': 0,
                 'ApparentWindAngleNMEA': 0}
        stale = xc.wind_stale({'TimestampData': {'ts_AppWindSpeed': 999999, 'ts_AppWindAngle': 999999}},
                              {'TimestampData'})
        self.assertEqual(stale, {'ApparentWindSpeedNMEA', 'ApparentWindAngleNMEA'})
        rec.sample(frame, stale)
        rec.close()
        self.assertEqual({k: v[-1] for k, v in rec.series.items()}, {'Tws': 12.0, 'Twa': 170.0, 'Aws': None, 'Awa': None})
        # a TS frame that is not fresh judges nothing
        self.assertEqual(xc.wind_stale({'TimestampData': {'ts_AppWindSpeed': 999999}}, set()), set())

    def test_frame_after(self):
        ts = {'ts_sendMs': 1000, 'ts_sessionId': 7}
        self.assertTrue(xc.frame_after({'sendMs': 1001, 'sessionId': 7}, ts))
        self.assertFalse(xc.frame_after({'sendMs': 1000, 'sessionId': 7}, ts))
        self.assertFalse(xc.frame_after({'sendMs': 999, 'sessionId': 7}, ts))
        self.assertFalse(xc.frame_after({'sendMs': 5000, 'sessionId': 8}, ts))          # another boot
        self.assertTrue(xc.frame_after({'sendMs': 5, 'sessionId': 7}, {'ts_sendMs': 0xFFFFFFF0, 'ts_sessionId': 7}))   # wrap
        self.assertFalse(xc.frame_after({}, ts))

    def test_instrument_coming_on_shows_at_once(self):
        # firmware that sends the empty marker: an instrument that comes on between two TS frames shows with the first
        # CSV4 frame built after the TS frame that still says never seen
        h, r = self.h, self.reg
        r.wind_demo_only = True                                     # no instrument: CSV4 carries the marker
        r.ts.update(ts_AppWindSpeed=999999, ts_AppWindAngle=999999, ts_sendMs=1000, ts_sessionId=7)
        r.csv4_extra.update(sendMs=900, sessionId=7)
        h.until(lambda: h.pub['/Live/AppWind'] is None, 10, 'no instrument')
        h.tick_for(1.0)
        r.wind_demo_only = False                                    # the instrument comes on: 15 kn, TS not sent yet
        r.csv4_extra.update(sendMs=2000)
        h.until(lambda: h.pub['/Live/AppWind'] == 15.0, 5, 'shows before the next TS frame')
        # a TS frame built later that still calls it never seen wins until CSV4 is newer again
        r.ts.update(ts_sendMs=3000)
        h.until(lambda: h.pub['/Live/AppWind'] is None, 10, 'TS newer')

    def test_older_firmware_keeps_the_ts_verdict(self):
        # no empty marker ever seen (older firmware sends 0 for no instrument): a newer CSV4 frame does not override TS
        h, r = self.h, self.reg
        r.ts.update(ts_AppWindSpeed=999999, ts_AppWindAngle=999999, ts_sendMs=1000, ts_sessionId=7)
        r.csv4_extra.update(sendMs=2000, sessionId=7)
        h.until(lambda: h.pub['/Live/AppWind'] is None, 10, 'never seen: none')
        h.tick_for(3.0)
        self.assertIsNone(h.pub['/Live/AppWind'])


class TestPostInterval(unittest.TestCase):
    # GxRate "devPost": a dashboard opening or the last one leaving moves the bridge at once
    tearDown, feed, until = TestDevicePost.tearDown, TestDevicePost.feed, TestDevicePost.until

    def start(self, dev_post_ms=None, dev_every=5):
        self.reg = fr.FakeReg(gx=4)
        self.reg.dev_post_ms, self.reg.dev_every = dev_post_ms, dev_every
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')
        return self.h, self.reg

    def run_feed(self, tick, seconds):
        end = time.time() + seconds
        while time.time() < end:
            tick()
            self.h.tick()
            time.sleep(0.05)

    def test_dashboard_opens_and_closes(self):
        h, reg = self.start(30000, 30)
        tick = self.feed(gx(devices=[CHARGER]))
        self.until(lambda: h.core.devpost_rate == 30.0 and reg.devposts, 'first post at 30 s', tick, timeout=20)
        n = len(reg.devposts)
        self.run_feed(tick, 3.0)
        self.assertEqual(len(reg.devposts), n)                      # 30 s spacing: nothing more yet
        # a page opens: the regulator sends GxRate devPost 5000 at once; the bridge posts now, then every 5 s
        reg.dev_post_ms, reg.dev_every = 5000, 5
        reg.rate_push.set()
        t0 = time.time()
        self.until(lambda: len(reg.devposts) > n, 'post at once', tick)
        self.assertLess(time.time() - t0, 2.0)
        self.assertEqual(h.core.devpost_every, 5.0)
        n = len(reg.devposts)
        t0 = time.time()
        self.until(lambda: len(reg.devposts) > n, 'next at 5 s', tick)
        self.assertAlmostEqual(time.time() - t0, 5.0, delta=1.0)
        # the last page leaves: back to 30 s, no extra post
        reg.dev_post_ms, reg.dev_every = 30000, 30
        reg.rate_push.set()
        self.until(lambda: h.core.devpost_every == 30.0, 'slow again', tick)
        n = len(reg.devposts)
        self.run_feed(tick, 7.0)
        self.assertEqual(len(reg.devposts), n)

    def test_an_answer_does_not_undo_the_stream(self):
        # an answer computed before the page opened (everyS 30) lands after devPost 5000: the stream's value stands
        h, reg = self.start(5000, 30)
        tick = self.feed(gx(devices=[CHARGER]))
        self.until(lambda: h.core.devpost_rate == 5.0 and len(reg.devposts) >= 2, 'two posts', tick, timeout=20)
        self.assertEqual(h.core.devpost_every, 5.0)

    def test_firmware_without_devpost(self):
        # no devPost in GxRate: the answers alone, up to one interval late (the old behaviour)
        h, reg = self.start(None, 30)
        tick = self.feed(gx(devices=[CHARGER]))
        self.until(lambda: reg.devposts and h.core.devpost_every == 30.0, 'answer says 30', tick)
        self.assertIsNone(h.core.devpost_rate)
        n = len(reg.devposts)
        reg.dev_every = 5                                           # a page opened: only the next answer tells
        self.run_feed(tick, 4.0)
        self.assertEqual(len(reg.devposts), n)


class TestDemoDevices(unittest.TestCase):
    setUp, tearDown, flow, wait, feed = (TestCoreFlow.setUp, TestCoreFlow.tearDown, TestCoreFlow.flow,
                                         TestCoreFlow.wait, TestCoreFlow.feed)

    def enter_demo(self):
        h, r = self.h, self.reg
        h.want('Flow')
        r.csv3['hardwarePresent'] = 0
        r.csv3_dirty.set()
        h.until(lambda: h.core.demo, 10, 'demo')
        return h, r

    def test_charger_from_the_regulator(self):
        h, r = self.enter_demo()
        f = self.wait('demo flow', lambda f: f['b']['from'] == 'GX')
        self.assertEqual({x['id']: x['a'] for x in f['rows']}['charger:280'], 0.0)   # no reading: off, never invented
        r.demo_devs.update(demoShoreA=2310)
        r.flow.update(flowBattA=5000 + 2310)
        f = self.wait('charger 23.1', lambda f: {x['id']: x['a'] for x in f['rows']}['charger:280'] == 23.1)
        self.assertAlmostEqual(f['b']['a'], 73.1, delta=0.6)        # the regulator's battery, the charger in it
        r.demo_devs.clear()
        r.flow.update(flowBattA=5000)
        self.wait('charger off', lambda f: {x['id']: x['a'] for x in f['rows']}['charger:280'] == 0.0)

    def test_demo_devices_never_reach_the_store(self):
        h, r = self.enter_demo()
        r.demo_devs.update(demoShoreA=1500, demoFridgeA=450, demoOrionA=520)
        r.flow.update(flowBattA=6500)
        f = self.wait('made-up devices', lambda f: {'dcdc:282', 'dcload:283'} <= {x['id'] for x in f['rows']})
        self.assertEqual({x['id']: x['k'] for x in f['rows']}['dcdc:282'], 'dcdcload')   # drawing, by FakeGx's own flag
        h.tick_for(2.0)
        made_up = {k for _, _, k, *_ in xc.FakeGx.DEVICES}
        self.assertEqual(made_up & set(h.mgr.gx_memory), set())
        self.assertEqual(made_up & set(json.loads(h.store.get('gx_devices') or '{}')), set())
        self.assertEqual(made_up & {d['key'] for d in json.loads(h.mpub['/GxDevices'])}, set())
        # and after the demo, a real GX's readings are remembered without them
        r.csv3['hardwarePresent'] = 1
        r.csv3_dirty.set()
        h.until(lambda: not h.core.demo, 10, 'demo left')
        tick = self.feed(gx(batt=10.0, devices=[CHARGER]))
        h.until(lambda: (tick(), 'charger:280' in h.mgr.gx_memory)[1], 10, 'real charger remembered')
        self.assertEqual(set(h.mgr.gx_memory), {'charger:280'})
        self.assertNotIn('h', h.mgr.gx_memory['charger:280'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
