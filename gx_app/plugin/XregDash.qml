import QtQuick
import QtQuick.Shapes
import Victron.VenusOS

// Home page for one regulator (svc: its service uid), shared by the Integrations page and the alternator's
// Device List page.
// Top: the web app's header banner (wordmark, status words, off-reason strip, chips, Alternator Enable).
// Below: alternator gauge, the app's boat speed-performance view (XregChartPerf), wind for the last hour and barometer
// for 24 hours over the two days before, then the Plots and Settings buttons.
// Under 600 px wide (remote console on a phone held upright) the same pieces stack in one scrolling column.
// Reads the bridge's /Live and /History paths; temperatures arrive in Celsius and show in the owner's unit.
// Status, stage, limiter and off-reason words are the web app's own (script.js fieldOffReasonText etc.).
Item {
	id: root

	property bool animationEnabled
	property string svc
	readonly property bool linkUp: connected.valid && connected.value === 1
	readonly property real capMax: capMaxItem.valid && capMaxItem.value > 0 ? capMaxItem.value : 100
	readonly property color brand: "#00a19a"    // web app --accent
	readonly property real m: 14
	readonly property real gap: 10
	readonly property bool portrait: width < 600
	readonly property real sc: portrait ? 0.85 : Math.min(1, width / 1024)    // 0.78 on an 800-wide GX Touch 50
	readonly property real stripY: portrait ? 142 : 60
	readonly property real chipsY: stripY + 36
	readonly property real chipsH: 76
	readonly property real chipW: portrait ? (width - 2 * m - gap) / 2 : (width - 2 * m - Math.round(250 * sc) - 5 * gap) / 5
	readonly property real lowerY: chipsY + (portrait ? 3 * chipsH + 2 * gap : chipsH) + gap
	readonly property real lowerH: portrait ? 0 : height - lowerY - m
	// 5-inch GX Touch 50 (228 px below the chips): the page buttons move under the gauge so wind and barometer get
	// the whole right column
	readonly property bool tight: !portrait && lowerH < 300
	readonly property real navH: 46
	readonly property real colW: portrait ? width - 2 * m : Math.min(width * 0.27, tight ? width : lowerH / 0.82)
	// 24: the gauge's 0 and full-scale labels hang below its 0.82 box
	readonly property real heroW: portrait ? Math.min(width - 2 * m, 320)
										   : tight ? Math.min(colW, (lowerH - navH - gap - 24) / 0.82) : colW
	readonly property real perfX: portrait ? m : m + colW + gap
	readonly property real perfY: portrait ? lowerY + heroW * 0.82 + 30 : lowerY
	readonly property real perfW: portrait ? width - 2 * m : Math.round(width * 0.42)
	readonly property real perfH: portrait ? 340 : lowerH
	readonly property real sideX: portrait ? m : perfX + perfW + gap
	readonly property real sideY: portrait ? perfY + perfH + gap : lowerY
	readonly property real sideW: portrait ? width - 2 * m : width - sideX - m
	readonly property real sideH: portrait ? 2 * 160 + 46 + 2 * gap : lowerH

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
	VeQuickItem { id: trueWind; uid: root.svc + "/Live/TrueWind" }
	VeQuickItem { id: capMaxItem; uid: root.svc + "/Live/AltCapMax" }
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
	VeQuickItem { id: histWind; uid: root.svc + "/History/Wind" }
	VeQuickItem { id: histBaro; uid: root.svc + "/History/Baro" }
	VeQuickItem { id: dvccState; uid: root.svc + "/Live/DvccState" }
	VeQuickItem { id: dvccUntrust; uid: root.svc + "/Live/DvccUntrust" }
	VeQuickItem { id: dvccCvl; uid: root.svc + "/Live/DvccCvl" }
	VeQuickItem { id: dvccCcl; uid: root.svc + "/Live/DvccCcl" }
	VeQuickItem { id: dvccCanAge; uid: root.svc + "/Live/DvccCanAge" }
	VeQuickItem { id: dvccWifiAge; uid: root.svc + "/Live/DvccWifiAge" }

	function num(item) { return root.linkUp && item.valid ? item.value : NaN }
	function fmt(v, decimals, unit) { return isNaN(v) ? "--" : v.toFixed(decimals) + (unit ? " " + unit : "") }

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
		const lim = [dvccCvl.valid ? dvccCvl.value.toFixed(2) + " V" : "", dvccCcl.valid ? dvccCcl.value.toFixed(1) + " A" : ""]
				.filter(t => t.length).join(", ")
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

	component Chip: Rectangle {
		property real sc: 1
		property string label1
		property string value1
		property color color1: Theme.color_font_primary
		property string label2
		property string value2
		property color color2: Theme.color_font_primary
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
					Label {
						anchors { right: parent.right; verticalCenter: parent.verticalCenter }
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
		Label {
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
			x: root.m; y: 10
			height: 46
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
			x: root.portrait ? root.m : root.width - root.m - width
			y: root.portrait ? 56 : 10
			height: 46
			verticalAlignment: Text.AlignVCenter
			text: root.statusWord + (root.statusWord === "ACTIVE" && field.valid ? "  " + Math.round(field.value) + "%" : "")
			color: root.statusColor
			font.pixelSize: Theme.font_size_h1
			font.bold: true
		}
		Row {
			x: root.portrait ? root.m : statusLabel.x - 10 - width
			y: root.portrait ? 102 : 10
			height: root.portrait ? 36 : 46
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

		// Chips, as in the app header, then Alternator Enable
		Grid {
			id: chips
			x: root.m; y: root.chipsY
			columns: root.portrait ? 2 : 6
			spacing: root.gap
			readonly property real enableW: root.portrait ? root.chipW : Math.round(250 * root.sc)
			readonly property real chipW: root.chipW

			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Batt"; value1: root.fmt(root.num(battV), 2, "V")
				label2: "SOC"; value2: root.fmt(root.num(soc), 1, "%")
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Alt"; value1: root.fmt(root.num(altAmps), 1, "A")
				label2: "Batt"; value2: root.fmt(root.num(battA), 1, "A")
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Alt"; value1: root.fmt(root.num(altTemp), 1, Global.systemSettings.temperatureUnitSuffix)
				label2: isNaN(root.num(battTemp)) ? "" : "Batt"
				value2: root.fmt(root.num(battTemp), 1, Global.systemSettings.temperatureUnitSuffix)
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "RPM"; value1: root.fmt(root.num(rpm), 0, "")
				label2: "IGN"
				value2: !root.linkUp || !ignition.valid ? "--" : ignition.value === 1 ? "ON" : "OFF"
				color2: ignition.value === 1 ? root.brand : Theme.color_font_primary
			}
			Chip {
				width: chips.chipW; height: root.chipsH
				sc: root.sc
				label1: "Health"; value1: isNaN(root.num(health)) ? "- %" : Math.round(root.num(health)) + " %"
			}
			Rectangle {
				width: chips.enableW; height: root.chipsH
				radius: Theme.geometry_overviewPage_widget_radius
				color: Theme.color_background_secondary
				Label {
					anchors { left: parent.left; leftMargin: 14; right: enableSwitch.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
					text: "Alternator Enable"
					wrapMode: Text.WordWrap
					font.pixelSize: Math.round(Theme.font_size_body2 * root.sc)
					font.bold: true
				}
				Switch {
					id: enableSwitch
					anchors { right: parent.right; rightMargin: 14; verticalCenter: parent.verticalCenter }
					enabled: root.linkUp
					checkable: false
					checked: onOff.valid && onOff.value === 1
					onClicked: onOff.setValue(checked ? 0 : 1)
				}
			}
		}

		// Hero gauge: alternator current on the outer arc (full scale = largest RPM-table current cap), field % on the
		// inner arc, target current as an orange tick
		Item {
			id: hero
			x: root.portrait ? (root.width - width) / 2 : root.m + (root.colW - width) / 2
			y: root.portrait || root.tight ? root.lowerY : root.lowerY + (root.lowerH - height) / 2
			width: root.heroW
			height: root.heroW * 0.82
			readonly property real r: width / 2
			readonly property real k: Math.min(1, width / 216)    // rings thin below the 800-wide size so the words fit
			readonly property real inset: Math.round(34 * k)
			readonly property real span: 260
			readonly property real endDx: Math.sin(span / 2 * Math.PI / 180) * (r - 13)
			readonly property real endY: r - Math.cos(span / 2 * Math.PI / 180) * (r - 13)

			Item {
				width: hero.width; height: hero.width
				layer.enabled: !UiConfig.msaaEnabled
				layer.textureSize: Qt.size(2 * width, 2 * height)
				layer.smooth: true

				ProgressArc {
					id: ampArc
					anchors.fill: parent
					radius: hero.r
					startAngle: -hero.span / 2
					endAngle: hero.span / 2
					strokeWidth: Math.round(24 * hero.k)
					value: isNaN(root.num(altAmps)) ? 0 : 100 * root.num(altAmps) / root.capMax
					progressColor: Theme.color_ok
					remainderColor: Theme.color_darkOk
					animationEnabled: root.animationEnabled
				}
				ProgressArc {
					x: hero.inset; y: hero.inset
					width: hero.width - 2 * hero.inset; height: width
					radius: hero.r - hero.inset
					startAngle: -hero.span / 2
					endAngle: hero.span / 2
					strokeWidth: Math.round(10 * hero.k)
					value: isNaN(root.num(field)) ? 0 : root.num(field)
					progressColor: Theme.color_green
					remainderColor: Theme.color_darkGreen
					animationEnabled: root.animationEnabled
				}
				Item {
					x: hero.r; y: hero.r
					rotation: ampArc.progressAnimatedEndAngle
					Rectangle { x: -3; y: -hero.r - 2; width: 6; height: Math.round(28 * hero.k); radius: 3; color: Theme.color_font_primary }
				}
				Item {
					visible: root.statusWord === "ACTIVE" && ampTarget.valid
					x: hero.r; y: hero.r
					rotation: -hero.span / 2 + hero.span * Math.max(0, Math.min(1, root.num(ampTarget) / root.capMax))
					Rectangle { x: -2; y: -hero.r - 8; width: 4; height: 14; radius: 2; color: Theme.color_orange }
				}
			}
			Column {
				readonly property real cap: Math.max(12, Math.round(hero.width * 0.058))
				anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: hero.r * 0.46 }
				spacing: 2
				QuantityLabel {
					anchors.horizontalCenter: parent.horizontalCenter
					value: root.num(altAmps)
					unit: VenusOS.Units_Amp
					decimals: 1
					font.pixelSize: Math.round(hero.width * 0.2)
				}
				Label {
					anchors.horizontalCenter: parent.horizontalCenter
					text: "Alternator current"
					color: Theme.color_font_secondary
					font.pixelSize: parent.cap
				}
				Item { width: 1; height: 4 }
				QuantityLabel {
					anchors.horizontalCenter: parent.horizontalCenter
					value: root.num(field)
					unit: VenusOS.Units_Percentage
					decimals: 0
					valueColor: Theme.color_green
					font.pixelSize: Math.round(hero.width * 0.123)
				}
				Label {
					anchors.horizontalCenter: parent.horizontalCenter
					text: "Field"
					color: Theme.color_font_secondary
					font.pixelSize: parent.cap
				}
			}
			Label {
				x: hero.r - hero.endDx - width / 2
				y: hero.endY + 14
				text: "0"
				color: Theme.color_font_secondary
				font.pixelSize: Theme.font_size_caption
			}
			Label {
				x: hero.r + hero.endDx - width / 2
				y: hero.endY + 14
				text: Math.round(root.capMax) + " A"
				color: Theme.color_font_secondary
				font.pixelSize: Theme.font_size_caption
			}
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

		// Right column: wind, barometer, page buttons
		Item {
			id: side
			x: root.sideX
			y: root.sideY
			width: root.sideW
			height: root.sideH
			readonly property real chartH: (height - root.gap - (root.tight ? 0 : root.navH + root.gap)) / 2

			ChartTile {
				id: windTile
				width: side.width; height: side.chartH
				title: "Wind, last hour"
				compactTitle: "Wind, 1 h"
				note: isNaN(root.num(trueWind)) ? "" : root.num(trueWind).toFixed(1) + (compact ? " kn" : " kn true")
				empty: "No wind data"
				readonly property var v: root.series(histWind.value)
				readonly property var valid: root.validOf(v)
				readonly property real yMax: Math.max(10, Math.ceil((valid.length ? Math.max.apply(null, valid) : 0) * 1.15))
				hasData: valid.length > 1
				Label {
					anchors { left: parent.left; top: parent.top }
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
				// 10-minute samples, newest last: today = last 144, then the 144 before each
				readonly property var v: root.series(histBaro.value)
				readonly property var today: v.slice(Math.max(0, v.length - 144))
				readonly property var day1: v.slice(Math.max(0, v.length - 288), Math.max(0, v.length - 144))
				readonly property var day2: v.slice(Math.max(0, v.length - 432), Math.max(0, v.length - 288))
				readonly property var valid: root.validOf(v)
				readonly property real lo: valid.length ? Math.min(Math.min.apply(null, valid), Math.max.apply(null, valid) - 4) - 0.5 : 0
				readonly property real hi: valid.length ? Math.max.apply(null, valid) + 0.5 : 1
				hasData: root.validOf(today).length > 1
				footH: 18
				Row {
					anchors { left: parent.left; top: parent.bottom; topMargin: 4 }
					spacing: 12
					Label { text: "Today"; color: Theme.color_ok; font.pixelSize: Theme.font_size_micro }
					Label { text: "Yesterday"; color: Theme.color_font_secondary; font.pixelSize: Theme.font_size_micro }
					Label { text: "2 days ago"; color: Theme.color_font_disabled; font.pixelSize: Theme.font_size_micro }
				}
				Label {
					anchors { left: parent.left; top: parent.top }
					text: baroTile.hi.toFixed(0)
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_micro
				}
				Label {
					anchors { left: parent.left; bottom: parent.bottom }
					text: baroTile.lo.toFixed(0)
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_micro
				}
				Shape {
					id: baroPlot
					anchors.fill: parent
					preferredRendererType: Shape.CurveRenderer
					ShapePath {
						strokeColor: Theme.color_font_disabled
						strokeWidth: 1.5
						fillColor: "transparent"
						PathPolyline { path: root.points(baroTile.day2, 144, baroTile.lo, baroTile.hi, baroPlot.width, baroPlot.height) }
					}
					ShapePath {
						strokeColor: Theme.color_font_secondary
						strokeWidth: 1.5
						fillColor: "transparent"
						PathPolyline { path: root.points(baroTile.day1, 144, baroTile.lo, baroTile.hi, baroPlot.width, baroPlot.height) }
					}
					ShapePath {
						strokeColor: Theme.color_ok
						strokeWidth: 3
						fillColor: "transparent"
						PathPolyline { path: root.points(baroTile.today, 144, baroTile.lo, baroTile.hi, baroPlot.width, baroPlot.height) }
					}
				}
			}

		}

		Row {
			id: nav
			x: root.tight ? root.m : side.x
			y: root.tight ? root.lowerY + root.lowerH - height : side.y + side.height - height
			width: root.tight ? root.colW : side.width
			height: root.navH
			spacing: root.gap
			Button {
				width: (nav.width - root.gap) / 2; height: nav.height
				flat: false
				text: "Plots"
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregPlots.qml", { title: "Plots", svc: root.svc })
			}
			Button {
				width: (nav.width - root.gap) / 2; height: nav.height
				flat: false
				text: "Settings"
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregSetup.qml", { title: "Settings", svc: root.svc })
			}
		}
	}
}
