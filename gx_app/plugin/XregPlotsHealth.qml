import QtQuick
import Victron.VenusOS

// Plots > Health: the web app's Charging System Health panel. Summary (live % of best, confidence, coverage),
// health vs engine-hours (average and low-10% lines from /alttrend.csv) and This Session (a dot every 5 s from
// /altsess.csv, then live from /Health/Live while the tab is open). Math and words follow script.js
// (updateAltHealth, altTrendData, altSessionPush, altSessAdoptHistory, renderAltSessPoint).
Item {
	id: tab

	property string svc
	property real m: 14
	property real toolX
	property real toolY
	property real bodyY
	property int windowH: 0
	signal windowPicked(int hours)

	readonly property var stateLabels: ["MEASURED", "ESTIMATED", "Learning this operating region", "No reference here yet"]
	readonly property bool oneColumn: width < 760
	property real now: Date.now() / 1000
	property real cursor: NaN

	VeQuickItem { id: want; uid: tab.svc + "/Ui/Want/Health" }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: tab.visible && !ScreenBlanker.blanked
		onTriggered: {
			want.setValue(Math.floor(Date.now() / 1000))
			trendWant.setValue(Math.floor(Date.now() / 1000))
		}
	}
	// The regulator thins the trend to 200 points within this many engine-hours (0 = all). The bridge keeps one trend
	// per window, so each screen asks for its own window's and shows only that.
	VeQuickItem { id: trendWant; uid: tab.svc + "/Ui/Want/HealthTrend/" + tab.windowH }
	onWindowHChanged: Qt.callLater(() => trendWant.setValue(Math.floor(Date.now() / 1000)))

	VeQuickItem { id: hValid; uid: tab.svc + "/Health/Live/valid" }
	VeQuickItem { id: hPct; uid: tab.svc + "/Health/Live/pct" }
	VeQuickItem { id: hState; uid: tab.svc + "/Health/Live/state" }
	VeQuickItem { id: hStatus; uid: tab.svc + "/Health/Live/status" }
	VeQuickItem { id: hSource; uid: tab.svc + "/Health/Live/source" }
	VeQuickItem { id: hPaused; uid: tab.svc + "/Health/Live/paused" }
	VeQuickItem { id: hPts; uid: tab.svc + "/Health/Live/ptCount" }
	VeQuickItem { id: hEng; uid: tab.svc + "/Health/Live/engHours" }
	VeQuickItem { id: hSessN; uid: tab.svc + "/Health/Live/sessionN" }
	VeQuickItem { id: hSessMean; uid: tab.svc + "/Health/Live/sessionMean" }
	VeQuickItem { id: hSessP10; uid: tab.svc + "/Health/Live/sessionP10" }
	VeQuickItem { id: hHiField; uid: tab.svc + "/Health/Live/hiFieldAlert" }
	VeQuickItem { id: hSessSteady; uid: tab.svc + "/Health/Live/sessSteady" }
	VeQuickItem { id: hSteady; uid: tab.svc + "/Health/Live/steady" }
	VeQuickItem { id: hRpm; uid: tab.svc + "/Health/Live/rpm" }
	VeQuickItem { id: hGAmps; uid: tab.svc + "/Health/Live/gAmps" }
	VeQuickItem { id: hAmps; uid: tab.svc + "/Health/Live/amps" }
	VeQuickItem { id: hPred; uid: tab.svc + "/Health/Live/pred" }
	VeQuickItem { id: hTSlope; uid: tab.svc + "/Health/Live/tSlope" }
	VeQuickItem { id: trendCsv; uid: tab.svc + "/Health/TrendCsv/" + tab.windowH }
	VeQuickItem {
		id: sessCsv
		uid: tab.svc + "/Health/SessionCsv"
		onValueChanged: tab.adoptSession()
	}
	Component.onCompleted: adoptSession()
	VeQuickItem { id: liveTemp; uid: tab.svc + "/Live/AltTemp" }
	VeQuickItem { id: liveField; uid: tab.svc + "/Live/Field" }
	VeQuickItem { id: liveBattV; uid: tab.svc + "/Live/BattV" }

	function num(item) { return item.valid && item.value !== null && item.value !== "" ? Number(item.value) : NaN }
	function stateColor(st) { return st === 0 ? Theme.color_green : st === 1 ? Theme.color_blue : Theme.color_font_secondary }
	function userTemp(v, fromUnit) { return isFinite(v) ? Units.convert(v, fromUnit, Global.systemSettings.temperatureUnit) : NaN }

	// Summary, as updateAltHealth
	readonly property int st: isFinite(num(hState)) ? num(hState) | 0 : 3
	readonly property bool live: num(hValid) >= 0.5
	readonly property bool graded: live && (st === 0 || st === 1) && num(hPct) > 0
	readonly property string statusText: {
		const s = num(hStatus)
		let t = s === 3 ? "Disabled (Ignore Temperature)"
			: s === 4 ? "Waiting for commissioning"
			: s === 5 ? "Waiting for Charge Health Calibration (commissioning step 10)"
			: !live ? "Not running"
			: stateLabels[st] || ""
		if (s !== 3 && num(hSource) >= 1) t += " · Uploaded reference"
		if (s === 0 && num(hPaused) >= 1) t += " · learning paused"
		return hValid.valid ? t : "Waiting for data"
	}
	readonly property int sessN: Math.max(0, Math.round(num(hSessN) || 0))

	// Health vs engine-hours, as altTrendData / altTrendXRange / altTrendYRange
	readonly property var trend: {
		let pts = []
		const lines = trendCsv.valid ? String(trendCsv.value).trim().split("\n") : []
		for (let i = 1; i < lines.length; ++i) {
			const c = lines[i].split(",")
			if (c.length >= 3 && isFinite(parseFloat(c[0])))
				pts.push({ eng: +c[0], worst: +c[1], overall: +c[2] })
		}
		return pts.sort((a, b) => a.eng - b.eng)
	}
	readonly property bool trendDemo: trend.length < 3 && (num(hEng) || 0) < 3
	readonly property var trendView: {
		let steps = []
		for (let i = 1; i < trend.length; ++i) {
			const s = trend[i].eng - trend[i - 1].eng
			if (s > 0) steps.push(s)
		}
		steps.sort((a, b) => a - b)
		const gap = Math.max(3, (steps.length ? steps[Math.floor(steps.length / 2)] : 1) * 3)
		let xs = [], avg = [], low = [], prev = null, maxE = 0, lo = 70, hi = 105
		for (const p of trend) {
			if (prev !== null && p.eng - prev > gap) {
				xs.push(prev + (p.eng - prev) / 2)
				avg.push(NaN)
				low.push(NaN)
			}
			xs.push(p.eng)
			avg.push(p.overall)
			low.push(p.worst)
			prev = p.eng
			maxE = Math.max(maxE, p.eng)
			if (isFinite(p.worst) && p.worst < lo) lo = Math.max(0, Math.floor(p.worst / 5) * 5)
			if (isFinite(p.worst) && p.worst > hi) hi = Math.ceil(p.worst)
			if (isFinite(p.overall) && p.overall > hi) hi = Math.ceil(p.overall)
		}
		const win = windowH
		const xr = win > 0 ? [Math.max(0, maxE - win), Math.max(win, maxE)] : [0, Math.max(4, Math.ceil(maxE))]
		const blue = Theme.color_blue
		return {
			xLo: xr[0], xHi: xr[1], yr: [lo, hi],
			lines: [
				{ key: "ref", x: xr, v: [100, 100], color: Theme.color_font_disabled, w: 1, dash: true, noLegend: true },
				{ key: "avg", label: "average", unit: "%", dec: 0, x: xs, v: avg, color: Qt.rgba(blue.r, blue.g, blue.b, 0.45), w: 1.5 },
				{ key: "low", label: "low 10%", unit: "%", dec: 0, x: xs, v: low, color: blue, w: 2.2 },
				{ key: "lowDots", x: xs, v: low, color: blue, w: 5, dots: true, noLegend: true }
			]
		}
	}
	readonly property var trendTicks: {
		const span = trendView.xHi - trendView.xLo
		const raw = span / 5, p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p
		const st = (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * p
		let t = []
		for (let k = Math.ceil(trendView.xLo / st - 1e-9); k * st <= trendView.xHi + 1e-9; ++k)
			t.push({ x: k * st, t: String(Math.round(k * st * 10) / 10) + " h" })
		return t
	}

	// This Session: rows from /altsess.csv anchored to when the text arrived, plus live dots since then
	property var sessRows: []
	property real sessAt: 0
	property var liveRows: []
	function adoptSession() {
		// read value, not valid: inside a value-change handler valid may not have caught up yet
		const txt = sessCsv.value !== undefined && sessCsv.value !== null ? String(sessCsv.value) : ""
		let nowMs = null, rows = [], prev = null
		const at = Date.now() / 1000
		for (const ln of txt.split("\n")) {
			if (!ln.length)
				continue
			if (ln[0] === "#") {
				const mm = ln.match(/nowMs=(\d+)/)
				if (mm) nowMs = +mm[1]
				continue
			}
			const c = ln.split(",")
			if (ln[0] === "t" || c.length < 12)
				continue
			const tMs = +c[0]
			// device millis can wrap; rows must move forward in uint32 arithmetic
			if (prev !== null) {
				const fwd = (tMs - prev) >>> 0
				if (fwd === 0 || fwd >= 0x80000000)
					continue
			}
			prev = tMs
			rows.push({ tMs: tMs, pct: +c[1], amps: +c[2], measAmps: +c[3], pred: +c[4], rpm: +c[5], fieldPct: +c[6],
				tempF: +c[7], vbus: +c[8], tSlope: +c[9], state: +c[10], ring: +c[11] })
		}
		if (nowMs === null) {
			sessRows = []
			return
		}
		sessRows = rows.map(r => {
			const g = (r.state === 0 || r.state === 1) && r.pct > 0
			return { t: at - ((nowMs - r.tMs) >>> 0) / 1000, g: g, pct: r.pct, state: r.state, ring: g && r.ring === 1,
				rpm: r.rpm, amps: r.amps > 0 ? r.amps : r.measAmps, pred: r.pred, fieldPct: r.fieldPct, vbus: r.vbus,
				temp: userTemp(r.tempF, VenusOS.Units_Temperature_Fahrenheit), tSlope: r.tSlope }
		})
		sessAt = at
		liveRows = liveRows.filter(r => r.t > at)
	}
	function pushLive() {
		const t = Date.now() / 1000
		const s = st
		const g = graded && num(hSessSteady) >= 1
		let rows = liveRows.filter(r => r.t > t - 1800)
		rows.push({ t: t, g: g, pct: num(hPct), state: s, ring: g && num(hSteady) >= 1, rpm: num(hRpm),
			amps: num(hGAmps) > 0 ? num(hGAmps) : num(hAmps), pred: num(hPred), fieldPct: num(liveField), vbus: num(liveBattV),
			temp: userTemp(num(liveTemp), VenusOS.Units_Temperature_Celsius), tSlope: num(hTSlope) })
		liveRows = rows
	}
	Timer {
		interval: 5000
		repeat: true
		running: tab.visible && hValid.valid
		onTriggered: tab.pushLive()
	}
	Timer {
		interval: 5000
		repeat: true
		triggeredOnStart: true
		running: tab.visible
		onTriggered: tab.now = Date.now() / 1000
	}
	readonly property var sess: {
		const all = sessRows.concat(liveRows).filter(r => r.t > now - 1830)
		let xs = [], meas = [], est = [], ringV = [], shades = [], lo = Infinity, hi = -Infinity, runStart = null, prevT = null
		for (const r of all) {
			const gapBefore = prevT !== null && r.t - prevT > 10
			if (gapBefore && runStart === null)
				runStart = prevT
			if (!r.g) {
				if (runStart === null) runStart = r.t
			} else if (runStart !== null) {
				shades.push([runStart, r.t])
				runStart = null
			}
			xs.push(r.t)
			meas.push(r.g && r.state === 0 ? r.pct : NaN)
			est.push(r.g && r.state === 1 ? r.pct : NaN)
			ringV.push(r.ring ? r.pct : NaN)
			if (r.g) {
				lo = Math.min(lo, r.pct)
				hi = Math.max(hi, r.pct)
			}
			prevT = r.t
		}
		if (runStart !== null)
			shades.push([runStart, now])
		const pad = Math.max(2, (hi - lo) * 0.15)
		return {
			rows: all,
			yr: isFinite(lo) ? [Math.floor(lo - pad), Math.ceil(hi + pad)] : [80, 110],
			shades: shades,
			lines: [
				{ key: "ring", x: xs, v: ringV, color: Theme.color_orange, w: 9.6, dots: true, noLegend: true },
				{ key: "hole", x: xs, v: ringV, color: Theme.color_background_secondary, w: 6.4, dots: true, noLegend: true },
				{ key: "meas", label: "Measured", unit: "%", dec: 0, x: xs, v: meas, color: Theme.color_green, w: 5.4, dots: true },
				{ key: "est", label: "Estimated", unit: "%", dec: 0, x: xs, v: est, color: Theme.color_blue, w: 5.4, dots: true }
			]
		}
	}
	readonly property var sessTicks: {
		let t = []
		for (let mins = 30; mins >= 0; mins -= 5)
			t.push({ x: now - mins * 60, t: mins === 0 ? "now" : mins + "m" })
		return t
	}
	// Graded dot nearest the cursor, within a minute
	readonly property var picked: {
		if (isNaN(cursor))
			return null
		let best = null, bestD = 60
		for (const r of sess.rows) {
			if (r.g && Math.abs(r.t - cursor) < bestD) {
				bestD = Math.abs(r.t - cursor)
				best = r
			}
		}
		return best
	}
	function fx(v, dp, u) { return isFinite(v) ? v.toFixed(dp) + u : "--" }
	readonly property string pickedLine1: {
		const p = picked
		if (!p)
			return "Tap a dot to see the conditions behind it."
		const ago = Math.max(0, Math.round((now - p.t) / 60))
		return Math.round(p.pct) + "% · " + (p.state === 0 ? "Measured" : "Estimated") + (p.ring ? " · full steady run" : " · brief point")
			+ " · " + (ago === 0 ? "just now" : ago + " min ago")
	}
	readonly property string pickedLine2: {
		const p = picked
		if (!p)
			return ""
		const unitC = Global.systemSettings.temperatureUnit !== VenusOS.Units_Temperature_Fahrenheit
		const slope = isFinite(p.tSlope) ? (unitC ? p.tSlope * 5 / 9 : p.tSlope) : NaN
		return "RPM " + fx(p.rpm, 0, "") + " · " + fx(p.amps, 1, " A") + " output · "
			+ fx(p.temp, 0, " " + Global.systemSettings.temperatureUnitSuffix) + " alt temp · " + fx(p.fieldPct, 0, "%") + " field · expected "
			+ fx(p.pred, 1, " A") + " · " + fx(p.vbus, 2, " V")
			+ (isFinite(slope) ? " · temp " + (slope >= 0 ? "+" : "") + slope.toFixed(1) + "°/min" : "")
	}

	Row {
		x: tab.toolX
		y: tab.toolY
		height: 40
		spacing: 12
		Label {
			anchors.verticalCenter: parent.verticalCenter
			text: "Window"
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_body1
		}
		SegmentedButtonRow {
			width: 240
			height: 40
			fontPixelSize: Theme.font_size_body1
			model: [{ value: "10 h" }, { value: "100 h" }, { value: "All" }]
			currentIndex: [10, 100, 0].indexOf(tab.windowH)
			onButtonClicked: function(buttonIndex) { tab.windowPicked([10, 100, 0][buttonIndex]) }
		}
	}

	Flickable {
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
			spacing: 10

			Rectangle {
				width: body.width
				height: 82
				radius: Theme.geometry_overviewPage_widget_radius
				color: Theme.color_background_secondary
				Column {
					id: summaryLeft
					x: 14
					anchors.verticalCenter: parent.verticalCenter
					spacing: 2
					Label {
						text: "Charging System Health"
						color: Theme.color_font_secondary
						font.pixelSize: Theme.font_size_caption
					}
					Row {
						spacing: 12
						XregNum {
							template: "100%"
							text: tab.graded ? Math.round(tab.num(hPct)) + "%" : "--"
							color: tab.graded ? tab.stateColor(tab.st) : Theme.color_font_primary
							font.pixelSize: Theme.font_size_h1
							font.bold: true
						}
						Column {
							anchors.verticalCenter: parent.verticalCenter
							Label {
								text: tab.statusText
								color: tab.graded ? tab.stateColor(tab.st) : Theme.color_font_secondary
								font.pixelSize: Theme.font_size_body1
							}
							Label {
								visible: tab.num(hHiField) >= 1
								text: "Low output despite high field drive"
								color: Theme.color_red
								font.pixelSize: Theme.font_size_caption
								font.bold: true
							}
						}
					}
				}
				Column {
					anchors { right: parent.right; rightMargin: 14; verticalCenter: parent.verticalCenter }
					width: Math.max(0, parent.width - summaryLeft.width - 60)
					spacing: 2
					Label {
						width: parent.width
						horizontalAlignment: Text.AlignRight
						elide: Text.ElideRight
						text: !hPts.valid ? ""
							: tab.num(hPts) > 0 ? Math.round(tab.num(hPts)) + " best surface points · " + Math.round(tab.num(hEng)) + " engine-hr"
							: "no best surface points yet"
						color: Theme.color_font_secondary
						font.pixelSize: Theme.font_size_caption
					}
					Label {
						width: parent.width
						horizontalAlignment: Text.AlignRight
						elide: Text.ElideRight
						text: !hSessN.valid ? "" : tab.sessN > 0 ? tab.sessN.toLocaleString(Qt.locale(), "f", 0) + " graded point" + (tab.sessN === 1 ? "" : "s") + " this session"
							: "no graded points yet this session"
						font.pixelSize: Theme.font_size_caption
					}
					Label {
						width: parent.width
						horizontalAlignment: Text.AlignRight
						visible: tab.sessN >= 10
						text: "mean " + Math.round(tab.num(hSessMean)) + "% · 10th percentile " + Math.round(tab.num(hSessP10)) + "%"
						color: Theme.color_font_secondary
						font.pixelSize: Theme.font_size_caption
					}
				}
			}

			Grid {
				id: charts
				readonly property real gap: 10
				readonly property real tileW: tab.oneColumn ? body.width : (body.width - gap) / 2
				readonly property real tileH: tab.oneColumn ? 280 : Math.max(240, tab.height - tab.bodyY - tab.m - 82 - 10)
				columns: tab.oneColumn ? 1 : 2
				spacing: gap

				Rectangle {
					width: charts.tileW
					height: charts.tileH
					radius: Theme.geometry_overviewPage_widget_radius
					color: Theme.color_background_secondary
					XregChart {
						anchors { fill: parent; margins: 10 }
						title: "Charging System Health vs Engine Hours"
						lines: tab.trendDemo ? [] : tab.trendView.lines
						xLo: tab.trendView.xLo
						xHi: tab.trendView.xHi
						xTicks: tab.trendTicks
						leftFixed: tab.trendView.yr
						leftUnit: "%"
						emptyText: !trendCsv.valid || !String(trendCsv.value).length ? "Waiting for data"
							: tab.trendDemo ? "Real results appear after 3 engine-hours (" + Math.min(tab.trend.length, 3) + " of 3 collected)"
							: "No points in this window"
					}
				}
				Rectangle {
					width: charts.tileW
					height: charts.tileH
					radius: Theme.geometry_overviewPage_widget_radius
					color: Theme.color_background_secondary
					XregChart {
						id: sessChart
						anchors { left: parent.left; right: parent.right; top: parent.top; bottom: detail.top; margins: 10 }
						title: "This Session"
						lines: tab.sess.lines
						xLo: tab.now - 1800
						xHi: tab.now
						xTicks: tab.sessTicks
						leftFixed: tab.sess.yr
						leftUnit: "%"
						shades: tab.sess.shades
						emptyText: "No graded points in the last 30 minutes"
						cursorX: tab.cursor
						onScrubbed: function(xValue) { if (!isNaN(xValue)) tab.cursor = xValue }
					}
					Column {
						id: detail
						anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 12 }
						height: 3 * Math.ceil(Theme.font_size_caption * 1.25) + 4
						spacing: 2
						Label {
							width: parent.width
							elide: Text.ElideRight
							text: tab.pickedLine1
							color: tab.picked ? tab.stateColor(tab.picked.state) : Theme.color_font_secondary
							font.pixelSize: Theme.font_size_caption
							font.bold: !!tab.picked
						}
						Label {
							width: parent.width
							wrapMode: Text.WordWrap
							maximumLineCount: 2
							text: tab.pickedLine2
							color: Theme.color_font_secondary
							font.pixelSize: Theme.font_size_caption
						}
					}
				}
			}
		}
	}
}
