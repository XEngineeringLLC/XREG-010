import QtQuick
import Victron.VenusOS

// Plots > Short Term: the web app's four live charts (current with the charge-stage strip, battery voltage, RPM,
// alternator temperature), each with field % on the right axis, over a 5 s to 5 min window. Data: the bridge's
// /Fast/Short, one sample per CSV1 frame of the last 5 min, appended as frames arrive while this tab asks for them
// (xreg_core.py ShortRecorder.message): the whole history when the tab first sees it or finds a gap (it writes
// /Fast/ShortFull), then the newest samples. The charts decimate and draw it incrementally (XregChart's streamed lines),
// scrolling every display frame, or with Slower plotting (the GX app page, and a row below the charts) stepping once a
// second as the bridge then sends.
Item {
	id: tab

	property string svc
	property real m: 14
	property real toolX
	property real toolY
	property real bodyY
	property int windowS: 30
	signal windowPicked(int seconds)

	readonly property var windows: [5, 10, 30, 60, 120, 300]   // the last is the history kept (the bridge's SHORT_S)
	readonly property bool oneColumn: width < 760
	property QtObject timing           // XregHome's bench timing log
	property int opens                 // XregPlots: shown again after another tab

	// /Fast/Short's series, in its order
	readonly property var names: ["MeasuredAmps", "Bcur", "iiout", "dutyCycle", "pidAltPV", "BatteryV", "IBV", "RPM",
		"AlternatorTemperatureC", "chargeStage"]
	// The samples held, oldest first: t in ms, one array per series, sample i at t[i - off], held from off + h; stage runs
	// { a, b (null: still on), c }. A new history replaces it with reset one higher; ver counts updates (XregChart).
	property var store: blank("", 0)
	property int ver
	property bool behind               // updated while hidden: redraw on return
	property var job: null             // a whole history being read in slices, and the messages that came meanwhile
	property real askedAt: -1e9
	readonly property int keepMs: windows[windows.length - 1] * 1000

	VeQuickItem { id: want; uid: tab.svc + "/Ui/Want/Short" }
	VeQuickItem { id: slowItem; uid: BackendConnection.serviceUidFromName("com.victronenergy.settings", 0) + "/Settings/Xregulator/SlowPlots" }
	VeQuickItem { id: slowAutoItem; uid: BackendConnection.serviceUidFromName("com.victronenergy.settings", 0) + "/Settings/Xregulator/SlowPlotsAuto" }
	readonly property bool slow: slowItem.valid && slowItem.value === 1

	// A tap on a chart opens its larger view (XregPop on the home page); its settings are in its view, its manual left
	// limits the regulator's Ymin/Ymax that the web app's plot of the same name uses (Current 1, Voltage 2, RPM 3,
	// Temperature 4, kept in whole degrees F). The charts under an open larger view stop scrolling between updates (they
	// step), so the Ekrano draws one scrolling chart, not five.
	readonly property XregView currentView: XregView { svc: tab.svc; name: "shortCurrent"; ylimPath: "/Plot/YLim/Current" }
	readonly property XregView voltsView: XregView { svc: tab.svc; name: "shortVolts"; ylimPath: "/Plot/YLim/Volts"; decimals: 1 }
	readonly property XregView rpmView: XregView { svc: tab.svc; name: "shortRpm"; ylimPath: "/Plot/YLim/Rpm" }
	readonly property XregView tempView: XregView { svc: tab.svc; name: "shortTemp"; ylimPath: "/Plot/YLim/Temp"; tempLimits: true }
	readonly property var views: ({ current: currentView, volts: voltsView, rpm: rpmView, temp: tempView })
	property var popSpec: specs[0]
	function openChart(spec) {
		popSpec = spec
		openPop(chartPop)
	}
	property Item home: null
	readonly property bool popShown: !!home && home.popShown
	Component.onCompleted: {
		for (let p = parent; p; p = p.parent) {
			if (p.openPop !== undefined) {
				home = p
				break
			}
		}
		take(feed.value)
	}
	function openPop(c) {
		if (home)
			home.openPop(c)
	}
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: tab.visible && !ScreenBlanker.blanked
		onTriggered: want.setValue(Math.floor(Date.now() / 1000))
	}

	VeQuickItem {
		id: feed
		uid: tab.svc + "/Fast/Short"
		onValueChanged: tab.take(feed.value)
	}
	VeQuickItem { id: ask; uid: tab.svc + "/Fast/ShortFull" }

	onVisibleChanged: if (visible && behind) bump()
	// Back from another tab: as the tab was when first opened
	function reopen() {
		flick.cancelFlick()
		flick.contentY = 0
		opens++
		bump()
	}

	function toUserTemp(c) {
		return isFinite(c) ? Units.convert(c, VenusOS.Units_Temperature_Celsius, Global.systemSettings.temperatureUnit) : NaN
	}
	// The charts' lines: what each draws (sk: its series); Field % on the right of all four
	readonly property var d: {
		const pct = { key: "duty", sk: "dutyCycle", label: "Field %", unit: "%", dec: 0, color: Theme.color_font_secondary,
					  right: true, dash: true }
		return {
			current: [
				{ key: "bcur", sk: "Bcur", label: "Battery Current", unit: "A", dec: 1, color: Theme.color_green },
				{ key: "alt", sk: "MeasuredAmps", label: "Alternator Current", unit: "A", dec: 1, color: Theme.color_blue },
				{ key: "field", sk: "iiout", label: "Field Current", unit: "A", dec: 2, color: Theme.color_blackWater },
				pct,
				{ key: "pv", sk: "pidAltPV", label: "Alt Current filtered", unit: "A", dec: 1, color: Theme.color_liveWell,
				  dash: true, w: 1.5, off: true }
			],
			volts: [
				{ key: "ads", sk: "BatteryV", label: "ADS Battery", unit: "V", dec: 2, color: Theme.color_orange },
				{ key: "ina", sk: "IBV", label: "INA Battery", unit: "V", dec: 2, color: Theme.color_green },
				pct
			],
			rpm: [{ key: "rpm", sk: "RPM", label: "RPM", unit: "", dec: 0, color: Theme.color_lpg }, pct],
			temp: [
				{ key: "temp", sk: "AlternatorTemperatureC", label: "Alt. Temp", unit: Global.systemSettings.temperatureUnitSuffix,
				  dec: 1, color: Theme.color_red, conv: toUserTemp },
				pct
			]
		}
	}

	function blank(epoch, reset) {
		let v = {}
		for (const name of names)
			v[name] = []
		return { t: [], v: v, off: 0, h: 0, reset: reset, stages: [], epoch: epoch, n: 0 }
	}
	// the charts draw what is new (hidden: on return)
	function bump() {
		behind = !visible
		if (visible)
			ver++
	}
	// One /Fast/Short message: "s1;epoch;newest number;count;full;dt,..;<a list per series>"
	function take(msg) {
		if (!msg)
			return
		if (job) {
			job.queue.push(String(msg))
			return
		}
		const t0 = Date.now()
		const r = apply(String(msg))
		if (r === "gap" && Date.now() - askedAt > 2000) {
			askedAt = Date.now()
			ask.setValue(1)
		}
		if (r === "inc" || r === "tail" || r === "new") {
			bump()
			if (timing && visible)
				timing.add("short", Date.now() - t0)
		}
	}
	// the samples after the ones held, a new history, or a gap (the same rules as test_fastlane.py's short_client)
	function apply(msg) {
		const p = msg.split(";")
		if (p.length !== 6 + names.length || p[0] !== "s1")
			return "bad"
		const epoch = p[1], n = Number(p[2]), k = Number(p[3]), full = p[4] === "1"
		const first = n - k + 1, same = epoch === store.epoch
		if (same && n <= store.n)
			return "old"
		if (same && first <= store.n) {
			append(p, k, store.n - first + 1, n)
			return "tail"
		}
		if (full) {
			job = { p: p, epoch: epoch, n: n, k: k, cols: [], queue: [], ms: 0 }
			loadSlice()
			return "load"
		}
		if (same && first === store.n + 1) {
			append(p, k, 0, n)
			return "inc"
		}
		if (!same && first === 1) {
			store = blank(epoch, store.reset + 1)
			append(p, k, 0, n)
			return "new"
		}
		return "gap"
	}
	// the last `count` of the `total` comma-separated items of str
	function items(str, count, total) {
		if (count >= total)
			return str.split(",")
		let i = str.length
		for (let c = 0; c < count; ++c)
			i = str.lastIndexOf(",", i - 1)
		return str.slice(i + 1).split(",")
	}
	function num(x) { return x === "" ? NaN : +x }
	function append(p, k, from, n) {
		const s = store, c = k - from
		if (c > 0) {
			const dts = items(p[5], c, k)
			const cols = []
			for (let j = 0; j < names.length; ++j)
				cols.push(items(p[6 + j], c, k))
			for (let i = 0; i < c; ++i) {
				const t = (s.t.length ? s.t[s.t.length - 1] : 0) + Number(dts[i])
				s.t.push(t)
				for (let j = 0; j < names.length - 1; ++j)
					s.v[names[j]].push(num(cols[j][i]))
				const st = num(cols[names.length - 1][i])
				s.v.chargeStage.push(st)
				stage(s, t, st)
			}
			trim(s)
		}
		s.n = n
	}
	function stage(s, t, c) {
		const code = isFinite(c) ? Math.round(c) : 0, runs = s.stages, last = runs.length ? runs[runs.length - 1] : null
		if (last && last.c === code)
			return
		if (last)
			last.b = t
		runs.push({ a: t, b: null, c: code })
	}
	// keep the longest window, and the sample before it that the lines enter from (the bridge keeps the same)
	function trim(s) {
		const n = s.t.length, lim = s.t[n - 1] - keepMs
		while (n - s.h >= 2 && s.t[s.h + 1] <= lim)
			s.h++
		while (s.stages.length > 1 && s.stages[0].b !== null && s.stages[0].b <= s.t[s.h])
			s.stages.shift()
		if (s.h > 2048 && s.h > n / 2) {
			s.t.splice(0, s.h)
			for (const name of names)
				s.v[name].splice(0, s.h)
			s.off += s.h
			s.h = 0
		}
	}
	// A whole history, read 4 ms at a time (once per animation tick, a Timer) so a tap is never held long: dt first, then
	// each series. Then the messages that came meanwhile.
	Timer {
		id: loadLater
		interval: 0
		onTriggered: tab.loadSlice()
	}
	function loadSlice() {
		const jb = job
		if (!jb)
			return
		const t0 = Date.now()
		do {
			const j = jb.cols.length
			const a = jb.k ? jb.p[5 + j].split(",") : []
			const out = new Array(a.length)
			for (let i = 0; i < a.length; ++i)
				out[i] = j === 0 ? Number(a[i]) : num(a[i])
			jb.cols.push(out)
		} while (jb.cols.length < names.length + 1 && Date.now() - t0 < 4)
		jb.ms += Date.now() - t0
		if (jb.cols.length < names.length + 1) {
			loadLater.restart()
			return
		}
		const s = blank(jb.epoch, store.reset + 1)
		let t = 0
		for (let i = 0; i < jb.k; ++i) {
			t += i ? jb.cols[0][i] : 0
			s.t.push(t)
			stage(s, t, jb.cols[names.length][i])
		}
		for (let j = 0; j < names.length; ++j)
			s.v[names[j]] = jb.cols[j + 1]
		s.n = jb.n
		trim(s)
		job = null
		const q = jb.queue
		store = s
		if (timing)
			timing.add("shortLoad", jb.ms)
		bump()
		for (const m of q)
			take(m)
	}

	readonly property var ticks: {
		const w = windowS
		const st = w <= 5 ? 1 : w <= 10 ? 2 : w <= 30 ? 5 : w <= 60 ? 10 : w <= 120 ? 20 : 60
		let t = []
		for (let s = w - (w % st === 0 ? st : w % st); s >= 0; s -= st)
			t.push({ x: -s, t: s === 0 ? "now" : s >= 60 && s % 60 === 0 ? (s / 60) + " min" : s + " s" })
		return t
	}

	SegmentedButtonRow {
		x: tab.toolX
		y: tab.toolY
		width: Math.min(tab.width - tab.m - x, 420)
		height: 40
		fontPixelSize: Theme.font_size_body1
		model: [{ value: "5S" }, { value: "10S" }, { value: "30S" }, { value: "60S" }, { value: "2M" }, { value: "5M" }]
		currentIndex: tab.windows.indexOf(tab.windowS)
		onButtonClicked: function(buttonIndex) { tab.windowPicked(tab.windows[buttonIndex]) }
	}

	Flickable {
		id: flick
		x: tab.m
		y: tab.bodyY
		width: tab.width - 2 * tab.m
		height: tab.height - tab.bodyY - tab.m
		contentHeight: body.height
		clip: true
		interactive: contentHeight > height

		Column {
			id: body
			width: parent.width
			spacing: grid.gap

			Grid {
				id: grid
				readonly property real gap: 10
				readonly property real tileW: tab.oneColumn ? parent.width : (parent.width - gap) / 2
				// two rows fill the page; on a 5-inch screen under the home page's header they would be 145 px, too low for the
				// current chart's three legend lines, so the page scrolls instead
				readonly property real tileH: tab.oneColumn ? 260 : Math.max(200, (tab.height - tab.bodyY - tab.m - gap) / 2)
				columns: tab.oneColumn ? 1 : 2
				spacing: gap

				Repeater {
					model: tab.specs
					delegate: Rectangle {
						required property var modelData
						required property int index
						width: grid.tileW
						height: grid.tileH
						radius: Theme.geometry_overviewPage_widget_radius
						color: Theme.color_background_secondary
						ShortChart {
							anchors { fill: parent; margins: 10 }
							page: tab
							spec: modelData
							streamSalt: index * 13
							tapOpens: true
							resets: tab.opens
							onOpened: tab.openChart(modelData)
						}
					}
				}
			}

			// the GX app page's setting, here too where it takes effect; set by hand it turns Automatic plot rate off (bridge)
			ListSwitch {
				width: parent.width
				text: "Slower plotting"
				caption: "Updates the Dashboard numbers and the Short Term plots once a second. Uses less of the GX's processor, "
						 + "for older GX models."
						 + (slowAutoItem.valid && slowAutoItem.value === 1
							? " Automatic plot rate sets this now; changing it here turns that off." : "")
				dataItem.uid: slowItem.uid
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: dataItem.valid
			}
		}
	}

	// axis: the left axis as the limit box names it
	readonly property var specs: [
		{ k: "current", title: "Current History", axis: "Current", unit: "A", zero: true, minSpan: 10 },
		{ k: "volts", title: "Battery Voltage History", axis: "Voltage", unit: "V", zero: false, minSpan: "V" },
		{ k: "rpm", title: "RPM History", axis: "RPM", unit: "", zero: true, minSpan: 3000 },
		{ k: "temp", title: "Alternator Temperature History", axis: "Temperature",
		  unit: Global.systemSettings.temperatureUnitSuffix, zero: false, minSpan: 10 }
	]

	Component {
		id: chartPop
		XregChartPop {
			view: tab.views[tab.popSpec.k]
			chart: bigChart
			title: tab.popSpec.title
			leftName: tab.popSpec.axis
			leftUnit: tab.popSpec.unit
			ShortChart {
				id: bigChart
				anchors.fill: parent
				page: tab
				spec: tab.popSpec
				big: true
				editAxes: true
			}
		}
	}

	// One Short Term chart, small or (big) in a larger view: its axes from its view, autoscale padded 10% (the left axis
	// at least spec.minSpan wide, Field % at least 20 from 0), lines 3 px inside the plot's edges, a fixed axis's limits
	// labelled.
	component ShortChart: XregChart {
		required property Item page
		required property var spec
		property bool big
		title: spec.title
		lines: page.d[spec.k] || []
		stream: page.store
		streamVer: page.ver
		streamStages: spec.k === "current"
		smooth: !page.slow && (big || !page.popShown)
		xLo: -page.windowS
		xHi: 0
		xTicks: page.ticks
		leftFromZero: spec.zero
		leftUnit: spec.unit
		rightUnit: "%"
		emptyText: "Waiting for data"
		view: page.views[spec.k]
		leftFixed: view.leftFixed
		rightFixed: view.rightFixed
		rightFromZero: true
		pad: 0.1
		leftMinSpan: spec.minSpan
		rightMinSpan: 20
		inset: 3
		fixedEnds: true
	}
}
