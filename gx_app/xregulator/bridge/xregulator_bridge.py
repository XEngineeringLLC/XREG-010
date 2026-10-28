#!/usr/bin/env python3
# XREG-010 bridge for Venus OS. Finds the regulators on the GX's networks by uid and talks to each one the owner picked
# over WiFi (its /gxevents stream, or /events on firmware without GX support, in; /get writes out). Publishes
# com.victronenergy.xregulator (found and picked regulators, for the gui-v2 plugin) and, per picked regulator,
# com.victronenergy.switch.xregulator_<uid6> for the switch pane and the plugin, plus, on firmware with /gxLimits,
# com.victronenergy.alternator.xregulator_<uid6> for systemcalc's DVCC. All logic lives in xreg_core.py; this file only
# maps it onto D-Bus. Picked regulators: com.victronenergy.settings /Settings/Xregulator/Units; each one's VRM
# instances and last address under /Settings/Devices/xregulator_<uid6>/ and .../xregulator_<uid6>_alt/.

import logging, os, sys
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
        self.settings = SettingsDevice(self.bus, {'units': ['/Settings/Xregulator/Units', '', 0, 0]},
                                       lambda *a: None, timeout=30)
        try:
            with open(SCHEMA_FILE) as f:
                schema = f.read()
        except OSError as e:
            log.warning('settings schema: %s', e)
            schema = ''
        self.units = {}   # uid -> (Core, its VeDbusService, its private bus connection)
        self.alts = {}    # uid -> (alternator service mode, its VeDbusService, its private bus connection)
        self.manager = xreg_core.Manager(Store(self.settings), schema, xreg_core.Discovery(), self._unit_store,
                                         self._forget_store, listener=self)
        self.svc = VeDbusService(xreg_core.MGR_SERVICE, self.bus, register=False)
        publish(self.svc, self.manager.static_paths(__file__), partial(self._on_write, self.manager))
        self.manager.start()
        self._scan_alternators()
        self.bus.add_signal_receiver(self._name_owner_changed, signal_name='NameOwnerChanged',
                                     dbus_interface='org.freedesktop.DBus', path='/org/freedesktop/DBus')
        GLib.timeout_add(TICK_MS, self._tick)
        GLib.timeout_add(ALT_SCAN_S * 1000, self._scan_alternators)

    @staticmethod
    def _paths(uid):
        group = '/Settings/Devices/' + xreg_core.unit_ident(uid)
        return group + '/ClassAndVrmInstance', group + '/Host', group + '_alt/ClassAndVrmInstance'

    def _unit_store(self, uid, instance):
        # localsettings moves a taken ClassAndVrmInstance to the next free one
        inst, host, alt = self._paths(uid)
        return Store(SettingsDevice(self.bus, {'instance': [inst, 'switch:%d' % instance, 0, 0],
                                               'host': [host, '', 0, 0],
                                               'alt_instance': [alt, 'alternator:%d' % xreg_core.INSTANCE, 0, 0]},
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

    def _other_alternator(self, name):
        return name.startswith(xreg_core.ALT_SERVICE) and name not in [xreg_core.alt_service(u) for u in self.units]

    def _name_owner_changed(self, name, old, new):
        # The 10 s scan alone let a returning NMEA 2000 twin count beside our measurements until it ran (seen: 1-3 s)
        name = str(name)
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
