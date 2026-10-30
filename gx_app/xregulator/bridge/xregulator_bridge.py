#!/usr/bin/env python3
# XREG-010 bridge for Venus OS. Finds the regulators on the GX's networks by uid and talks to each one the owner picked
# over WiFi (its /gxevents stream, or /events on firmware without GX support, in; /get writes out). Publishes
# com.victronenergy.xregulator (found and picked regulators, for the gui-v2 plugin) and, per picked regulator,
# com.victronenergy.switch.xregulator_<uid6> for the switch pane and the plugin, plus, on firmware with /gxLimits,
# com.victronenergy.alternator.xregulator_<uid6> for systemcalc's DVCC, and in demo mode, when nothing on the GX could take
# it as a control input, com.victronenergy.battery.xregulator_<uid6>. While a power-flow tile shows, or a regulator takes
# them for its web app (/gxDevices), it also reads the GX's own battery, solar, charger, inverter and DC meter readings
# (GxReader). All logic lives in xreg_core.py; this file only maps it onto D-Bus. Picked regulators:
# com.victronenergy.settings /Settings/Xregulator/Units, the GX devices seen /Settings/Xregulator/GxDevices; each
# regulator's VRM instances and last address under /Settings/Devices/xregulator_<uid6>/ and .../xregulator_<uid6>_alt/.

import logging, math, os, re, sys
from functools import partial

sys.dont_write_bytecode = True   # no __pycache__ in the app folder: dbus-flashmq walks it to serve the plugin
import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(1, os.path.join(HERE, 'ext', 'velib_python'))
from vedbus import VeDbusService, VeDbusItemExport
from settingsdevice import SettingsDevice
from ve_utils import unwrap_dbus_value

import xreg_core

SCHEMA_FILE = os.path.join(HERE, 'settings_schema.json')
TICK_MS = 500
ALT_SCAN_S = 10   # how often the GX's other alternator services are looked at (NMEA 2000 twins, native control)
ALT_RESCAN_MS = (300, 1500, 5000)   # after one appears: vecan-dbus may name the service before its /Serial reads
GX_READ_MS = 1000   # the GX reader's timer; it reads at the cadence the regulators ask (Manager.wants_gx)
SYSTEM_SERVICE = 'com.victronenergy.system'
SYSTEM_PATHS = ('/Dc/Battery/Current', '/Dc/Battery/BatteryService', '/Dc/Pv/Current')
# per device class: what tells how much it puts into the battery bus (and a solar charger's or alternator's serial, and
# an NMEA 2000 alternator's unique number, which tells a regulator's twin before its serial reads).
# Signs as Victron publishes them (venus wiki dbus, dbus-systemcalc-py): + into the battery for every class here; an
# inverter's /Dc/0/Current is negative while it inverts, and a VE.Direct inverter has none (its AC output instead). A DC
# load meter's and a whole-DC-system meter's are + for what their loads draw (venus wiki dbus "dcload" and "dcsystem";
# systemcalc's /Dc/System is + consuming), so the snapshot turns them around. A DC source meter's, every monitor mode
# DC-DC included, is + charging (venus wiki dbus "alternator and dcsource"). A DC-DC charger's input side (/Dc/In/*) is
# its draw when it charges another battery from the house bank.
GX_PATHS = {'battery': ('/Dc/0/Current',),
            'solarcharger': ('/Serial', '/Dc/0/Current', '/Load/I'),
            'charger': ('/Dc/0/Current',),
            'fuelcell': ('/Dc/0/Current',),
            'alternator': ('/Serial', '/N2kUniqueNumber', '/Dc/0/Current', '/Dc/0/Power', '/Dc/0/Voltage'),
            'dcgenset': ('/Dc/0/Current', '/Dc/0/Power', '/Dc/0/Voltage'),
            'vebus': ('/Dc/0/Current',),
            'multi': ('/Dc/0/Current', '/Dc/0/Voltage', '/Yield/Power'),
            'inverter': ('/Dc/0/Current', '/Dc/0/Voltage', '/Yield/Power', '/Ac/Out/L1/V', '/Ac/Out/L1/I'),
            'dcdc': ('/Dc/0/Current', '/Dc/In/I', '/Dc/In/P', '/Dc/In/V'),
            'dcsource': ('/Dc/0/Current',),
            'dcload': ('/Dc/0/Current',),
            'dcsystem': ('/Dc/0/Current',)}
# who a device is: read when it appears and then every NAME_EVERY_S (owners rename devices rarely)
NAME_PATHS = ('/ProductName', '/CustomName', '/DeviceInstance', '/Settings/MonitorMode')
NAME_EVERY_S = 60

log = logging.getLogger('xregulator')


class Store:
    # Core's persistent values, kept in localsettings so they survive reboots and GX firmware updates
    def __init__(self, settings):
        self.settings = settings

    def get(self, name):
        return self.settings[name]

    def set(self, name, value):
        if self.settings[name] != value:
            self.settings[name] = value


class Publisher:
    # What a tick writes into: one VeDbusService context per tick, so a tick's changes go out as one ItemsChanged
    def __init__(self, ctx, on_write):
        self.ctx = ctx
        self.on_write = on_write

    def add(self, path, value, mode):
        if path in self.ctx:
            return
        if mode == 'core':
            self.ctx.add_path(path, value, writeable=True, onchangecallback=self.on_write)
        else:
            self.ctx.add_path(path, value, writeable=mode == 'free')

    def __setitem__(self, path, value):
        if path not in self.ctx:
            self.add(path, value, None)
        else:
            self.ctx[path] = value


def stamped_item(core):
    class StampedItem(VeDbusItemExport):
        # velib's SetValue returns before the change callback when the value is unchanged, so a steady value
        # systemcalc keeps writing would look stopped: stamp every call
        def SetValue(self, newvalue):
            core.alt_write(self.__dbus_object_path__, unwrap_dbus_value(newvalue))
            return VeDbusItemExport.SetValue(self, newvalue)
    return StampedItem


def bus_value(bus, service, path):
    # (found, value); a missing path or a service that does not answer is not found
    try:
        return True, unwrap_dbus_value(bus.get_object(service, path, introspect=False).GetValue(
            dbus_interface='com.victronenergy.BusItem', timeout=2))
    except dbus.exceptions.DBusException:
        return False, None


class GxReader:
    # The GX's own readings for the power flow and /gxDevices (xreg_core.gx_devices, flow_model). Asynchronous GetValue
    # calls while a tile shows or a regulator takes the readings, so a slow service never holds up the main loop.
    # Nothing is published from here.
    def __init__(self, bus, clock):
        self.bus, self.clock = bus, clock
        self.names = set()
        self.values = {}    # (service, path) -> (value, clock)
        self.proxies = {}   # (service, path) -> proxy; dropped on an error, so a restarted service is found again
        self.named = {}     # service -> clock its NAME_PATHS were last asked for
        self.since = None   # clock of the first poll since reading resumed: a snapshot before its replies is empty
        self.asked = {}     # service -> clock of the first poll that asked it since reading resumed (its serial wait)

    def set_names(self, names):
        self.names = {n for n in names if self._class(n) in GX_PATHS}
        for key in [k for k in self.proxies if k[0] != SYSTEM_SERVICE and k[0] not in self.names]:
            self.proxies.pop(key, None)
        for key in [k for k in self.values if k[0] != SYSTEM_SERVICE and k[0] not in self.names]:
            self.values.pop(key, None)
        for name in [n for n in self.named if n not in self.names]:
            del self.named[name]
        for name in [n for n in self.asked if n not in self.names]:
            del self.asked[name]

    def name_changed(self, name, present):
        if self._class(name) in GX_PATHS:
            self.set_names(self.names | {name} if present else self.names - {name})

    @staticmethod
    def _class(name):
        parts = name.split('.')
        return parts[2] if len(parts) > 3 and name.startswith('com.victronenergy.') else None

    def idle(self):
        self.since = None
        self.asked.clear()

    def settled(self):
        return self.since is not None and self.clock() - self.since >= 0.5

    def poll(self):
        now = self.clock()
        if self.since is None:
            self.since = now
        keys = [(SYSTEM_SERVICE, p) for p in SYSTEM_PATHS]
        for name in sorted(self.names):
            self.asked.setdefault(name, now)
            keys += [(name, p) for p in GX_PATHS[self._class(name)]]
            if now - self.named.get(name, -1e18) >= NAME_EVERY_S:
                self.named[name] = now
                keys += [(name, p) for p in NAME_PATHS]
        for key in keys:
            self._get(key)

    def _get(self, key):
        def ok(v):
            self.values[key] = (unwrap_dbus_value(v), self.clock())

        def fail(e):
            self.values.pop(key, None)
            self.proxies.pop(key, None)
        try:
            obj = self.proxies.get(key)
            if obj is None:
                obj = self.proxies[key] = self.bus.get_object(key[0], key[1], introspect=False)
            obj.GetValue(dbus_interface='com.victronenergy.BusItem', reply_handler=ok, error_handler=fail, timeout=2)
        except dbus.exceptions.DBusException as e:
            fail(e)

    def snapshot(self):
        now = self.clock()

        def num(svc, path, fresh=True):
            e = self.values.get((svc, path))
            v = e[0] if e and (not fresh or now - e[1] <= xreg_core.GX_FRESH_S) else None
            return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) else None

        def text(svc, path, fresh=True):
            e = self.values.get((svc, path))
            return str(e[0]).strip() if e and (not fresh or now - e[1] <= xreg_core.GX_FRESH_S) \
                and isinstance(e[0], str) else ''

        def age(svc, paths):
            # a reading worked out from several paths is as old as the oldest of them
            e = [self.values.get((svc, p)) for p in paths]
            oldest = max(now - x[1] for x in e) if all(e) else None
            return round(oldest, 1) if oldest is not None and oldest <= xreg_core.GX_FRESH_S else None
        devices = []
        for name in sorted(self.names):
            cls = self._class(name)
            product, custom = text(name, '/ProductName', False), text(name, '/CustomName', False)
            if not product and not custom:
                continue   # not read yet, or no device (gui-v2 lists only a service with a name; ours without one)
            i, pv, read = num(name, '/Dc/0/Current'), None, ['/Dc/0/Current']
            v = num(name, '/Dc/0/Voltage')
            if cls == 'solarcharger' and i is not None:
                load = num(name, '/Load/I')
                if load is not None:
                    i, read = i + load, read + ['/Load/I']
            if i is None and cls in ('alternator', 'dcgenset'):
                p = num(name, '/Dc/0/Power')
                i = p / v if p is not None and v and v > 1.0 else None
                read = ['/Dc/0/Power', '/Dc/0/Voltage']
            if cls in ('dcload', 'dcsystem') and i is not None:
                i = -i or 0.0
            if cls in ('multi', 'inverter'):
                y = num(name, '/Yield/Power')
                pv = y / v if y is not None and v and v > 1.0 else None   # its PV is in systemcalc's /Dc/Pv/Current
            if i is None and cls == 'inverter':
                # a VE.Direct inverter has no DC current: its AC output over the battery voltage, as systemcalc does
                av, ai = num(name, '/Ac/Out/L1/V'), num(name, '/Ac/Out/L1/I')
                i = -av * ai / v if av is not None and ai is not None and v and v > 1.0 else None
                read = ['/Ac/Out/L1/V', '/Ac/Out/L1/I', '/Dc/0/Voltage']
            iin, read_in = None, ['/Dc/In/I']
            if cls == 'dcdc':
                iin = num(name, '/Dc/In/I')
                if iin is None:
                    p, vin, read_in = num(name, '/Dc/In/P'), num(name, '/Dc/In/V'), ['/Dc/In/P', '/Dc/In/V']
                    iin = p / vin if p is not None and vin and vin > 1.0 else None
            inst = num(name, '/DeviceInstance', False)
            mode = num(name, '/Settings/MonitorMode', False)
            # an NMEA 2000 device's NAME unique number, from vecan-dbus's path or its service name (..._uc1684700)
            n2k = num(name, '/N2kUniqueNumber', False) if cls == 'alternator' else None
            uc = re.search(r'_uc(\d+)$', name) if cls == 'alternator' and n2k is None else None
            n2k = int(n2k) if n2k is not None else int(uc.group(1)) if uc else None
            devices.append({'service': name, 'cls': cls, 'key': '%s:%d' % (cls, inst) if inst is not None else name,
                            'serial': text(name, '/Serial', False), 'name': custom, 'product': product,
                            'n2k': n2k, 'since': round(now - self.asked.get(name, now), 1),
                            'mode': int(mode) if mode is not None else None, 'amps': i, 'pv': pv,
                            'age': age(name, read) if i is not None else None,
                            'in_amps': iin, 'in_age': age(name, read_in) if iin is not None else None})
        return {'batt': num(SYSTEM_SERVICE, '/Dc/Battery/Current'),
                'batt_service': text(SYSTEM_SERVICE, '/Dc/Battery/BatteryService'),
                'pv': num(SYSTEM_SERVICE, '/Dc/Pv/Current'), 'devices': devices, 'at': now}


def publish(svc, paths, on_write):
    for path, value, mode in paths:
        if mode == 'core':
            svc.add_path(path, value, writeable=True, onchangecallback=on_write)
        else:
            svc.add_path(path, value, writeable=mode == 'free')
    svc.register()


class Bridge:
    def __init__(self):
        self.bus = dbus.SystemBus()
        self.settings = SettingsDevice(self.bus, {'units': ['/Settings/Xregulator/Units', '', 0, 0],
                                                  'gx_devices': ['/Settings/Xregulator/GxDevices', '', 0, 0]},
                                       lambda *a: None, timeout=30)
        try:
            with open(SCHEMA_FILE) as f:
                schema = f.read()
        except OSError as e:
            log.warning('settings schema: %s', e)
            schema = ''
        self.units = {}   # uid -> (Core, its VeDbusService, its private bus connection)
        self.alts = {}    # uid -> (alternator service mode, its VeDbusService, its private bus connection)
        self.batts = {}   # uid -> (battery service mode, its VeDbusService, its private bus connection), demo only
        self.manager = xreg_core.Manager(Store(self.settings), schema, xreg_core.Discovery(), self._unit_store,
                                         self._forget_store, listener=self)
        self.svc = VeDbusService(xreg_core.MGR_SERVICE, self.bus, register=False)
        publish(self.svc, self.manager.static_paths(__file__), partial(self._on_write, self.manager))
        self.manager.start()
        self.gx = GxReader(self.bus, self.manager.clock)
        self.gx_ticks = 0
        self._scan_alternators()
        self.bus.add_signal_receiver(self._name_owner_changed, signal_name='NameOwnerChanged',
                                     dbus_interface='org.freedesktop.DBus', path='/org/freedesktop/DBus')
        GLib.timeout_add(TICK_MS, self._tick)
        GLib.timeout_add(ALT_SCAN_S * 1000, self._scan_alternators)
        GLib.timeout_add(GX_READ_MS, self._read_gx)

    @staticmethod
    def _paths(uid):
        group = '/Settings/Devices/' + xreg_core.unit_ident(uid)
        return (group + '/ClassAndVrmInstance', group + '/Host', group + '_alt/ClassAndVrmInstance',
                group + '_batt/ClassAndVrmInstance')

    def _unit_store(self, uid, instance):
        # localsettings moves a taken ClassAndVrmInstance to the next free one
        inst, host, alt, batt = self._paths(uid)
        return Store(SettingsDevice(self.bus, {'instance': [inst, 'switch:%d' % instance, 0, 0],
                                               'host': [host, '', 0, 0],
                                               'alt_instance': [alt, 'alternator:%d' % xreg_core.INSTANCE, 0, 0],
                                               'batt_instance': [batt, 'battery:%d' % xreg_core.INSTANCE, 0, 0]},
                                    lambda *a: None, timeout=30))

    def _forget_store(self, uid):
        try:
            self.bus.get_object('com.victronenergy.settings', '/', introspect=False).RemoveSettings(
                list(self._paths(uid)), dbus_interface='com.victronenergy.Settings')
        except dbus.exceptions.DBusException as e:
            log.warning('remove settings of %s: %s', xreg_core.label(uid), e)

    # Each regulator's service on its own connection: VeDbusService exports its items at the connection's root
    def unit_added(self, core):
        bus = dbus.SystemBus(private=True)
        svc = VeDbusService(xreg_core.unit_service(core.uid), bus, register=False)
        publish(svc, core.static_paths(__file__), partial(self._on_write, core))
        self.units[core.uid] = (core, svc, bus)

    def unit_removed(self, core):
        self._drop_alt(core.uid)
        self._drop_batt(core.uid)
        _, svc, bus = self.units.pop(core.uid)
        svc.__del__()
        bus.close()

    # The alternator service follows Core.alt_mode(): made, rebuilt when the mode changes, dropped when None
    def _sync_alts(self):
        for core, _, _ in list(self.units.values()):
            want = core.alt_mode()
            have = self.alts.get(core.uid)
            if have and have[0] != want:
                self._drop_alt(core.uid)
                have = None
            if want and not have:
                bus = dbus.SystemBus(private=True)
                svc = VeDbusService(xreg_core.alt_service(core.uid), bus, register=False)
                item = stamped_item(core)
                for path, value, mode in core.alt_static_paths(want, __file__):
                    if mode == 'stamp':
                        svc.add_path(path, value, writeable=True, itemtype=item)
                    else:
                        svc.add_path(path, value)
                svc.register()
                have = self.alts[core.uid] = (want, svc, bus)
                log.info('%s: alternator service %s', xreg_core.label(core.uid, core.last_name), want)
            if have:
                try:
                    with have[1] as ctx:
                        core.alt_tick(Publisher(ctx, None), want)
                except Exception:
                    log.exception('alternator tick')

    def _drop_alt(self, uid):
        have = self.alts.pop(uid, None)
        if have:
            have[1].__del__()
            have[2].close()

    # The demo battery service follows Core.batt_mode(): made when it turns 'shown', dropped when it turns None
    def _sync_batts(self):
        for core, _, _ in list(self.units.values()):
            want = core.batt_mode()
            have = self.batts.get(core.uid)
            if have and have[0] != want:
                self._drop_batt(core.uid)
                have = None
            if want and not have:
                bus = dbus.SystemBus(private=True)
                svc = VeDbusService(xreg_core.batt_service(core.uid), bus, register=False)
                for path, value, _mode in core.batt_static_paths(__file__):
                    svc.add_path(path, value)
                svc.register()
                have = self.batts[core.uid] = (want, svc, bus)
                log.info('%s: demo battery service shown', xreg_core.label(core.uid, core.last_name))
            if have:
                try:
                    with have[1] as ctx:
                        core.batt_tick(Publisher(ctx, None))
                except Exception:
                    log.exception('battery tick')

    def _drop_batt(self, uid):
        have = self.batts.pop(uid, None)
        if have:
            have[1].__del__()
            have[2].close()
            log.info('%s: demo battery service removed', xreg_core.label(uid))

    def _ours(self, name):
        return name in [xreg_core.batt_service(u) for u in self.units] + [xreg_core.alt_service(u) for u in self.units]

    def _battery_blocker(self, name):
        parts = name.split('.')
        return len(parts) > 3 and name.startswith('com.victronenergy.') and parts[2] in xreg_core.BATT_BLOCKERS \
            and not self._ours(name)

    def _scan_battery_gate(self, names):
        # A demo battery only on a GX with no CAN port, no other battery, nothing that takes a battery reading, DVCC off
        # and Modbus TCP off (xreg_core.BATT_SERVICE); a setting that cannot be read counts as on
        blocked = any(self._battery_blocker(n) for n in names)
        for path in ('/Settings/Services/Bol', '/Settings/Services/Modbus'):
            found, value = bus_value(self.bus, 'com.victronenergy.settings', path)
            blocked = blocked or not found or value != 0
        self.manager.set_battery_gate(not blocked)

    def _other_alternator(self, name):
        return name.startswith(xreg_core.ALT_SERVICE) and name not in [xreg_core.alt_service(u) for u in self.units]

    def _name_owner_changed(self, name, old, new):
        # The 10 s scan alone let a returning NMEA 2000 twin count beside our measurements until it ran (seen: 1-3 s)
        name = str(name)
        self.gx.name_changed(name, bool(new))
        if new and self._battery_blocker(name):
            # a CAN port, a battery or a charger arrived: the demo battery goes before systemcalc can use it
            self.manager.set_battery_gate(False)
            self._sync_batts()
        if not self._other_alternator(name):
            return
        if new:
            self.manager.alternator_appeared()
            self._sync_alts()   # the measurements go invalid now, before systemcalc has scanned the new service
            for i, ms in enumerate(ALT_RESCAN_MS):
                GLib.timeout_add(ms, partial(self._rescan, i == len(ALT_RESCAN_MS) - 1))
        else:
            self._scan_alternators()

    def _rescan(self, final):
        self._scan_alternators(final)
        return False

    def _scan_alternators(self, final=False):
        found, unread = [], False
        try:
            names = [str(n) for n in self.bus.list_names()]
        except dbus.exceptions.DBusException as e:
            log.warning('list names: %s', e)
            return True
        for name in names:
            if not self._other_alternator(name):
                continue
            ok, serial = bus_value(self.bus, name, '/Serial')
            if ok and serial:
                found.append({'service': name, 'serial': str(serial),
                              'link': bus_value(self.bus, name, '/Link/ChargeCurrent')[0]})
            else:
                unread = True
        self.manager.set_alternators(found, settled=final or not unread)
        self._sync_alts()
        self.gx.set_names(names)
        self._scan_battery_gate(names)
        self._sync_batts()
        return True

    def _read_gx(self):
        every = self.manager.wants_gx()
        self.gx_ticks += 1
        if not every:
            self.gx.idle()
        elif self.gx_ticks * GX_READ_MS >= every * 1000:
            self.gx_ticks = 0
            self.gx.poll()
            if self.gx.settled():   # the replies are in from the poll before this one
                self.manager.set_gx_side(self.gx.snapshot())
        return True

    @staticmethod
    def _on_write(target, path, value):
        try:
            return target.on_write(path, value)
        except Exception:
            log.exception('write %s', path)
            return False

    def _tick(self):
        self._tick_one(self.manager, self.svc)   # first: it may add or remove regulators
        for core, svc, _ in list(self.units.values()):
            self._tick_one(core, svc)
        self._sync_alts()
        self._sync_batts()
        return True

    def _tick_one(self, target, svc):
        try:
            with svc as ctx:
                target.tick(Publisher(ctx, partial(self._on_write, target)))
        except Exception:
            log.exception('tick')


def main():
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    DBusGMainLoop(set_as_default=True)
    Bridge()
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
