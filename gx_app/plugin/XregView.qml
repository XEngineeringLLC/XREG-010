import QtQuick
import Victron.VenusOS

// The settings of one chart that opens a larger view: autoscale, the axis lock and the ranges it froze, the right axis's
// own limits and the lines tapped off. The small chart and its larger view read the same object, so the small one
// shows whatever was set in the large one. Kept per regulator (its /Serial) in localsettings, /Settings/Xregulator/Views
// (the bridge creates it), one JSON object for every view, so they survive a reboot; a missing entry is autoscale on.
// A Short Term chart's left-axis manual limits are the regulator's own (ylimPath, e.g. the web app's Ymin1/Ymax1 as the
// bridge's /Plot/YLim/Current), so the GX and the web app share them; the rest are kept here, as the web app keeps its
// Field % and Long Term limits in the browser.
QtObject {
	id: v

	property string svc
	property string name
	property string ylimPath
	// a temperature left axis: the bridge's limits (the regulator keeps whole degrees F) and those kept here are in
	// Celsius, shown in the GX's temperature unit, so a change of unit moves the number and not the limit
	property bool tempLimits
	property int decimals: 0           // the left axis's limits are kept to this many decimals
	// the right axis with autoscale off and no limit set; null: where it was shown
	property var rightDefault: [0, 100]
	readonly property bool shared: ylimPath.length > 0

	readonly property VeQuickItem serial: VeQuickItem { uid: v.svc + "/Serial" }
	readonly property VeQuickItem store: VeQuickItem {
		uid: BackendConnection.serviceUidFromName("com.victronenergy.settings", 0) + "/Settings/Xregulator/Views"
		onValueChanged: v.load()
	}
	readonly property VeQuickItem ylimItem: VeQuickItem { uid: v.ylimPath ? v.svc + v.ylimPath : "" }
	// a limit just written shows until the bridge publishes it (its hold, then the regulator's echo)
	readonly property XregSync ylimSync: XregSync { value: v.ylimItem.valid ? String(v.ylimItem.value) : "" }

	readonly property string key: serial.valid && serial.value ? String(serial.value) + "/" + name : ""
	onKeyChanged: load()
	Component.onCompleted: load()

	property var all: ({})
	property var mine: ({})
	function load() {
		let a = {}
		try {
			// the value itself, not store.valid, which can still read false while the first value arrives
			const raw = store.value
			const p = typeof raw === "string" && raw.length ? JSON.parse(raw) : {}
			if (p && typeof p === "object" && !Array.isArray(p))
				a = p
		} catch (e) {
		}
		all = a
		const m = key ? a[key] : null
		mine = m && typeof m === "object" ? m : {}
	}
	// a patch's null entries are removed
	function set(patch) {
		if (!key)
			return
		const m = Object.assign({}, mine)
		for (const k in patch) {
			if (patch[k] === null)
				delete m[k]
			else
				m[k] = patch[k]
		}
		const a = Object.assign({}, all)
		a[key] = m
		all = a
		mine = m
		store.setValue(JSON.stringify(a))
	}

	function range(r) {
		return Array.isArray(r) && r.length === 2 && isFinite(r[0]) && isFinite(r[1]) && r[1] > r[0] ? [Number(r[0]), Number(r[1])] : null
	}
	readonly property bool auto: mine.auto !== false
	readonly property bool lock: auto && !!mine.lock && !!range(mine.lockL)
	readonly property var hidden: mine.hidden && typeof mine.hidden === "object" ? mine.hidden : ({})
	function toShown(c) {
		return tempLimits ? Units.convert(c, VenusOS.Units_Temperature_Celsius, Global.systemSettings.temperatureUnit) : c
	}
	function leftShown(r) {
		const g = range(r)
		return g && tempLimits ? range([toShown(g[0]), toShown(g[1])]) : g
	}
	function leftKept(r) {
		if (!r || !tempLimits)
			return r
		const u = Global.systemSettings.temperatureUnit
		return r.map(x => Units.convert(x, u, VenusOS.Units_Temperature_Celsius))
	}
	// the regulator's limits, "lo,hi"; null while unknown
	readonly property var ylim: {
		const s = ylimSync.shown
		if (!s)
			return null
		const p = String(s).split(",")
		return p.length === 2 ? range([toShown(+p[0]), toShown(+p[1])]) : null
	}
	readonly property bool manual: !auto
	// the axes the charts draw: null fits the data
	readonly property var leftFixed: auto ? (lock ? leftShown(mine.lockL) : null) : shared ? ylim : leftShown(mine.left)
	readonly property var rightFixed: auto ? (lock ? range(mine.lockR) : null) : (range(mine.right) || rightDefault)

	function setHidden(k, hide) {
		const h = Object.assign({}, hidden)
		h[k] = hide
		set({ hidden: h })
	}
	// Autoscale off: the limits set before, else the axes stay where they are shown (l, r)
	function setAuto(on, l, r) {
		if (on) {
			set({ auto: true, lock: null, lockL: null, lockR: null })
			return
		}
		let p = { auto: false, lock: null, lockL: null, lockR: null }
		if (!shared && !range(mine.left) && range(l))
			p.left = leftKept([round(l[0], false), round(l[1], true)])
		if (!rightDefault && !range(mine.right) && range(r))
			p.right = range(r)
		set(p)
	}
	// to the kept decimals, outward (up: the maximum); within 2% of a step counts as on it, so a temperature limit that
	// comes back through Celsius (130 F as 54.44 C, 129.99 F) keeps its value instead of moving a step out at each edit
	function round(x, up) {
		const f = Math.pow(10, decimals)
		return Number(((up ? Math.ceil(x * f - 0.02) : Math.floor(x * f + 0.02)) / f).toFixed(decimals))
	}
	// freeze the ranges shown now ([lo, hi] each; r null: a chart with no right axis), or let autoscale move them again
	function setLock(on, l, r) {
		if (on && range(l))
			set({ lock: true, lockL: leftKept(range(l)), lockR: range(r) })
		else
			set({ lock: null, lockL: null, lockR: null })
	}
	// An axis end set by hand: autoscale off, the other three ends held where they are shown (l, r: the axes shown).
	// The left axis is kept to `decimals`; a shared one is written to the regulator.
	function setEnd(right, isMax, value, l, r) {
		let nl = range(l), nr = range(r)
		if (!nl || right && !nr)
			return false
		if (right)
			nr = isMax ? [nr[0], value] : [value, nr[1]]
		else
			nl = isMax ? [nl[0], value] : [value, nl[1]]
		nl = [round(nl[0], false), round(nl[1], true)]
		if (!range(nl) || right && !range(nr))
			return false
		let p = { auto: false, lock: null, lockL: null, lockR: null }
		if (nr)   // no right axis shown (Sailing has no RPM): its limit stays
			p.right = nr
		if (!shared) {
			p.left = leftKept(nl)
			set(p)
			return true
		}
		const sl = ylim
		if (!sl || Math.abs(sl[0] - nl[0]) > 1e-6 || Math.abs(sl[1] - nl[1]) > 1e-6) {
			const w = wire(nl[0]) + "," + wire(nl[1])
			ylimSync.expect(w)
			ylimItem.setValue(w)
		}
		set(p)
		return true
	}
	// A limit as the bridge publishes it: at most two decimals, no trailing zeros; a temperature in Celsius, of the whole
	// degree F the regulator will keep, so the bridge's echo is this same text
	function wire(x) {
		if (tempLimits) {
			const u = Global.systemSettings.temperatureUnit
			const f = Math.round(Units.convert(x, u, VenusOS.Units_Temperature_Fahrenheit))
			x = Units.convert(f, VenusOS.Units_Temperature_Fahrenheit, VenusOS.Units_Temperature_Celsius)
		}
		return String(Number(x.toFixed(2)))
	}
}
