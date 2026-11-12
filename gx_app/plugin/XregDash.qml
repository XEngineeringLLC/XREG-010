import QtQuick
import QtQuick.Shapes
import Victron.VenusOS

// Dashboard, the first page of the home page (XregHome, under its header with Alternator Enable), for one regulator
// (svc: its service uid).
// Top: the web app's header banner (wordmark, status words, off-reason strip, chips).
// Below: power flow (XregFlow: the battery, then a row per source and load on one fixed scale), the app's boat
// speed-performance view (XregChartPerf), apparent wind for the last hour and barometer for today, yesterday and the day
// before on one midnight-to-midnight axis. A tap on any of them or on a chip opens its larger view (XregPop on the home
// page); a drag still scrolls.
// Under 600 px wide (remote console on a phone held upright) the same pieces stack in one scrolling column, the power
// flow as tall as its rows.
// Reads the bridge's /Live and /History paths, its wind hour /Plot/Wind (XregWindStore), and its fast lane /Fast/Live
// for the CSV1 readouts while it is sent; temperatures arrive in Celsius and show in the owner's unit.
// Status, stage, limiter and off-reason words are the web app's own (script.js fieldOffReasonText etc.).
Item {
	id: root

	property bool animationEnabled
	property string svc
	property QtObject timing           // XregHome's bench timing log
	readonly property bool linkUp: connected.valid && connected.value === 1
	readonly property color brand: "#00a19a"    // web app --accent
	readonly property real m: 14
	readonly property real gap: 10
	readonly property bool portrait: width < 600
	readonly property real sc: portrait ? 0.85 : Math.min(1, width / 1024)    // 0.78 on an 800-wide GX Touch 50
	// 5-inch GX Touch 50 (372 px under the header): banner, strip and chips packed closer, so the tiles below keep 214 px
	readonly property bool tight: !portrait && height < 420
	readonly property real topY: tight ? 0 : 4
	readonly property real bannerH: tight ? 40 : 46
	readonly property real stripY: portrait ? 136 : topY + bannerH + 4
	readonly property real chipsY: stripY + (tight ? 34 : 36)
	readonly property real chipsH: tight ? 64 : 76
	readonly property real chipW: portrait ? (width - 2 * m - gap) / 2 : (width - 2 * m - 4 * gap) / 5
	readonly property real lowerY: chipsY + (portrait ? 3 * chipsH + 2 * gap : chipsH) + (tight ? 8 : gap)
	readonly property real lowerH: portrait ? 0 : height - lowerY - (tight ? 8 : m)
	readonly property real colW: portrait ? width - 2 * m : width * 0.27
	readonly property real flowH: portrait ? flowTile.implicitHeight : lowerH
	readonly property real perfX: portrait ? m : m + colW + gap
	readonly property real perfY: portrait ? lowerY + flowH + gap : lowerY
	readonly property real perfW: portrait ? width - 2 * m : Math.round(width * 0.42)
	readonly property real perfH: portrait ? 340 : lowerH
	readonly property real sideX: portrait ? m : perfX + perfW + gap
	readonly property real sideY: portrait ? perfY + perfH + gap : lowerY
	readonly property real sideW: portrait ? width - 2 * m : width - sideX - m
	readonly property real sideH: portrait ? 2 * 160 + gap : lowerH

	VeQuickItem { id: connected; uid: root.svc + "/Connected" }
	VeQuickItem { id: altAmps; uid: root.svc + "/Live/AltAmps" }
	// highs and lows since the regulator started (its ignition-cycle watermarks, as the web app's Live Data shows them),
	// for the chips' larger views: the battery current's from the shunt (no shunt, none) and the battery temperature's
	// from the probe
	VeQuickItem { id: altAmpsHi; uid: root.svc + "/Live/AltAmpsHi" }
	VeQuickItem { id: altAmpsLo; uid: root.svc + "/Live/AltAmpsLo" }
	VeQuickItem { id: battAHi; uid: root.svc + "/Live/BattAHi" }
	VeQuickItem { id: battALo; uid: root.svc + "/Live/BattALo" }
	VeQuickItem { id: battVHi; uid: root.svc + "/Live/BattVHi" }
	VeQuickItem { id: battVLo; uid: root.svc + "/Live/BattVLo" }
	VeQuickItem { id: socHi; uid: root.svc + "/Live/SocHi" }
	VeQuickItem { id: socLo; uid: root.svc + "/Live/SocLo" }
	VeQuickItem { id: rpmHi; uid: root.svc + "/Live/RpmHi" }
	VeQuickItem { id: rpmLo; uid: root.svc + "/Live/RpmLo" }
	TempItem { id: altTempHi; uid: root.svc + "/Live/AltTempHi" }
	TempItem { id: altTempLo; uid: root.svc + "/Live/AltTempLo" }
	TempItem { id: battTempHi; uid: root.svc + "/Live/BattTempHi" }
	TempItem { id: battTempLo; uid: root.svc + "/Live/BattTempLo" }
	// when each pair started (the regulator's boot or an owner's reset), GX clock seconds; a panel's Reset writes
	// /Live/WmReset with its names, answered on /Settings/Result as "wmReset"
	VeQuickItem { id: sinceAmps; uid: root.svc + "/Live/WmSince/amps" }
	VeQuickItem { id: sinceBcur; uid: root.svc + "/Live/WmSince/Bcur" }
	VeQuickItem { id: sinceIbv; uid: root.svc + "/Live/WmSince/IBV" }
	VeQuickItem { id: sinceSoc; uid: root.svc + "/Live/WmSince/SOC" }
	VeQuickItem { id: sinceAltTemp; uid: root.svc + "/Live/WmSince/altTempF" }
	VeQuickItem { id: sinceBattTemp; uid: root.svc + "/Live/WmSince/battTempF" }
	VeQuickItem { id: sinceRpm; uid: root.svc + "/Live/WmSince/RPM" }
	VeQuickItem { id: wmReset; uid: root.svc + "/Live/WmReset" }
	VeQuickItem { id: settingsResult; uid: root.svc + "/Settings/Result" }
	readonly property var wm: ({
		amps: { label: "alternator current", since: sinceAmps, hi: altAmpsHi, lo: altAmpsLo },
		Bcur: { label: "battery current", since: sinceBcur, hi: battAHi, lo: battALo },
		IBV: { label: "battery voltage", since: sinceIbv, hi: battVHi, lo: battVLo },
		SOC: { label: "state of charge", since: sinceSoc, hi: socHi, lo: socLo },
		altTempF: { label: "alternator temperature", since: sinceAltTemp, hi: altTempHi, lo: altTempLo },
		battTempF: { label: "battery temperature", since: sinceBattTemp, hi: battTempHi, lo: battTempLo },
		RPM: { label: "RPM", since: sinceRpm, hi: rpmHi, lo: rpmLo }
	})
	function wmTime(s) {
		const d = clock.date(s)
		const t = d.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
		return d.toDateString() === clock.date(nowS).toDateString() ? t : d.toLocaleDateString(Qt.locale(), "MMM d") + ", " + t
	}
	// "High and low since 2:14 PM.", naming any pair that started at another time (reset from the web app); pairs with
	// no reading (no shunt, no probe) count only when none has one, as right after a reset
	function wmNote(names) {
		if (!linkUp)
			return ""
		const has = n => wm[n].since.valid && (wm[n].hi.valid || wm[n].lo.valid)
		let use = names.filter(has)
		if (!use.length)
			use = names.filter(n => wm[n].since.valid)
		if (!use.length)
			return ""
		const byTime = {}
		const order = []
		for (const n of use) {
			const t = wmTime(wm[n].since.value)
			if (!byTime[t]) {
				byTime[t] = []
				order.push(t)
			}
			byTime[t].push(n)
		}
		const newest = t => Math.max.apply(null, byTime[t].map(n => wm[n].since.value))
		order.sort((a, b) => byTime[b].length - byTime[a].length || newest(b) - newest(a))
		const upper = t => t.charAt(0).toUpperCase() + t.slice(1)
		return (use.length === 1 ? "High and low since " : "Highs and lows since ") + order[0] + "."
			+ order.slice(1).map(t => " " + upper(byTime[t].map(n => wm[n].label).join(", ")) + " since " + t + ".").join("")
	}
	component TempItem: VeQuickItem {
		sourceUnit: Units.unitToVeUnit(VenusOS.Units_Temperature_Celsius)
		displayUnit: Units.unitToVeUnit(Global.systemSettings.temperatureUnit)
	}
	VeQuickItem { id: healthMean; uid: root.svc + "/Health/Live/sessionMean" }
	VeQuickItem { id: healthP10; uid: root.svc + "/Health/Live/sessionP10" }
	VeQuickItem { id: healthN; uid: root.svc + "/Health/Live/sessionN" }
	// the home page (XregHome), which shows a tile's larger view (XregPop)
	property Item home: null
	Component.onCompleted: {
		for (let p = parent; p; p = p.parent) {
			if (p.openPop !== undefined) {
				home = p
				break
			}
		}
	}
	VeQuickItem { id: fieldItem; uid: root.svc + "/Live/Field" }
	VeQuickItem { id: battVItem; uid: root.svc + "/Live/BattV" }
	VeQuickItem { id: battA; uid: root.svc + "/Live/BattA" }
	VeQuickItem { id: rpmItem; uid: root.svc + "/Live/Rpm" }
	VeQuickItem { id: ampTargetItem; uid: root.svc + "/Live/AmpTarget" }
	VeQuickItem { id: ignitionItem; uid: root.svc + "/Live/Ignition" }
	VeQuickItem { id: fieldStatusItem; uid: root.svc + "/Live/FieldStatus" }
	VeQuickItem { id: offReasonItem; uid: root.svc + "/Live/OffReason" }
	VeQuickItem { id: limiter; uid: root.svc + "/Live/Limiter" }
	VeQuickItem { id: stage; uid: root.svc + "/Live/Stage" }
	VeQuickItem { id: soc; uid: root.svc + "/Live/Soc" }
	VeQuickItem { id: health; uid: root.svc + "/Live/Health" }
	VeQuickItem { id: baro; uid: root.svc + "/Live/Baro" }
	VeQuickItem { id: appWind; uid: root.svc + "/Live/AppWind" }
	VeQuickItem { id: onOffItem; uid: root.svc + "/SwitchableOutput/0/State" }
	VeQuickItem {
		id: altTempItem
		uid: root.svc + "/Live/AltTemp"
		sourceUnit: Units.unitToVeUnit(VenusOS.Units_Temperature_Celsius)
		displayUnit: Units.unitToVeUnit(Global.systemSettings.temperatureUnit)
	}

	// The CSV1 readouts, twice a second as the web app's gauges: the bridge's fast lane while it sends one (/Fast/Live,
	// state changes as frames arrive, while this page asks, /Ui/Want/Live), else the 500 ms /Live paths. Alt and Batt amps stay
	// on /Live: the power flow's own readings, so a chip and the tile never disagree.
	VeQuickItem { id: wantLive; uid: root.svc + "/Ui/Want/Live" }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: root.visible && !ScreenBlanker.blanked
		onTriggered: wantLive.setValue(Math.floor(Date.now() / 1000))
	}
	VeQuickItem { id: fastLive; uid: root.svc + "/Fast/Live" }
	readonly property var fast: {
		if (!fastLive.valid || !fastLive.value)
			return null
		try {
			return JSON.parse(fastLive.value)
		} catch (e) {
			return null
		}
	}
	// a readout as its VeQuickItem has it (valid, value), from the fast lane (fast) when that carries `name`
	component Live: QtObject {
		property var fast
		property string name
		property QtObject item
		property var conv: null
		readonly property bool fromFast: !!fast && fast[name] !== undefined
		readonly property bool valid: fromFast ? fast[name] !== null : item.valid
		readonly property var value: !fromFast ? item.value : fast[name] === null ? undefined
			: conv ? conv(fast[name]) : fast[name]
	}
	Live { id: field; fast: root.fast; name: "Field"; item: fieldItem }
	Live { id: battV; fast: root.fast; name: "BattV"; item: battVItem }
	Live { id: rpm; fast: root.fast; name: "Rpm"; item: rpmItem }
	Live { id: ampTarget; fast: root.fast; name: "AmpTarget"; item: ampTargetItem }
	Live { id: ignition; fast: root.fast; name: "Ignition"; item: ignitionItem }
	Live { id: fieldStatus; fast: root.fast; name: "FieldStatus"; item: fieldStatusItem }
	Live { id: offReason; fast: root.fast; name: "OffReason"; item: offReasonItem }
	Live { id: onOff; fast: root.fast; name: "OnOff"; item: onOffItem }
	Live {
		id: altTemp
		fast: root.fast
		name: "AltTemp"
		item: altTempItem
		conv: c => Units.convert(c, VenusOS.Units_Temperature_Celsius, Global.systemSettings.temperatureUnit)
	}
	VeQuickItem {
		id: battTemp
		uid: root.svc + "/Live/BattTemp"
		sourceUnit: Units.unitToVeUnit(VenusOS.Units_Temperature_Celsius)
		displayUnit: Units.unitToVeUnit(Global.systemSettings.temperatureUnit)
	}
	// the wind hour (/Plot/Wind, the Dashboard's /Ui/Want/Live keeps it coming) for the wind tile and its larger view
	XregWindStore { id: windStore; svc: root.svc; active: root.visible }
	VeQuickItem { id: histBaro; uid: root.svc + "/History/Baro" }
	VeQuickItem { id: histBaroEnd; uid: root.svc + "/History/BaroEnd" }
	VeQuickItem { id: dvccState; uid: root.svc + "/Live/DvccState" }
	VeQuickItem { id: dvccUntrust; uid: root.svc + "/Live/DvccUntrust" }
	VeQuickItem { id: dvccCvl; uid: root.svc + "/Live/DvccCvl" }
	VeQuickItem { id: dvccCcl; uid: root.svc + "/Live/DvccCcl" }
	VeQuickItem { id: dvccCanAge; uid: root.svc + "/Live/DvccCanAge" }
	VeQuickItem { id: dvccWifiAge; uid: root.svc + "/Live/DvccWifiAge" }
	VeQuickItem { id: dvccStop; uid: root.svc + "/Live/DvccStop" }

	function num(item) { return root.linkUp && item.valid ? item.value : NaN }
	// Fixed decimals per quantity and a true minus sign (a digit's width; a hyphen is half that)
	function fmt(v, decimals, unit) {
		return isNaN(v) ? "--" : (v < 0 && Math.abs(v).toFixed(decimals) !== (0).toFixed(decimals) ? "\u2212" : "")
			+ Math.abs(v).toFixed(decimals) + (unit ? " " + unit : "")
	}
	// A number padded with figure spaces (a digit's width) to `chars`, for numbers inside a sentence or a centred line,
	// so the words after it stay put as the digit count changes
	function pad(text, chars) {
		let t = String(text)
		while (t.length < chars)
			t = "\u2007" + t
		return t
	}

	readonly property var faultReasons: [1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 15, 16, 17, 19, 22, 23, 24, 27, 28, 29, 31, 32]
	readonly property var idleReasons: [10, 20, 21, 25, 30]
	readonly property string statusWord: {
		if (!linkUp)
			return connected.valid ? "NO DATA" : "NO BRIDGE"
		switch (fieldStatus.value) {
		case 1: return "ACTIVE"
		case 2: return "RAMP DOWN"
		case 3: return "MANUAL"
		case 4: return "WAITING CLOUD"
		}
		if (faultReasons.indexOf(offReason.value) >= 0)
			return "FAULT"
		if (idleReasons.indexOf(offReason.value) >= 0 && onOff.value === 1)
			return "IDLE"
		return "OFF"
	}
	readonly property color statusColor: statusWord === "ACTIVE" ? Theme.color_green
		: statusWord === "MANUAL" ? Theme.color_ok
		: statusWord === "RAMP DOWN" || statusWord === "WAITING CLOUD" ? Theme.color_orange
		: statusWord === "FAULT" || !linkUp ? Theme.color_red
		: Theme.color_font_primary
	readonly property string stageWord: !linkUp || !stage.valid ? ""
		: (["", "BULK", "ABSORPTION", "FLOAT", "", "MAINTAIN", "TARGET V", "IDLE", "COMMISSIONING"][stage.value] || "")
	readonly property string limiterWord: !linkUp || !limiter.valid || fieldStatus.value !== 1 ? ""
		: (["", "ALT CURRENT CAP", "THERMAL DERATE", "VOLTAGE TARGET", "BATT CURRENT LIMIT", "MAX FIELD", "PROTECTION",
			"BATT > TARGET", "EXT LIMIT", "EXT LIMIT", "", "MIN FIELD", "ZERO CURRENT", "WARM-UP", "RATE GLIDE",
			"10 A FIELD", "DRIVE HEAT"][limiter.value] || "")
	readonly property string offText: {
		if (!linkUp || fieldStatus.value !== 0)
			return ""
		const r = offReason.value
		if (r === 10) {
			if (ignition.value === 0) return "ignition off"
			if (onOff.value === 0) return "alternator switched off"
			if (stage.value === 7) return "charge cycle complete, resting"
			return "charging disabled"
		}
		return ({
			1: "temperature sensor stale", 2: "over temperature (critical)", 3: "over temperature",
			4: "sustained over temperature", 5: "voltage reading implausible", 6: "voltage sensors disagree",
			7: "voltage spike", 8: "voltage sensors disagree", 9: "protection lockout active",
			12: "overvoltage, hardware cutoff", 13: "over-current cutoff", 14: "engine not running / RPM too low",
			15: "current sensor stale", 16: "overvoltage lockout", 17: "too cold to charge",
			18: "commissioning, field resting", 19: "field on but no alternator output",
			20: "resting, solar forecast is strong (Defer to Solar)",
			21: "BMS on/off signal is withholding permission to charge",
			22: "sustained overvoltage, timed cut (low tier)", 23: "sustained overvoltage, timed cut (mid tier)",
			24: "too hot to charge", 25: "charge cycle complete, battery full, resting",
			26: "measuring the current sensor zero", 27: "battery temperature sensor not responding",
			28: "main battery voltage sensor not responding", 29: "second battery voltage sensor not responding",
			30: "waiting for the first battery temperature reading", 31: "two or more sensors failed",
			32: "regulator temperature sensor not responding", 33: "field resistance test running (engine off)"
		})[r] || ""
	}

	// External charge-limit follow, the app's words: dvccState 0 off .. 5 untrusted; a carrier counts as heard within
	// 20 s, as on the app's Victron card. Shown in the strip when no off reason needs it.
	readonly property string limitsText: {
		if (!linkUp || !dvccState.valid)
			return ""
		const heard = []
		if (dvccCanAge.valid && dvccCanAge.value <= 20000)
			heard.push("VE.Can")
		if (dvccWifiAge.valid && dvccWifiAge.value <= 20000)
			heard.push("WiFi")
		const over = heard.length ? " over " + heard.join(" and ") : ""
		const lim = [dvccCvl.valid ? pad(dvccCvl.value.toFixed(2), 5) + " V" : "",
					 dvccCcl.valid ? pad(dvccCcl.value.toFixed(1), 6) + " A" : ""].filter(t => t.length).join(", ")
		// Stop Charging Without Limits holds charging at zero in every state but following
		const stopped = dvccStop.valid && dvccStop.value === 1 ? ", charging stopped" : ""
		switch (dvccState.value) {
		case 0: return heard.length ? "Charge limits heard" + over + ", not followed" + (lim ? ": " + lim : "") : ""
		case 1: return "Charge limits: waiting, none heard" + stopped
		case 2: return "Charge limits: settling" + (lim ? ", " + lim : "") + over + stopped
		case 3: return "Charge limits: following " + (lim || "no values") + over
		case 4: return "Charge limits: stale, the sender went silent" + stopped
		case 5: return "Charge limits: untrusted" + (["", ", voltage limit out of range", ", current limit implausible",
													  ", values flapping"][dvccUntrust.value] || "") + stopped
		}
		return ""
	}
	readonly property color limitsColor: !dvccState.valid ? Theme.color_font_secondary
		: dvccState.value === 3 ? Theme.color_green
		: dvccState.value >= 4 || (dvccStop.valid && dvccStop.value === 1) ? Theme.color_orange : Theme.color_font_secondary

	// "a,b,,c" -> numbers, empty entries NaN
	function series(str) {
		return str ? String(str).split(",").map(s => s.length ? parseFloat(s) : NaN) : []
	}
	// Samples -> polyline points across a window of n slots, newest at the right edge, lo..hi inset px inside the top and
	// bottom; NaN slots are skipped
	function points(v, n, lo, hi, w, h, inset) {
		if (!(hi > lo) || v.length === 0)
			return []
		const step = w / (n - 1)
		const x0 = w - (v.length - 1) * step
		let pts = []
		for (let i = 0; i < v.length; ++i) {
			if (isNaN(v[i]))
				continue
			const f = Math.max(lo, Math.min(hi, v[i]))
			pts.push(Qt.point(x0 + i * step, inset + (h - 2 * inset) * (1 - (f - lo) / (hi - lo))))
		}
		return pts
	}
	// A speed axis fitted as XregChart fits one from 0: at least 10 kn, 10% above the fastest, up to a round step
	function speedAxis(vals) {
		let hi = Math.max(10, vals.length ? Math.max.apply(null, vals) : 0)
		hi *= 1.1
		const raw = hi / 4, p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p
		const st = (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 && p >= 10 ? 2.5 : f <= 5 ? 5 : 10) * p
		return [0, Math.ceil(hi / st - 1e-9) * st]
	}
	function validOf(v) { return v.filter(x => !isNaN(x)) }

	readonly property XregClock clock: XregClock {}
	readonly property real nowS: clock.nowS
	readonly property int tzS: clock.tzS
	// Barometer samples (the regulator's 10 min apart, the newest at epoch end) that fall on local day `day`: x in hours
	// after its midnight, v in mbar (NaN: no sample)
	function dayLine(v, end, day) {
		let x = [], y = []
		for (let i = 0; i < v.length; ++i) {
			const t = end - (v.length - 1 - i) * 600 + tzS
			if (Math.floor(t / 86400) !== day)
				continue
			x.push((t - day * 86400) / 3600)
			y.push(v[i])
		}
		return { x: x, v: y }
	}
	// a day's line on the tile's midnight-to-midnight plot, w by h px, as one run of points per unbroken stretch: a NaN
	// sample (none taken) ends the run, so no line is drawn across the time the regulator was off
	function linePoints(l, lo, hi, w, h) {
		if (!(hi > lo))
			return []
		let runs = [], run = []
		for (let i = 0; i < l.v.length; ++i) {
			if (isNaN(l.v[i])) {
				if (run.length)
					runs.push(run)
				run = []
				continue
			}
			const f = Math.max(lo, Math.min(hi, l.v[i]))
			run.push(Qt.point(l.x[i] / 24 * w, h - (f - lo) / (hi - lo) * h))
		}
		if (run.length)
			runs.push(run)
		return runs
	}
	// The barometer tile's larger view: the three days on one chart, with Autoscale, Lock axes and axis limits kept on
	// this GX; the tile follows what is set there (its axis, the days hidden)
	readonly property XregView baroView: XregView { svc: root.svc; name: "dashBaro"; rightDefault: null }
	readonly property var baroKeys: ["today", "yesterday", "dayBefore"]
	readonly property var baroColors: [Theme.color_blue, Theme.color_orange, Theme.color_blackWater]
	readonly property var baroV: series(histBaro.value)
	readonly property real baroEnd: histBaroEnd.valid && histBaroEnd.value > 0 ? histBaroEnd.value : nowS
	readonly property int baroToday: Math.floor((nowS + tzS) / 86400)
	readonly property var baroDays: [0, 1, 2].map(k => dayLine(baroV, baroEnd, baroToday - k))
	function baroHidden(k) { return baroView.hidden[baroKeys[k]] === true }
	Component {
		id: baroPop
		XregChartPop {
			view: root.baroView
			chart: bigBaro
			title: "Barometer, last three days"
			leftName: "Pressure"
			leftUnit: "mbar"
			XregChart {
				id: bigBaro
				anchors.fill: parent
				title: "Barometer"
				lines: [0, 1, 2].map(k => ({ key: root.baroKeys[k], label: ["Today", "Yesterday", "2 days ago"][k], unit: "mbar",
					dec: 1, color: root.baroColors[k], w: k === 0 ? 3 : 2, x: root.baroDays[k].x, v: root.baroDays[k].v }))
				xLo: 0
				xHi: 24
				xTicks: [0, 6, 12, 18, 24].map(h => ({ x: h, t: ("0" + h).slice(-2) + ":00" }))
				view: root.baroView
				leftUnit: "mbar"
				leftMinSpan: 10
				leftFixed: root.baroView.leftFixed
				emptyText: "No barometer data"
				pad: 0.1
				inset: 3
				fixedEnds: true
				editAxes: true
			}
		}
	}

	// The wind tile's larger view: true and apparent wind speed over the last hour, every reading the GX receives
	// (XregChart's streamed lines, stepping once per reading), with Autoscale, Lock axes and axis limits kept on this GX.
	// The tile draws apparent wind, and true wind once it is shown in the larger view; it follows the view's axis and the
	// lines hidden there.
	readonly property XregView windView: XregView { svc: root.svc; name: "dashWind" }
	readonly property var windLines: [
		{ key: "true", sk: "Tws", label: "True", unit: "kn", dec: 1, color: Theme.color_blue },
		{ key: "app", sk: "Aws", label: "Apparent", unit: "kn", dec: 1, color: brand, w: 2.5 }
	]
	// The tile's lines: the hour in columns windColMs wide, fixed in time, each the mean of its readings at their mean
	// time (s before the newest reading), so a reading changes only the newest column and the line slides left as the hour
	// moves on. Only the samples not yet taken are read (windAgg.a, changed in place, so no binding follows it); worked
	// out while the Dashboard is shown.
	readonly property int windColMs: 15000
	readonly property var windAgg: ({ a: null })
	readonly property var windCols: windStore.ver >= 0 ? windColsNow() : null
	function windColsNow() {
		const s = windStore.store, n = s.t.length
		let a = windAgg.a
		if (!a || a.reset !== s.reset || a.i < s.off + s.h)
			a = windAgg.a = { reset: s.reset, i: s.off + s.h, c0: 0, cols: [] }
		const sum = (acc, v, t) => {
			if (isFinite(v)) {
				acc[0] += v
				acc[1] += t
				acc[2]++
			}
		}
		for (; a.i < s.off + n; ++a.i) {
			const k = a.i - s.off
			if (s.b[k])
				continue
			const t = s.t[k], c = Math.floor(t / windColMs)
			if (!a.cols.length)
				a.c0 = c
			while (a.c0 + a.cols.length <= c)
				a.cols.push({ app: [0, 0, 0], tru: [0, 0, 0] })
			const col = a.cols[c - a.c0]
			sum(col.app, s.v.Aws[k], t)
			sum(col.tru, s.v.Tws[k], t)
		}
		const out = { app: { x: [], v: [] }, tru: { x: [], v: [] } }
		if (n <= s.h)
			return out
		const tN = s.t[n - 1], cut = Math.floor((tN - 3600000) / windColMs)
		while (a.cols.length && a.c0 < cut) {
			a.cols.shift()
			a.c0++
		}
		for (const col of a.cols) {
			for (const key of ["app", "tru"]) {
				const acc = col[key]
				if (acc[2] && acc[1] / acc[2] >= tN - 3600000) {
					out[key].x.push((acc[1] / acc[2] - tN) / 1000)
					out[key].v.push(acc[0] / acc[2])
				}
			}
		}
		return out
	}
	// Columns -> polyline points over the last hour, the newest reading at the right edge, lo..hi inset px inside the top
	// and bottom
	function hourPoints(c, lo, hi, w, h, inset) {
		if (!(hi > lo) || !c)
			return []
		let pts = []
		for (let i = 0; i < c.v.length; ++i) {
			const f = Math.max(lo, Math.min(hi, c.v[i]))
			pts.push(Qt.point(w * (1 + c.x[i] / 3600), inset + (h - 2 * inset) * (1 - (f - lo) / (hi - lo))))
		}
		return pts
	}
	Component {
		id: windPop
		XregChartPop {
			view: root.windView
			chart: bigWind
			title: "Wind speed, last hour"
			leftName: "Speed"
			leftUnit: "kn"
			XregChart {
				id: bigWind
				anchors.fill: parent
				title: "Wind"
				lines: root.windLines
				stream: windStore.store
				streamVer: windStore.ver
				smooth: false
				xLo: -3600
				xHi: 0
				xTicks: [50, 40, 30, 20, 10, 0].map(mm => ({ x: -mm * 60, t: mm === 0 ? "now" : mm + " min" }))
				view: root.windView
				leftFromZero: true
				leftUnit: "kn"
				leftMinSpan: 10
				leftFixed: root.windView.leftFixed
				emptyText: "No wind data"
				pad: 0.1
				inset: 3
				fixedEnds: true
				editAxes: true
			}
		}
	}

	// The power flow's larger view: the tile in its full layout (two-line rows with full names, sources and loads
	// labelled, the footnote and the scale line), as tall as its rows need, scaled up to 1.5 times where the screen has
	// room; on a screen too low for that at full size, the tile's denser layouts. It has no settings of its own.
	Component {
		id: flowPop
		Item {
			id: fp
			readonly property bool fits: true
			readonly property real availW: (root.home ? root.home.width : root.width) * (root.portrait ? 0.94 : 0.8) - 28
			readonly property real availH: (root.home ? root.home.height : root.height) - 16 - 28 - flowHead.height - 10
			readonly property real baseW: Math.min(340, availW)
			readonly property real zoom: Math.max(1, Math.min(1.5, availW / baseW, availH / bigFlow.growH))
			implicitWidth: Math.ceil(baseW * zoom)
			implicitHeight: flowHead.height + 10 + Math.ceil(bigFlow.height * zoom)
			Label {
				id: flowHead
				text: "Power flow"
				color: Theme.color_font_primary
				font.pixelSize: Theme.font_size_body2
			}
			XregFlow {
				id: bigFlow
				y: flowHead.height + 10
				width: fp.baseW
				height: Math.min(growH, fp.availH)
				grow: growH <= fp.availH
				transformOrigin: Item.TopLeft
				scale: fp.zoom
				svc: root.svc
				linkUp: root.linkUp
				active: root.statusWord === "ACTIVE"
				target: root.num(ampTarget)
				animationEnabled: root.animationEnabled
			}
		}
	}

	// The speed-performance tile's larger view: the tile again, its type up to 1.6 times as big, mirroring the small one
	// (XregChartPerf mirror), with Autoscale, Lock axes and the speed and RPM maximums kept on this GX (the web app's
	// plots have none); the small tile follows them
	readonly property XregView perfView: XregView { svc: root.svc; name: "dashPerf"; rightDefault: null }
	Component {
		id: perfPop
		XregChartPop {
			view: root.perfView
			chart: bigPerf
			title: "Boat speed performance"
			leftName: "Speed"
			leftUnit: "kn"
			rightName: "RPM"
			rightUnit: ""
			hint: "Tap the top number of an axis to set its maximum."
			XregChartPerf {
				id: bigPerf
				anchors.fill: parent
				svc: root.svc
				linkUp: root.linkUp
				mirror: perfTile
				axes: root.perfView
				editAxes: true
				maxK: 1.6
			}
		}
	}

	component Chip: Rectangle {
		property real sc: 1
		property string label1
		property string value1
		property color color1: Theme.color_font_primary
		property string label2
		property string value2
		property color color2: Theme.color_font_primary
		property string template1
		property string template2
		// opens: a tap opens the tile's larger view (a drag still scrolls the page)
		property bool opens
		signal opened()
		radius: Theme.geometry_overviewPage_widget_radius
		color: Theme.color_background_secondary
		TapHandler {
			enabled: parent.opens
			onTapped: parent.opened()
		}
		Column {
			anchors { fill: parent; leftMargin: Math.round(12 * sc); rightMargin: Math.round(12 * sc) }
			topPadding: label2.length ? 8 : (parent.height - 30) / 2
			spacing: 4
			Repeater {
				model: label2.length ? 2 : 1
				Item {
					width: parent.width
					height: 26
					Label {
						anchors { left: parent.left; verticalCenter: parent.verticalCenter }
						text: index === 0 ? label1 : label2
						color: Theme.color_font_secondary
						font.pixelSize: Math.round(Theme.font_size_body1 * sc)
					}
					XregNum {
						anchors { right: parent.right; verticalCenter: parent.verticalCenter }
						template: index === 0 ? template1 : template2
						text: index === 0 ? value1 : value2
						color: index === 0 ? color1 : color2
						font.pixelSize: Math.round(Theme.font_size_body2 * sc)
						font.bold: true
					}
				}
			}
		}
	}

	component Pill: Rectangle {
		property alias text: pillLabel.text
		property color tint: Theme.color_font_secondary
		visible: text.length > 0
		height: 32
		width: pillLabel.implicitWidth + 24
		radius: Theme.geometry_iochannel_status_radius
		color: Qt.rgba(tint.r, tint.g, tint.b, 0.2)
		Label {
			id: pillLabel
			anchors.centerIn: parent
			color: parent.tint
			font.pixelSize: Theme.font_size_body1
			font.bold: true
		}
	}

	component ChartTile: Rectangle {
		property string title
		property string compactTitle
		readonly property bool compact: width < 250
		property alias note: chartNote.text
		property alias empty: emptyLabel.text
		property bool hasData: true
		property real footH: 0
		default property alias content: plotArea.data
		radius: Theme.geometry_overviewPage_widget_radius
		color: Theme.color_background_secondary
		Label {
			x: 14; y: 8
			text: parent.compact && parent.compactTitle.length ? parent.compactTitle : parent.title
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_caption
		}
		XregNum {
			id: chartNote
			anchors { right: parent.right; rightMargin: 14 }
			y: parent.compact ? 8 : 6
			font.pixelSize: parent.compact ? Theme.font_size_caption : Theme.font_size_body1
			font.bold: true
		}
		Item {
			id: plotArea
			anchors { fill: parent; leftMargin: 14; rightMargin: 14; topMargin: 34; bottomMargin: 10 + parent.footH }
			visible: parent.hasData
		}
		Label {
			id: emptyLabel
			anchors.centerIn: parent
			anchors.verticalCenterOffset: 10
			visible: !parent.hasData
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_body1
		}
	}

	// A chip's larger view (XregPop, sized to it): each reading large, under it its highs and lows since the regulator
	// started where the regulator keeps them. cols: [{ name, value, template, rows: [[label, text, template]] }]; side by
	// side, stacked on a phone held upright.
	component ReadoutPanel: Column {
		id: rp
		readonly property bool fits: true
		property string title
		property var cols: []
		// the regulator's names for the highs and lows shown (Reset clears these); none, no Reset
		property var wmNames: []
		property string note: root.wmNote(wmNames)
		spacing: 16
		Item {
			width: Math.max(colsGrid.width, titleLabel.implicitWidth + (resetLink.visible ? resetLink.implicitWidth + 24 : 0))
			height: Math.max(titleLabel.implicitHeight, resetLink.visible ? resetLink.implicitHeight : 0)
			Label {
				id: titleLabel
				anchors.verticalCenter: parent.verticalCenter
				text: rp.title
				color: Theme.color_font_primary
				font.pixelSize: Theme.font_size_body2
			}
			// small and out of the way; a second tap within 3 s resets, so a stray tap does nothing
			Label {
				id: resetLink
				property int stage: 0   // 0 Reset, 1 waiting for the second tap, 2 sent, 3 refused
				anchors { right: parent.right; verticalCenter: parent.verticalCenter }
				// firmware before the reset command sends no start times
				visible: root.linkUp && rp.wmNames.some(n => root.wm[n].since.valid)
				text: ["Reset", "Tap again to reset", "Reset", "Could not reset"][stage]
				color: stage === 1 ? Theme.color_font_primary : Theme.color_font_secondary
				font.pixelSize: Theme.font_size_body1
				Timer {
					id: resetTimer
					interval: resetLink.stage === 2 ? 15000 : 3000   // sent: the answer's wait
					onTriggered: resetLink.stage = 0
				}
				Connections {
					target: settingsResult
					enabled: resetLink.stage === 2
					function onValueChanged() {
						let r = null
						try {
							r = JSON.parse(settingsResult.value || "")
						} catch (e) {
							return
						}
						if (!r || r.key !== "wmReset")
							return
						resetTimer.stop()
						resetLink.stage = r.ok ? 0 : 3
						if (!r.ok)
							resetTimer.start()
					}
				}
				MouseArea {
					anchors { fill: parent; margins: -12 }
					onClicked: {
						if (resetLink.stage === 1) {
							resetLink.stage = 2
							resetTimer.restart()
							wmReset.setValue(rp.wmNames.join(","))
						} else if (resetLink.stage !== 2) {
							resetLink.stage = 1
							resetTimer.restart()
						}
					}
				}
			}
		}
		Grid {
			id: colsGrid
			columns: root.portrait ? 1 : Math.max(1, rp.cols.length)
			columnSpacing: 40
			rowSpacing: 16
			Repeater {
				model: rp.cols
				delegate: Column {
					required property var modelData
					spacing: 4
					Label {
						text: modelData.name
						color: Theme.color_font_secondary
						font.pixelSize: Theme.font_size_body2
					}
					XregNum {
						template: modelData.template
						text: modelData.value
						font.pixelSize: Theme.font_size_h4
						font.bold: true
					}
					// label and value cells, so the values line up
					Grid {
						columns: 2
						columnSpacing: 12
						rowSpacing: 4
						verticalItemAlignment: Grid.AlignVCenter
						Repeater {
							model: (modelData.rows || []).reduce((a, r) => a.concat([{ s: r[0], t: "" }, { s: r[1], t: r[2], v: true }]), [])
							delegate: XregNum {
								required property var modelData
								template: modelData.t
								text: modelData.s
								color: modelData.v ? Theme.color_font_primary : Theme.color_font_secondary
								font.pixelSize: modelData.v ? Theme.font_size_body3 : Theme.font_size_body2
								font.bold: !!modelData.v
							}
						}
					}
				}
			}
		}
		Label {
			width: colsGrid.width
			visible: text.length > 0
			text: rp.note
			wrapMode: Text.Wrap
			color: Theme.color_font_secondary
			font.pixelSize: Theme.font_size_tiny
		}
	}
	function hiLo(hi, lo, decimals, unit, template) {
		return [["High", fmt(num(hi), decimals, unit), template], ["Low", fmt(num(lo), decimals, unit), template]]
	}
	readonly property string tempUnit: Global.systemSettings.temperatureUnitSuffix
	Component {
		id: battPop
		ReadoutPanel {
			title: "Battery"
			wmNames: ["IBV", "SOC"]
			cols: [
				{ name: "Voltage", value: root.fmt(root.num(battV), 2, "V"), template: "88.88 V",
				  rows: root.hiLo(battVHi, battVLo, 2, "V", "88.88 V") },
				{ name: "State of charge", value: root.fmt(root.num(soc), 1, "%"), template: "100.0 %",
				  rows: root.hiLo(socHi, socLo, 1, "%", "100.0 %") }
			]
		}
	}
	Component {
		id: ampsPop
		ReadoutPanel {
			title: "Alternator and Battery Current"
			wmNames: ["amps", "Bcur"]
			cols: [
				{ name: "Alternator", value: root.fmt(root.num(altAmps), 1, "A"), template: "\u2212888.8 A",
				  rows: root.hiLo(altAmpsHi, altAmpsLo, 1, "A", "\u2212888.8 A") },
				{ name: "Battery", value: root.fmt(root.num(battA), 1, "A"), template: "\u2212888.8 A",
				  rows: root.hiLo(battAHi, battALo, 1, "A", "\u2212888.8 A") }
			]
		}
	}
	Component {
		id: tempPop
		ReadoutPanel {
			title: "Temperatures"
			readonly property var alt: ({ name: "Alternator", value: root.fmt(root.num(altTemp), 1, root.tempUnit),
				template: "\u2212888.8 " + root.tempUnit, rows: root.hiLo(altTempHi, altTempLo, 1, root.tempUnit, "\u2212888.8 " + root.tempUnit) })
			readonly property var batt: ({ name: "Battery", value: root.fmt(root.num(battTemp), 1, root.tempUnit),
				template: "\u2212888.8 " + root.tempUnit, rows: root.hiLo(battTempHi, battTempLo, 1, root.tempUnit, "\u2212888.8 " + root.tempUnit) })
			cols: isNaN(root.num(battTemp)) ? [alt] : [alt, batt]
			wmNames: isNaN(root.num(battTemp)) ? ["altTempF"] : ["altTempF", "battTempF"]
		}
	}
	Component {
		id: enginePop
		ReadoutPanel {
			title: "Engine"
			wmNames: ["RPM"]
			cols: [
				{ name: "RPM", value: root.fmt(root.num(rpm), 0, ""), template: "88888", rows: root.hiLo(rpmHi, rpmLo, 0, "", "88888") },
				{ name: "Ignition", value: !root.linkUp || !ignition.valid ? "--" : ignition.value === 1 ? "ON" : "OFF", template: "OFF" }
			]
		}
	}
	Component {
		id: healthPop
		ReadoutPanel {
			readonly property int n: Math.max(0, Math.round(root.num(healthN) || 0))
			title: "Alternator Health"
			cols: [{ name: "Health", value: isNaN(root.num(health)) ? "- %" : Math.round(root.num(health)) + " %", template: "100 %",
					 rows: n > 0 ? [["Mean", root.fmt(root.num(healthMean), 0, "%"), "100 %"],
									["10th percentile", root.fmt(root.num(healthP10), 0, "%"), "100 %"]] : [] }]
			note: !healthN.valid || !root.linkUp ? "" : n > 0 ? n.toLocaleString(Qt.locale(), "f", 0) + " graded point"
				+ (n === 1 ? "" : "s") + " this session" : "No graded points this session"
		}
	}

	Flickable {
		anchors.fill: parent
		contentHeight: root.portrait ? root.sideY + root.sideH + root.m : height
		interactive: root.portrait
		clip: root.portrait

		// Banner row: connection dot + wordmark, status words on the right (their own line in portrait)
		Row {
			x: root.m; y: root.topY
			height: root.bannerH
			spacing: 10
			Rectangle {
				anchors.verticalCenter: parent.verticalCenter
				width: 14; height: 14; radius: 7
				color: root.linkUp ? root.brand : Theme.color_red
			}
			Row {
				anchors.verticalCenter: parent.verticalCenter
				Label {
					text: "X"
					color: root.brand
					font.pixelSize: 32
					font.weight: Font.Black
				}
				Label {
					text: " Engineering"
					font.pixelSize: 32
					font.weight: Font.Light
				}
			}
		}
		Label {
			id: statusLabel
			x: root.portrait ? root.m : fieldPct.visible ? fieldPct.x - 14 - width : root.width - root.m - width
			y: root.portrait ? 50 : root.topY
			height: root.bannerH
			verticalAlignment: Text.AlignVCenter
			text: root.statusWord
			color: root.statusColor
			font.pixelSize: Theme.font_size_h1
			font.bold: true
		}
		XregNum {
			id: fieldPct
			visible: root.statusWord === "ACTIVE" && field.valid
			x: root.portrait ? statusLabel.x + statusLabel.width + 14 : root.width - root.m - width
			y: statusLabel.y
			height: root.bannerH
			verticalAlignment: Text.AlignVCenter
			template: "100%"
			text: Math.round(field.value) + "%"
			color: root.statusColor
			font.pixelSize: Theme.font_size_h1
			font.bold: true
		}
		Row {
			x: root.portrait ? root.m : statusLabel.x - 10 - width
			y: root.portrait ? 96 : root.topY
			height: root.portrait ? 36 : root.bannerH
			spacing: 10
			Pill { anchors.verticalCenter: parent.verticalCenter; text: root.limiterWord; tint: Theme.color_orange }
			Pill { anchors.verticalCenter: parent.verticalCenter; text: root.stageWord; tint: Theme.color_ok }
		}

		// Off-reason strip, else the charge-limit follow line; the space is always reserved so nothing below moves
		Rectangle {
			readonly property color tint: root.offText.length ? Theme.color_orange : root.limitsColor
			x: root.m; y: root.stripY
			width: root.width - 2 * root.m
			height: 28
			radius: 6
			color: offLabel.text.length ? Qt.rgba(tint.r, tint.g, tint.b, 0.16) : "transparent"
			Label {
				id: offLabel
				anchors.centerIn: parent
				width: Math.min(implicitWidth, parent.width - 16)
				elide: Text.ElideRight
				text: root.offText || root.limitsText
				color: parent.tint
				font.pixelSize: Theme.font_size_body1
				font.bold: true
			}
		}

		// Chips, as in the app header (Alternator Enable is in the header above the page, XregHeader)
		Grid {
			id: chips
			x: root.m; y: root.chipsY
			columns: root.portrait ? 2 : 5
			spacing: root.gap
			readonly property real chipW: root.chipW

			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Batt"; value1: root.fmt(root.num(battV), 2, "V"); template1: "88.88 V"
				label2: "SOC"; value2: root.fmt(root.num(soc), 1, "%"); template2: "100.0 %"
				opens: !!root.home
				onOpened: root.home.openPop(battPop)
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Alt"; value1: root.fmt(root.num(altAmps), 1, "A"); template1: "\u2212888.8 A"
				label2: "Batt"; value2: root.fmt(root.num(battA), 1, "A"); template2: "\u2212888.8 A"
				opens: !!root.home
				onOpened: root.home.openPop(ampsPop)
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Alt"; value1: root.fmt(root.num(altTemp), 1, Global.systemSettings.temperatureUnitSuffix)
				template1: "\u221288.8 " + Global.systemSettings.temperatureUnitSuffix
				label2: isNaN(root.num(battTemp)) ? "" : "Batt"
				value2: root.fmt(root.num(battTemp), 1, Global.systemSettings.temperatureUnitSuffix)
				template2: template1
				opens: !!root.home
				onOpened: root.home.openPop(tempPop)
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "RPM"; value1: root.fmt(root.num(rpm), 0, ""); template1: "8888"
				label2: "IGN"
				value2: !root.linkUp || !ignition.valid ? "--" : ignition.value === 1 ? "ON" : "OFF"
				color2: ignition.value === 1 ? root.brand : Theme.color_font_primary
				opens: !!root.home
				onOpened: root.home.openPop(enginePop)
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Health"; value1: isNaN(root.num(health)) ? "- %" : Math.round(root.num(health)) + " %"; template1: "100 %"
				opens: !!root.home
				onOpened: root.home.openPop(healthPop)
			}
		}

		XregFlow {
			id: flowTile
			x: root.m
			y: root.lowerY
			width: root.colW
			height: root.flowH
			svc: root.svc
			linkUp: root.linkUp
			active: root.statusWord === "ACTIVE"
			target: root.num(ampTarget)
			compact: root.tight
			grow: root.portrait
			animationEnabled: root.animationEnabled
			shown: root.visible && !ScreenBlanker.blanked
			// a tap opens the larger view (a drag still scrolls the page)
			TapHandler {
				enabled: !!root.home
				onTapped: root.home.openPop(flowPop)
			}
		}

		XregChartPerf {
			id: perfTile
			x: root.perfX
			y: root.perfY
			width: root.perfW
			height: root.perfH
			svc: root.svc
			linkUp: root.linkUp
			active: root.visible && !ScreenBlanker.blanked
			timing: root.timing
			axes: root.perfView
			// a tap opens the larger view, except on the tile's own buttons and their touch margins (XregChartPerf); a drag
			// still scrolls the page
			TapHandler {
				enabled: !!root.home
				onTapped: root.home.openPop(perfPop)
			}
		}

		// Right column: wind, barometer
		Item {
			id: side
			x: root.sideX
			y: root.sideY
			width: root.sideW
			height: root.sideH
			readonly property real chartH: (height - root.gap) / 2

			ChartTile {
				id: windTile
				width: side.width; height: side.chartH
				title: "Wind, last hour"
				compactTitle: "Wind, 1 h"
				note: isNaN(root.num(appWind)) ? "" : root.num(appWind).toFixed(1) + (compact ? " kn" : " kn apparent")
				empty: "No wind data"
				// apparent wind, true wind once shown in the larger view; the axis and the lines as set there (windView)
				readonly property bool appOn: root.windView.hidden.app !== true
				readonly property bool trueOn: root.windView.hidden["true"] === false
				readonly property var v: root.windCols ? root.windCols.app.v : []
				readonly property var vt: root.windCols ? root.windCols.tru.v : []
				readonly property var axis: root.windView.leftFixed
					|| root.speedAxis([].concat(appOn ? v : [], trueOn ? vt : []))
				hasData: v.length > 1 || trueOn && vt.length > 1
				footH: trueOn ? 18 : 0
				// a tap opens the larger view (a drag still scrolls the page)
				TapHandler {
					enabled: !!root.home
					onTapped: root.home.openPop(windPop)
				}
				XregNum {
					anchors { left: parent.left; top: parent.top }
					template: "88 kn"
					text: Math.round(windTile.axis[1]) + " kn"
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_micro
				}
				XregNum {
					anchors { left: parent.left; bottom: parent.bottom }
					visible: windTile.axis[0] !== 0
					template: "88 kn"
					text: Math.round(windTile.axis[0]) + " kn"
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_micro
				}
				Shape {
					id: windPlot
					anchors.fill: parent
					preferredRendererType: Shape.GeometryRenderer
					// the axis's ends 3 px inside the plot, so a line on one is drawn whole
					readonly property var pts: windTile.appOn && root.windCols ? root.hourPoints(root.windCols.app, windTile.axis[0], windTile.axis[1], width, height, 3) : []
					readonly property var ptsTrue: windTile.trueOn && root.windCols ? root.hourPoints(root.windCols.tru, windTile.axis[0], windTile.axis[1], width, height, 3) : []
					ShapePath {
						strokeColor: "transparent"
						fillGradient: LinearGradient {
							y1: 0; y2: windPlot.height
							GradientStop { position: 0; color: Qt.rgba(root.brand.r, root.brand.g, root.brand.b, 0.35) }
							GradientStop { position: 1; color: Qt.rgba(root.brand.r, root.brand.g, root.brand.b, 0.0) }
						}
						PathPolyline {
							path: windPlot.pts.length > 1 ? windPlot.pts.concat([Qt.point(windPlot.pts[windPlot.pts.length - 1].x, windPlot.height),
								Qt.point(windPlot.pts[0].x, windPlot.height)]) : []
						}
					}
					ShapePath {
						strokeColor: Theme.color_blue
						strokeWidth: 2
						fillColor: "transparent"
						PathPolyline { path: windPlot.ptsTrue }
					}
					ShapePath {
						strokeColor: root.brand
						strokeWidth: 2.5
						fillColor: "transparent"
						PathPolyline { path: windPlot.pts }
					}
				}
				Row {
					anchors { left: parent.left; top: parent.bottom; topMargin: 4 }
					visible: windTile.trueOn
					spacing: 10
					Repeater {
						model: windTile.appOn ? [["Apparent", root.brand], ["True", Theme.color_blue]] : [["True", Theme.color_blue]]
						delegate: Row {
							required property var modelData
							spacing: 4
							Rectangle {
								anchors.verticalCenter: parent.verticalCenter
								width: 10; height: 3; radius: 1.5
								color: modelData[1]
							}
							Label { text: modelData[0]; color: Theme.color_font_secondary; font.pixelSize: Theme.font_size_micro }
						}
					}
				}
			}

			ChartTile {
				id: baroTile
				y: side.chartH + root.gap
				width: side.width; height: side.chartH
				title: "Barometer"
				note: isNaN(root.num(baro)) ? "" : root.num(baro).toFixed(1) + " mbar"
				empty: "No barometer data"
				// Three days of 10-minute samples, each day on the same midnight-to-midnight axis, so today's line ends at
				// the latest sample; the axis and the days shown as set in the larger view (baroView)
				readonly property var valid: root.validOf(root.baroV)
				readonly property var shownValid: root.validOf([].concat(root.baroHidden(0) ? [] : root.baroDays[0].v,
					root.baroHidden(1) ? [] : root.baroDays[1].v, root.baroHidden(2) ? [] : root.baroDays[2].v))
				readonly property var fixed: root.baroView.leftFixed
				readonly property real lo: fixed ? fixed[0] : shownValid.length
					? Math.min(Math.min.apply(null, shownValid), Math.max.apply(null, shownValid) - 9) - 0.5 : 0
				readonly property real hi: fixed ? fixed[1] : shownValid.length ? Math.max.apply(null, shownValid) + 0.5 : 1
				readonly property var days: [0, 1, 2].map(k => root.baroHidden(k) ? []
					: root.linePoints(root.baroDays[k], lo, hi, baroPlot.width, baroPlot.height))
				readonly property var colors: root.baroColors
				readonly property var lastRun: days[0].length ? days[0][days[0].length - 1] : []
				readonly property var nowPt: lastRun.length ? lastRun[lastRun.length - 1] : null
				readonly property real gutter: baroHi.width + 4   // lines start right of the scale; fixed, so the plot never shifts
				hasData: valid.length > 1
				footH: 18
				// a tap opens the larger view (a drag still scrolls the page)
				TapHandler {
					enabled: !!root.home
					onTapped: root.home.openPop(baroPop)
				}
				Row {
					anchors { left: parent.left; top: parent.bottom; topMargin: 4 }
					spacing: 10
					Repeater {
						model: baroTile.compact ? ["Today", "-1 d", "-2 d"] : ["Today", "Yesterday", "2 days ago"]
						delegate: Row {
							required property int index
							required property string modelData
							opacity: root.baroHidden(index) ? 0.35 : 1
							spacing: 4
							Rectangle {
								anchors.verticalCenter: parent.verticalCenter
								width: 10; height: 3; radius: 1.5
								color: baroTile.colors[index]
							}
							Label { text: modelData; color: Theme.color_font_secondary; font.pixelSize: Theme.font_size_micro }
						}
					}
				}
				Repeater {
					model: [6, 12, 18]
					delegate: Rectangle {
						required property int modelData
						x: baroPlot.x + Math.round(baroPlot.width * modelData / 24)
						width: 1; height: baroPlot.height
						color: Theme.color_card_separator
					}
				}
				XregNum {
					id: baroHi
					anchors { left: parent.left; top: parent.top }
					template: "8888"
					text: baroTile.hi.toFixed(0)
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_micro
				}
				XregNum {
					id: baroLo
					anchors { left: parent.left; bottom: parent.bottom }
					template: "8888"
					text: baroTile.lo.toFixed(0)
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_micro
				}
				Shape {
					id: baroPlot
					anchors { fill: parent; leftMargin: baroTile.gutter }
					preferredRendererType: Shape.GeometryRenderer
					ShapePath {
						strokeColor: baroTile.colors[2]
						strokeWidth: 2
						fillColor: "transparent"
						PathMultiline { paths: baroTile.days[2] }
					}
					ShapePath {
						strokeColor: baroTile.colors[1]
						strokeWidth: 2
						fillColor: "transparent"
						PathMultiline { paths: baroTile.days[1] }
					}
					ShapePath {
						strokeColor: baroTile.colors[0]
						strokeWidth: 3
						fillColor: "transparent"
						PathMultiline { paths: baroTile.days[0] }
					}
				}
				// Where today stops: the latest sample and its time of day
				Rectangle {
					visible: !!baroTile.nowPt
					x: baroPlot.x + (baroTile.nowPt ? baroTile.nowPt.x : 0) - width / 2
					y: (baroTile.nowPt ? baroTile.nowPt.y : 0) - height / 2
					width: 8; height: 8; radius: 4
					color: baroTile.colors[0]
				}
				Label {
					visible: !!baroTile.nowPt
					readonly property int mins: baroTile.nowPt ? Math.round(baroTile.nowPt.x / baroPlot.width * 1440) % 1440 : 0
					x: baroPlot.x + Math.max(0, Math.min(baroPlot.width - width, (baroTile.nowPt ? baroTile.nowPt.x : 0) - width / 2))
					y: !baroTile.nowPt ? 0 : baroTile.nowPt.y - height - 6 >= 0 ? baroTile.nowPt.y - height - 6 : baroTile.nowPt.y + 6
					text: ("0" + Math.floor(mins / 60)).slice(-2) + ":" + ("0" + mins % 60).slice(-2)
					color: baroTile.colors[0]
					font.pixelSize: Theme.font_size_micro
					font.bold: true
				}
			}
		}
	}
}
