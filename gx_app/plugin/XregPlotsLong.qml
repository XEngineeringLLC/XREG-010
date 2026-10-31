import QtQuick
import Victron.VenusOS

// Plots > Long Term: the web app's seven long-term charts (same groups, series and min/max bands) over 1 h to 30 d,
// from the regulator's on-device ring as the bridge bins it (/Plot/Long/*, at most 300 points, fetched while this
// tab asks for it). Where the bridge also sends an engine-on average (<name>OnAvg) it is the solid line and the
// all-hours average is the faint dashed one, as in the app; otherwise the all-hours average is the line. The
// temperature chart adds Battery (v5 records; empty on an older ring) and starts with Thermistor and Board hidden
// (Mark, 2026-10-28); a tap on their legend entry shows them.
Item {
	id: tab

	property string svc
	property real m: 14
	property real toolX
	property real toolY
	property real bodyY

	readonly property var ranges: ["1h", "6h", "24h", "7d", "30d"]
	readonly property var spans: [3600, 21600, 86400, 604800, 2592000]
	readonly property string range: rangeItem.valid && ranges.indexOf(String(rangeItem.value)) >= 0 ? String(rangeItem.value) : "24h"
	readonly property bool oneColumn: width < 760
	readonly property var envelope: ["battVolt", "battCurr", "altCurr", "victronCurr", "rpm", "duty", "altTemp", "tempTherm",
		"sog", "tws", "vmg", "aws", "awa", "twa", "heel", "pitch", "soc", "battTemp"]
	// the app keeps Victron/Solar on the all-hours average: it charges with the engine off
	readonly property var engineOn: ["battVolt", "battCurr", "altCurr", "duty"]
	readonly property var fields: fieldsItem.valid && String(fieldsItem.value).length ? String(fieldsItem.value).split(",") : []
	property var d: ({})               // the last complete redraw (snap)
	property bool dirty: true
	property real cursor: NaN
	property real openedAt: Date.now()
	property real now: Date.now()
	property QtObject timing           // XregHome's bench timing log
	// A redraw gives one chart its data per event-loop turn (step), the charts on screen first, so a tap waits for one
	// chart's work at most; a chart whose data has not changed keeps what it shows.
	property var job: null
	property bool stepPending
	readonly property bool busy: job !== null
	property var cache: ({})           // published name -> { raw, n, v, unit, t }: each string parsed once, until it changes

	VeQuickItem { id: want; uid: tab.svc + "/Ui/Want/Long" }
	VeQuickItem { id: rangeItem; uid: tab.svc + "/Ui/LongRange" }
	VeQuickItem { id: fieldsItem; uid: tab.svc + "/Plot/Long/Fields"; onValueChanged: tab.changed() }
	VeQuickItem { id: startItem; uid: tab.svc + "/Plot/Long/StartEpoch"; onValueChanged: tab.changed() }
	VeQuickItem { id: stepItem; uid: tab.svc + "/Plot/Long/StepS"; onValueChanged: tab.changed() }
	VeQuickItem { id: echoItem; uid: tab.svc + "/Plot/Long/Range"; onValueChanged: tab.changed() }
	VeQuickItem { id: stageItem; uid: tab.svc + "/Plot/Long/chargeStage"; onValueChanged: tab.changed() }
	// each field's newest recorded value in the whole ring: the legend's value when its window has none
	VeQuickItem { id: lastItem; uid: tab.svc + "/Plot/Long/Last"; onValueChanged: tab.changed() }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: tab.visible && !ScreenBlanker.blanked
		onTriggered: want.setValue(Math.floor(Date.now() / 1000))
	}

	// Series name as published: temperatures may carry a C suffix once converted
	function nm(base) {
		return fields.indexOf(base) >= 0 ? base : fields.indexOf(base + "C") >= 0 ? base + "C" : ""
	}
	readonly property var paths: {
		let p = []
		for (const f of fields) {
			p.push(f)
			if (envelope.indexOf(f.replace(/C$/, "")) >= 0)
				p.push(f + "Min", f + "Max")
			if (engineOn.indexOf(f) >= 0)
				p.push(f + "OnAvg")
		}
		return p
	}
	property var items: ({})
	Instantiator {
		model: tab.paths
		delegate: VeQuickItem {
			required property string modelData
			uid: tab.svc + "/Plot/Long/" + modelData
			onValueChanged: tab.changed()
		}
		onObjectAdded: function(index, object) { tab.items[object.modelData] = object; tab.changed() }
		onObjectRemoved: function(index, object) { delete tab.items[object.modelData] }
	}

	function entry(name) {
		const it = name ? items[name] : null
		const raw = it && it.valid ? String(it.value) : ""
		if (!raw.length)
			return null
		let c = cache[name]
		if (!c || c.raw !== raw) {
			let n = 1
			for (let i = raw.indexOf(","); i >= 0; i = raw.indexOf(",", i + 1))
				++n
			c = cache[name] = { raw: raw, n: n, v: null, unit: -1, t: null }
		}
		return c
	}
	// The series as numbers, NaN where a bin is empty; temp: converted to the GX's temperature unit
	function arr(name, temp) {
		const c = entry(name)
		if (!c)
			return null
		if (!c.v) {
			const s = c.raw.split(",")
			let v = new Array(s.length)
			for (let i = 0; i < s.length; ++i)
				v[i] = s[i].length ? parseFloat(s[i]) : NaN
			c.v = v
		}
		if (!temp)
			return c.v
		const u = Global.systemSettings.temperatureUnit
		if (c.unit !== u) {
			c.t = u === VenusOS.Units_Temperature_Celsius ? c.v.map(x => isFinite(x) ? x : NaN) : c.v.map(toUserTemp)
			c.unit = u
		}
		return c.t
	}
	function toUserTemp(c) {
		return isFinite(c) ? Units.convert(c, VenusOS.Units_Temperature_Celsius, Global.systemSettings.temperatureUnit) : NaN
	}

	readonly property var specs: [
		{ k: "cur", title: "Currents", unit: "A", zero: true, series: [
			{ key: "battCurr", label: "Battery", unit: "A", color: Theme.color_orange },
			{ key: "altCurr", label: "Alternator", unit: "A", color: Theme.color_green },
			{ key: "victronCurr", label: "Victron/Solar", unit: "A", color: Theme.color_blue }] },
		{ k: "volt", title: "Battery Voltage & SOC", unit: "V", rightFixed: [0, 100], rightUnit: "%", series: [
			{ key: "battVolt", label: "Battery", unit: "V", dec: 2, color: Theme.color_green },
			{ key: "soc", label: "SOC", unit: "%", dec: 0, color: Theme.color_blue, right: true }] },
		{ k: "temp", title: "Temperatures & Barometric Pressure", unit: Global.systemSettings.temperatureUnitSuffix, rightUnit: "mbar", series: [
			{ key: "altTemp", label: "Alternator", temp: true, color: Theme.color_red },
			{ key: "battTemp", label: "Battery", temp: true, color: Theme.color_green },
			{ key: "tempTherm", label: "Thermistor", temp: true, color: Theme.color_orange, off: true },
			{ key: "ambTemp", label: "Board", temp: true, color: Theme.color_blue, off: true },
			{ key: "baro", label: "Baro", unit: "mbar", dec: 1, color: Theme.color_blackWater, right: true }] },
		{ k: "rpm", title: "Engine RPM & Alternator Duty", unit: "", zero: true, rightFixed: [0, 100], rightUnit: "%", series: [
			{ key: "rpm", label: "RPM", unit: "", dec: 0, color: Theme.color_blackWater },
			{ key: "duty", label: "Duty", unit: "%", dec: 0, color: Theme.color_font_secondary, right: true }] },
		{ k: "spd", title: "Speeds", unit: "kn", zero: true, series: [
			{ key: "sog", label: "SOG", unit: "kn", color: Theme.color_green },
			{ key: "tws", label: "True Wind", unit: "kn", color: Theme.color_blue },
			{ key: "vmg", label: "VMG", unit: "kn", color: Theme.color_blackWater },
			{ key: "aws", label: "App. Wind", unit: "kn", color: Theme.color_wasteWater }] },
		{ k: "ang", title: "Angles", unit: "°", series: [
			{ key: "cog", label: "COG", unit: "°", dec: 0, color: Theme.color_blue, wrap: true },
			{ key: "heading", label: "Heading", unit: "°", dec: 0, color: Theme.color_orange, wrap: true },
			{ key: "awa", label: "AWA", unit: "°", dec: 0, color: Theme.color_green },
			{ key: "twa", label: "TWA", unit: "°", dec: 0, color: Theme.color_lpg },
			{ key: "leeway", label: "Leeway", unit: "°", dec: 1, color: Theme.color_wasteWater }] },
		{ k: "mot", title: "Motion: Heel & Pitch", unit: "°", series: [
			{ key: "heel", label: "Heel", unit: "°", color: Theme.color_lng },
			{ key: "pitch", label: "Pitch", unit: "°", color: Theme.color_red }] }
	]

	Component.onCompleted: Qt.callLater(flush)
	onVisibleChanged: if (visible) Qt.callLater(flush)
	// new colours or temperature unit: the Repeater makes new, empty charts
	onSpecsChanged: changed()

	// Redraw when an update arrives, once per update (as XregPlotsShort)
	function changed() {
		dirty = true
		if (visible && !settle.running)
			settle.start()
	}
	function flush() {
		if (dirty && visible)
			snap()
	}
	Timer {
		id: settle
		interval: BackendConnection.type === BackendConnection.MqttSource ? 100 : 0
		onTriggered: tab.flush()
	}
	// Back from another tab: as the tab was when first opened
	function reopen() {
		if (!isNaN(cursor))
			cursor = NaN
		openedAt = Date.now()
		now = openedAt
		flick.cancelFlick()
		flick.contentY = 0
		snap(true)
	}

	// What every chart shares (the x axis, the newest values, the charge stages), then a step per chart
	function snap(reset) {
		const t0 = Date.now()
		dirty = false
		settle.stop()
		const start = Number(startItem.value), stepS = Number(stepItem.value)
		const valid = startItem.valid && stepItem.valid && stepS > 0 && fields.length > 0
		let out = { n: 0 }
		let j = { out: out, valid: valid, ticks: [], last: {}, lastRaw: "", stageRaw: "", hdr: "",
			unit: Global.systemSettings.temperatureUnit, suffix: Global.systemSettings.temperatureUnitSuffix, reset: !!reset,
			todo: order(), started: t0, work: 0 }
		if (valid) {
			let n = 0
			for (const f of fields) {
				const c = entry(f)
				if (c && c.n > n) n = c.n
			}
			let xs = []
			for (let i = 0; i < n; ++i)
				xs.push(start + i * stepS)
			out.n = n
			out.xLo = start
			out.xHi = start + Math.max(1, n) * stepS
			out.xs = xs
			j.hdr = start + "|" + stepS + "|" + n
			j.lastRaw = lastItem.valid && lastItem.value ? String(lastItem.value) : ""
			try {
				j.last = j.lastRaw ? JSON.parse(lastItem.value) : {}
			} catch (e) {
				j.last = {}
			}
			j.stageRaw = stageItem.valid ? String(stageItem.value) : ""
			const sv = j.stageRaw.length ? j.stageRaw.split(",").map(s => s.length ? parseFloat(s) : NaN) : null
			out.stages = sv ? { x: xs, c: sv.map(c => isFinite(c) ? Math.round(c) : 0) } : null
			j.ticks = ticksFor(out)
		}
		job = j
		j.work = Date.now() - t0
		if (timing)
			timing.add("longSlice", j.work)
		if (!stepPending) {
			stepPending = true
			Qt.callLater(step)
		}
	}
	function step() {
		stepPending = false
		const j = job
		if (!j)
			return
		if (!visible) {
			job = null
			dirty = true
			return
		}
		const t0 = Date.now()
		const i = j.todo.shift()
		const t = tiles.itemAt(i)
		if (t)
			show(j, specs[i], t)
		else
			dirty = true
		const done = j.todo.length === 0
		if (done) {
			job = null
			d = j.out
		} else {
			stepPending = true
			Qt.callLater(step)
		}
		const ms = Date.now() - t0
		j.work += ms
		if (timing) {
			timing.add("longSlice", ms)
			if (done) {
				timing.add("long", j.work)
				timing.add("longWall", Date.now() - j.started)
			}
		}
	}
	// Charts on screen first, then the rest in page order
	function order() {
		let on = [], off = []
		for (let i = 0; i < specs.length; ++i) {
			const t = tiles.itemAt(i)
			if (t && t.y < flick.contentY + flick.height && t.y + t.height > flick.contentY)
				on.push(i)
			else
				off.push(i)
		}
		return on.concat(off)
	}
	// One chart: its lines from the series it plots, unless none of what they are made from has changed
	function show(j, sp, t) {
		const out = j.out
		let sig = [sp, j.valid, j.hdr, j.lastRaw, j.unit, j.suffix]
		if (j.valid) {
			for (const s of sp.series) {
				const name = nm(s.key)
				sig.push(name, entry(name), entry(name + "Min"), entry(name + "Max"), entry(name + "OnAvg"))
			}
			if (sp.k === "cur")
				sig.push(j.stageRaw)
		}
		if (t.sig && t.sig.length === sig.length && sig.every((x, k) => x === t.sig[k])) {
			if (j.valid)
				out[sp.k] = t.lines
			if (t.dash)
				out[sp.k + "Dash"] = true
		} else {
			let lines = []
			if (j.valid) {
				for (const s of sp.series) {
					const name = nm(s.key)
					const v = arr(name, s.temp)
					if (!v)
						continue
					const lo = arr(name + "Min", s.temp), hi = arr(name + "Max", s.temp)
					const on = arr(name + "OnAvg")
					const lastV = j.last[s.key]
					const base = { unit: s.temp ? j.suffix : s.unit, dec: s.dec !== undefined ? s.dec : 1, color: s.color,
						right: !!s.right, wrap: !!s.wrap, off: !!s.off, x: out.xs,
						last: typeof lastV === "number" ? (s.temp ? toUserTemp(lastV) : lastV) : NaN }
					if (on) {
						lines.push(Object.assign({}, base, { key: s.key, label: s.label, v: on, lo: lo, hi: hi }))
						lines.push(Object.assign({}, base, { key: s.key + "W", v: v, dash: true, w: 1, noLegend: true }))
						out[sp.k + "Dash"] = true
					} else {
						lines.push(Object.assign({}, base, { key: s.key, label: s.label, v: v, lo: lo, hi: hi }))
					}
				}
				out[sp.k] = lines
			}
			const xLo = out.n > 0 ? out.xLo : 0, xHi = out.n > 0 ? out.xHi : 1
			// XregChart redraws in full for each of lines, xLo and xHi that changes: a new x range goes in on no lines
			if ((t.xLo !== xLo || t.xHi !== xHi) && t.lines.length)
				t.lines = []
			t.n = out.n
			t.xLo = xLo
			t.xHi = xHi
			if (t.ticks !== j.ticks)
				t.ticks = j.ticks
			t.stages = sp.k === "cur" ? out.stages || null : null
			t.dash = !!out[sp.k + "Dash"]
			t.lines = lines
			t.sig = sig
		}
		// back from another tab: a legend entry tapped off or a cursor of its own, as on a new chart
		if (j.reset && (Object.keys(t.chart.hiddenKeys).length > 0 || !isNaN(t.chart.ownCursor)))
			t.resets++
	}
	Timer {
		interval: 1000
		repeat: true
		triggeredOnStart: true
		running: tab.visible
		onTriggered: tab.now = Date.now()
	}

	// Ticks on local-time boundaries
	function ticksFor(r) {
		if (!(r.n > 0))
			return []
		const span = r.xHi - r.xLo
		const st = span <= 3700 ? 600 : span <= 22000 ? 3600 : span <= 90000 ? 14400 : span <= 700000 ? 86400 : 432000
		const fmt = span <= 90000 ? "hh:mm" : span <= 700000 ? "ddd d" : "MMM d"
		const tzo = new Date(r.xLo * 1000).getTimezoneOffset() * 60
		let t = []
		for (let s = Math.ceil((r.xLo - tzo) / st) * st + tzo; s <= r.xHi; s += st)
			t.push({ x: s, t: Qt.formatDateTime(new Date(s * 1000), fmt) })
		return t
	}
	readonly property string noData: now - openedAt < 60000 ? "Loading long-term history" : "No long-term records yet"
	readonly property string status: {
		if (!fieldsItem.valid || fields.length === 0)
			return ""
		// the range tapped, before the bridge echoes /Ui/LongRange
		if (echoItem.valid && String(echoItem.value) !== rangeSync.shown)
			return "Loading " + rangeSync.shown
		if (!isNaN(cursor))
			return Qt.formatDateTime(new Date(cursor * 1000), spans[ranges.indexOf(range)] > 90000 ? "ddd d MMM hh:mm" : "hh:mm")
		return ""
	}

	SegmentedButtonRow {
		id: rangeRow
		x: tab.toolX
		y: tab.toolY
		width: Math.min(tab.width - tab.m - x, 320)
		height: 40
		fontPixelSize: Theme.font_size_body1
		model: [{ value: "1h" }, { value: "6h" }, { value: "24h" }, { value: "7d" }, { value: "30d" }]
		currentIndex: tab.ranges.indexOf(rangeSync.shown)
		onButtonClicked: function(buttonIndex) {
			rangeItem.setValue(tab.ranges[buttonIndex])
			rangeSync.expect(tab.ranges[buttonIndex])
			// the click sets currentIndex itself; follow the range again, which every screen on this GX shares
			Qt.callLater(() => rangeRow.currentIndex = Qt.binding(() => tab.ranges.indexOf(rangeSync.shown)))
		}
	}
	XregSync {
		id: rangeSync
		value: tab.range
	}
	Label {
		anchors { left: rangeRow.right; leftMargin: 14; right: parent.right; rightMargin: tab.m; verticalCenter: rangeRow.verticalCenter }
		text: tab.status
		elide: Text.ElideRight
		horizontalAlignment: Text.AlignRight
		color: Theme.color_font_secondary
		font.pixelSize: Theme.font_size_body1
	}

	Flickable {
		id: flick
		x: tab.m
		y: tab.bodyY
		width: tab.width - 2 * tab.m
		height: tab.height - tab.bodyY - tab.m
		contentHeight: grid.height
		clip: true
		interactive: contentHeight > height

		Grid {
			id: grid
			readonly property real gap: 10
			readonly property real tileW: tab.oneColumn ? parent.width : (parent.width - gap) / 2
			readonly property real tileH: tab.oneColumn ? 260 : Math.max(230, (tab.height - tab.bodyY - tab.m - gap) / 2)
			columns: tab.oneColumn ? 1 : 2
			spacing: gap

			Repeater {
				id: tiles
				model: tab.specs
				delegate: Rectangle {
					id: tile
					required property var modelData
					// this chart's part of the last redraw, set by show()
					property var lines: []
					property real xLo: 0
					property real xHi: 1
					property var ticks: []
					property var stages: null
					property bool dash
					property int n
					property var sig: null
					property int resets
					readonly property Item chart: chartItem
					width: grid.tileW
					height: grid.tileH
					radius: Theme.geometry_overviewPage_widget_radius
					color: Theme.color_background_secondary
					XregChart {
						id: chartItem
						anchors { fill: parent; margins: 10 }
						title: modelData.title
						lines: tile.lines
						xLo: tile.xLo
						xHi: tile.xHi
						xTicks: tile.ticks
						leftFromZero: !!modelData.zero
						leftUnit: modelData.unit
						rightFixed: modelData.rightFixed || null
						rightUnit: modelData.rightUnit || ""
						stages: tile.stages
						dashNote: tile.dash ? "dashed: incl. engine off" : ""
						emptyText: tile.n > 0 ? "No data in this range" : tab.noData
						cursorX: tab.cursor
						resets: tile.resets
						onScrubbed: function(xValue) { tab.cursor = xValue }
					}
				}
			}
		}
	}
}
