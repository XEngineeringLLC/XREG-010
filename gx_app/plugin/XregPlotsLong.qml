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
	property var d: ({})
	property bool dirty: true
	property real cursor: NaN
	property real openedAt: Date.now()
	property real now: Date.now()

	VeQuickItem { id: want; uid: tab.svc + "/Ui/Want/Long" }
	VeQuickItem { id: rangeItem; uid: tab.svc + "/Ui/LongRange" }
	VeQuickItem { id: fieldsItem; uid: tab.svc + "/Plot/Long/Fields"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: startItem; uid: tab.svc + "/Plot/Long/StartEpoch"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: stepItem; uid: tab.svc + "/Plot/Long/StepS"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: echoItem; uid: tab.svc + "/Plot/Long/Range"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: stageItem; uid: tab.svc + "/Plot/Long/chargeStage"; onValueChanged: tab.dirty = true }
	// each field's newest recorded value in the whole ring: the legend's value when its window has none
	VeQuickItem { id: lastItem; uid: tab.svc + "/Plot/Long/Last"; onValueChanged: tab.dirty = true }
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
			onValueChanged: tab.dirty = true
		}
		onObjectAdded: function(index, object) { tab.items[object.modelData] = object; tab.dirty = true }
		onObjectRemoved: function(index, object) { delete tab.items[object.modelData] }
	}

	function arr(name) {
		const it = name ? items[name] : null
		return it && it.valid && String(it.value).length ? String(it.value).split(",").map(s => s.length ? parseFloat(s) : NaN) : null
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

	function snap() {
		dirty = false
		const start = Number(startItem.value), stepS = Number(stepItem.value)
		let out = { n: 0 }
		if (!startItem.valid || !stepItem.valid || !(stepS > 0) || fields.length === 0) {
			d = out
			return
		}
		let n = 0
		for (const f of fields) {
			const a = arr(f)
			if (a && a.length > n) n = a.length
		}
		let xs = []
		for (let i = 0; i < n; ++i)
			xs.push(start + i * stepS)
		out.n = n
		out.xLo = start
		out.xHi = start + Math.max(1, n) * stepS
		out.xs = xs
		const tempSuffix = Global.systemSettings.temperatureUnitSuffix
		let lastMap = {}
		try {
			lastMap = lastItem.valid && lastItem.value ? JSON.parse(lastItem.value) : {}
		} catch (e) {
			lastMap = {}
		}
		for (const sp of specs) {
			let lines = []
			for (const s of sp.series) {
				const name = nm(s.key)
				let v = arr(name)
				if (!v)
					continue
				let lo = arr(name + "Min"), hi = arr(name + "Max")
				const on = arr(name + "OnAvg")
				if (s.temp) {
					v = v.map(toUserTemp)
					lo = lo ? lo.map(toUserTemp) : null
					hi = hi ? hi.map(toUserTemp) : null
				}
				const lastV = lastMap[s.key]
				const base = { unit: s.temp ? tempSuffix : s.unit, dec: s.dec !== undefined ? s.dec : 1, color: s.color,
					right: !!s.right, wrap: !!s.wrap, off: !!s.off, x: xs,
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
		const sv = stageItem.valid && String(stageItem.value).length
			? String(stageItem.value).split(",").map(s => s.length ? parseFloat(s) : NaN) : null
		out.stages = sv ? { x: xs, c: sv.map(c => isFinite(c) ? Math.round(c) : 0) } : null
		d = out
	}
	Timer {
		interval: 1000
		repeat: true
		triggeredOnStart: true
		running: tab.visible
		onTriggered: {
			tab.now = Date.now()
			if (tab.dirty)
				tab.snap()
		}
	}

	// Ticks on local-time boundaries
	readonly property var ticks: {
		if (!(d.n > 0))
			return []
		const span = d.xHi - d.xLo
		const st = span <= 3700 ? 600 : span <= 22000 ? 3600 : span <= 90000 ? 14400 : span <= 700000 ? 86400 : 432000
		const fmt = span <= 90000 ? "hh:mm" : span <= 700000 ? "ddd d" : "MMM d"
		const tzo = new Date(d.xLo * 1000).getTimezoneOffset() * 60
		let t = []
		for (let s = Math.ceil((d.xLo - tzo) / st) * st + tzo; s <= d.xHi; s += st)
			t.push({ x: s, t: Qt.formatDateTime(new Date(s * 1000), fmt) })
		return t
	}
	readonly property string noData: now - openedAt < 60000 ? "Loading long-term history" : "No long-term records yet"
	readonly property string status: {
		if (!fieldsItem.valid || fields.length === 0)
			return ""
		if (echoItem.valid && String(echoItem.value) !== range)
			return "Loading " + range
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
				model: tab.specs
				delegate: Rectangle {
					required property var modelData
					width: grid.tileW
					height: grid.tileH
					radius: Theme.geometry_overviewPage_widget_radius
					color: Theme.color_background_secondary
					XregChart {
						anchors { fill: parent; margins: 10 }
						title: modelData.title
						lines: tab.d[modelData.k] || []
						xLo: tab.d.n > 0 ? tab.d.xLo : 0
						xHi: tab.d.n > 0 ? tab.d.xHi : 1
						xTicks: tab.ticks
						leftFromZero: !!modelData.zero
						leftUnit: modelData.unit
						rightFixed: modelData.rightFixed || null
						rightUnit: modelData.rightUnit || ""
						stages: modelData.k === "cur" ? tab.d.stages || null : null
						dashNote: tab.d[modelData.k + "Dash"] ? "dashed: incl. engine off" : ""
						emptyText: tab.d.n > 0 ? "No data in this range" : tab.noData
						cursorX: tab.cursor
						onScrubbed: function(xValue) { tab.cursor = xValue }
					}
				}
			}
		}
	}
}
