import QtQuick
import QtQuick.Shapes
import Victron.VenusOS

// Dashboard, the first page of the home page (XregHome, under its header with Alternator Enable), for one regulator
// (svc: its service uid).
// Top: the web app's header banner (wordmark, status words, off-reason strip, chips).
// Below: power flow (XregFlow: the battery, then a row per source and load on one fixed scale), the app's boat
// speed-performance view (XregChartPerf), apparent wind for the last hour and barometer for today, yesterday and the day
// before on one midnight-to-midnight axis.
// Under 600 px wide (remote console on a phone held upright) the same pieces stack in one scrolling column, the power
// flow as tall as its rows.
// Reads the bridge's /Live and /History paths; temperatures arrive in Celsius and show in the owner's unit.
// Status, stage, limiter and off-reason words are the web app's own (script.js fieldOffReasonText etc.).
Item {
	id: root

	property bool animationEnabled
	property string svc
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
	VeQuickItem { id: field; uid: root.svc + "/Live/Field" }
	VeQuickItem { id: battV; uid: root.svc + "/Live/BattV" }
	VeQuickItem { id: battA; uid: root.svc + "/Live/BattA" }
	VeQuickItem { id: rpm; uid: root.svc + "/Live/Rpm" }
	VeQuickItem { id: ampTarget; uid: root.svc + "/Live/AmpTarget" }
	VeQuickItem { id: ignition; uid: root.svc + "/Live/Ignition" }
	VeQuickItem { id: fieldStatus; uid: root.svc + "/Live/FieldStatus" }
	VeQuickItem { id: offReason; uid: root.svc + "/Live/OffReason" }
	VeQuickItem { id: limiter; uid: root.svc + "/Live/Limiter" }
	VeQuickItem { id: stage; uid: root.svc + "/Live/Stage" }
	VeQuickItem { id: soc; uid: root.svc + "/Live/Soc" }
	VeQuickItem { id: health; uid: root.svc + "/Live/Health" }
	VeQuickItem { id: baro; uid: root.svc + "/Live/Baro" }
	VeQuickItem { id: appWind; uid: root.svc + "/Live/AppWind" }
	VeQuickItem { id: onOff; uid: root.svc + "/SwitchableOutput/0/State" }
	VeQuickItem {
		id: altTemp
		uid: root.svc + "/Live/AltTemp"
		sourceUnit: Units.unitToVeUnit(VenusOS.Units_Temperature_Celsius)
		displayUnit: Units.unitToVeUnit(Global.systemSettings.temperatureUnit)
	}
	VeQuickItem {
		id: battTemp
		uid: root.svc + "/Live/BattTemp"
		sourceUnit: Units.unitToVeUnit(VenusOS.Units_Temperature_Celsius)
		displayUnit: Units.unitToVeUnit(Global.systemSettings.temperatureUnit)
	}
	VeQuickItem { id: histWind; uid: root.svc + "/History/AppWind" }
	VeQuickItem { id: histBaro; uid: root.svc + "/History/Baro" }
	VeQuickItem { id: histBaroEnd; uid: root.svc + "/History/BaroEnd" }
	VeQuickItem { id: dvccState; uid: root.svc + "/Live/DvccState" }
	VeQuickItem { id: dvccUntrust; uid: root.svc + "/Live/DvccUntrust" }
	VeQuickItem { id: dvccCvl; uid: root.svc + "/Live/DvccCvl" }
	VeQuickItem { id: dvccCcl; uid: root.svc + "/Live/DvccCcl" }
	VeQuickItem { id: dvccCanAge; uid: root.svc + "/Live/DvccCanAge" }
	VeQuickItem { id: dvccWifiAge; uid: root.svc + "/Live/DvccWifiAge" }

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
		switch (dvccState.value) {
		case 0: return heard.length ? "Charge limits heard" + over + ", not followed" + (lim ? ": " + lim : "") : ""
		case 1: return "Charge limits: waiting, none heard"
		case 2: return "Charge limits: settling" + (lim ? ", " + lim : "") + over
		case 3: return "Charge limits: following " + (lim || "no values") + over
		case 4: return "Charge limits: stale, the sender went silent"
		case 5: return "Charge limits: untrusted" + (["", ", voltage limit out of range", ", current limit implausible",
													  ", values flapping"][dvccUntrust.value] || "")
		}
		return ""
	}
	readonly property color limitsColor: !dvccState.valid ? Theme.color_font_secondary
		: dvccState.value === 3 ? Theme.color_green
		: dvccState.value >= 4 ? Theme.color_orange : Theme.color_font_secondary

	// "a,b,,c" -> numbers, empty entries NaN
	function series(str) {
		return str ? String(str).split(",").map(s => s.length ? parseFloat(s) : NaN) : []
	}
	// Samples -> polyline points across a window of n slots, newest at the right edge; NaN slots are skipped
	function points(v, n, lo, hi, w, h) {
		if (!(hi > lo) || v.length === 0)
			return []
		const step = w / (n - 1)
		const x0 = w - (v.length - 1) * step
		let pts = []
		for (let i = 0; i < v.length; ++i) {
			if (isNaN(v[i]))
				continue
			const f = Math.max(lo, Math.min(hi, v[i]))
			pts.push(Qt.point(x0 + i * step, h - (f - lo) / (hi - lo) * h))
		}
		return pts
	}
	function validOf(v) { return v.filter(x => !isNaN(x)) }

	// The GX clock and its offset from UTC. ClockTime follows the GX's time-zone setting; a JS Date follows the process
	// (UTC on a GX) or the browser.
	readonly property real nowS: typeof ClockTime !== "undefined" && ClockTime.clockTime > 0 ? ClockTime.clockTime : Date.now() / 1000
	readonly property int tzS: {
		if (typeof ClockTime === "undefined" || !(ClockTime.clockTime > 0))
			return -new Date().getTimezoneOffset() * 60
		const u = new Date(ClockTime.clockTime * 1000)
		const d = ((ClockTime.hour * 60 + ClockTime.minute - u.getUTCHours() * 60 - u.getUTCMinutes()) % 1440 + 1440) % 1440
		return (d > 840 ? d - 1440 : d) * 60
	}
	// Barometer samples (the regulator's 10 min apart, the newest at epoch end) that fall on local day `day`, as points
	// on a midnight-to-midnight axis; NaN samples are skipped
	function dayPoints(v, end, day, lo, hi, w, h) {
		if (!(hi > lo))
			return []
		let pts = []
		for (let i = 0; i < v.length; ++i) {
			if (isNaN(v[i]))
				continue
			const t = end - (v.length - 1 - i) * 600 + tzS
			if (Math.floor(t / 86400) !== day)
				continue
			const f = Math.max(lo, Math.min(hi, v[i]))
			pts.push(Qt.point((t - day * 86400) / 86400 * w, h - (f - lo) / (hi - lo) * h))
		}
		return pts
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
		radius: Theme.geometry_overviewPage_widget_radius
		color: Theme.color_background_secondary
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
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Alt"; value1: root.fmt(root.num(altAmps), 1, "A"); template1: "\u2212888.8 A"
				label2: "Batt"; value2: root.fmt(root.num(battA), 1, "A"); template2: "\u2212888.8 A"
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Alt"; value1: root.fmt(root.num(altTemp), 1, Global.systemSettings.temperatureUnitSuffix)
				template1: "\u221288.8 " + Global.systemSettings.temperatureUnitSuffix
				label2: isNaN(root.num(battTemp)) ? "" : "Batt"
				value2: root.fmt(root.num(battTemp), 1, Global.systemSettings.temperatureUnitSuffix)
				template2: template1
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "RPM"; value1: root.fmt(root.num(rpm), 0, ""); template1: "8888"
				label2: "IGN"
				value2: !root.linkUp || !ignition.valid ? "--" : ignition.value === 1 ? "ON" : "OFF"
				color2: ignition.value === 1 ? root.brand : Theme.color_font_primary
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Health"; value1: isNaN(root.num(health)) ? "- %" : Math.round(root.num(health)) + " %"; template1: "100 %"
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
		}

		XregChartPerf {
			x: root.perfX
			y: root.perfY
			width: root.perfW
			height: root.perfH
			svc: root.svc
			linkUp: root.linkUp
			active: root.visible && !ScreenBlanker.blanked
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
				readonly property var v: root.series(histWind.value)
				readonly property var valid: root.validOf(v)
				readonly property real yMax: Math.max(10, Math.ceil((valid.length ? Math.max.apply(null, valid) : 0) * 1.15))
				hasData: valid.length > 1
				XregNum {
					anchors { left: parent.left; top: parent.top }
					template: "88 kn"
					text: Math.round(windTile.yMax) + " kn"
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_micro
				}
				Shape {
					id: windPlot
					anchors.fill: parent
					preferredRendererType: Shape.CurveRenderer
					readonly property var pts: root.points(windTile.v, 120, 0, windTile.yMax, width, height)   // bridge WIND_LEN
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
						strokeColor: root.brand
						strokeWidth: 2.5
						fillColor: "transparent"
						PathPolyline { path: windPlot.pts }
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
				// the latest sample
				readonly property var v: root.series(histBaro.value)
				readonly property real end: histBaroEnd.valid && histBaroEnd.value > 0 ? histBaroEnd.value : root.nowS
				readonly property int today: Math.floor((root.nowS + root.tzS) / 86400)
				readonly property var valid: root.validOf(v)
				readonly property real lo: valid.length ? Math.min(Math.min.apply(null, valid), Math.max.apply(null, valid) - 4) - 0.5 : 0
				readonly property real hi: valid.length ? Math.max.apply(null, valid) + 0.5 : 1
				readonly property var days: [0, 1, 2].map(k => root.dayPoints(v, end, today - k, lo, hi, baroPlot.width, baroPlot.height))
				readonly property var colors: [Theme.color_blue, Theme.color_orange, Theme.color_blackWater]
				readonly property var nowPt: days[0].length ? days[0][days[0].length - 1] : null
				readonly property real gutter: baroHi.width + 4   // lines start right of the scale; fixed, so the plot never shifts
				hasData: valid.length > 1
				footH: 18
				Row {
					anchors { left: parent.left; top: parent.bottom; topMargin: 4 }
					spacing: 10
					Repeater {
						model: baroTile.compact ? ["Today", "-1 d", "-2 d"] : ["Today", "Yesterday", "2 days ago"]
						delegate: Row {
							required property int index
							required property string modelData
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
					preferredRendererType: Shape.CurveRenderer
					ShapePath {
						strokeColor: baroTile.colors[2]
						strokeWidth: 2
						fillColor: "transparent"
						PathPolyline { path: baroTile.days[2] }
					}
					ShapePath {
						strokeColor: baroTile.colors[1]
						strokeWidth: 2
						fillColor: "transparent"
						PathPolyline { path: baroTile.days[1] }
					}
					ShapePath {
						strokeColor: baroTile.colors[0]
						strokeWidth: 3
						fillColor: "transparent"
						PathPolyline { path: baroTile.days[0] }
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
