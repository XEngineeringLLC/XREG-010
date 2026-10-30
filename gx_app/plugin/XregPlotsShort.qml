import QtQuick
import Victron.VenusOS

// Plots > Short Term: the web app's four live charts (current with the charge-stage strip, battery voltage, RPM,
// alternator temperature), each with field % on the right axis, over a 5 s to 5 min window. Data: the bridge's
// 1 s /Plot/Short series (last 300 s, sent every 2 s while this tab asks for them).
Item {
	id: tab

	property string svc
	property real m: 14
	property real toolX
	property real toolY
	property real bodyY
	property int windowS: 30
	signal windowPicked(int seconds)

	readonly property var windows: [5, 10, 30, 60, 120, 300]
	readonly property bool oneColumn: width < 760
	property real cursor: NaN
	property var d: ({})
	property bool dirty: true

	VeQuickItem { id: want; uid: tab.svc + "/Ui/Want/Short" }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: tab.visible && !ScreenBlanker.blanked
		onTriggered: want.setValue(Math.floor(Date.now() / 1000))
	}

	VeQuickItem { id: bcur; uid: tab.svc + "/Plot/Short/Bcur"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: altA; uid: tab.svc + "/Plot/Short/MeasuredAmps"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: fieldA; uid: tab.svc + "/Plot/Short/iiout"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: duty; uid: tab.svc + "/Plot/Short/dutyCycle"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: altPv; uid: tab.svc + "/Plot/Short/pidAltPV"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: adsV; uid: tab.svc + "/Plot/Short/BatteryV"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: inaV; uid: tab.svc + "/Plot/Short/IBV"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: rpm; uid: tab.svc + "/Plot/Short/RPM"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: altT; uid: tab.svc + "/Plot/Short/AlternatorTemperatureC"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: stage; uid: tab.svc + "/Plot/Short/chargeStage"; onValueChanged: tab.dirty = true }

	onWindowSChanged: snap()

	// "a,b,,c" -> numbers, empty entries NaN, newest last; keep the window and place sample i at -(n-1-i) s
	function series(str, conv) {
		let v = str ? String(str).split(",").map(s => s.length ? parseFloat(s) : NaN) : []
		v = v.slice(Math.max(0, v.length - windowS))
		return conv ? v.map(conv) : v
	}
	function xsFor(v) {
		const n = v.length
		return v.map((q, i) => i - (n - 1))
	}
	function line(src, key, label, unit, dec, color, more, conv) {
		const v = series(src.valid ? src.value : "", conv)
		return Object.assign({ key: key, label: label, unit: unit, dec: dec, color: color, x: xsFor(v), v: v }, more || {})
	}
	function toUserTemp(c) {
		return isFinite(c) ? Units.convert(c, VenusOS.Units_Temperature_Celsius, Global.systemSettings.temperatureUnit) : NaN
	}
	function snap() {
		dirty = false
		const pct = () => line(duty, "duty", "Field %", "%", 0, Theme.color_font_secondary, { right: true, dash: true })
		const sv = series(stage.valid ? stage.value : "")
		d = {
			current: [
				line(bcur, "bcur", "Battery Current", "A", 1, Theme.color_green),
				line(altA, "alt", "Alternator Current", "A", 1, Theme.color_blue),
				line(fieldA, "field", "Field Current", "A", 2, Theme.color_blackWater),
				pct(),
				line(altPv, "pv", "Alt Current filtered", "A", 1, Theme.color_liveWell, { dash: true, w: 1.5, off: true })
			],
			volts: [
				line(adsV, "ads", "ADS Battery", "V", 2, Theme.color_orange),
				line(inaV, "ina", "INA Battery", "V", 2, Theme.color_green),
				pct()
			],
			rpm: [line(rpm, "rpm", "RPM", "", 0, Theme.color_lpg), pct()],
			temp: [
				line(altT, "temp", "Alt. Temp", Global.systemSettings.temperatureUnitSuffix, 1, Theme.color_red, null, toUserTemp),
				pct()
			],
			stages: { x: xsFor(sv), c: sv.map(c => isFinite(c) ? Math.round(c) : 0) }
		}
	}
	Timer {
		interval: 1000
		repeat: true
		triggeredOnStart: true
		running: tab.visible
		onTriggered: if (tab.dirty) tab.snap()
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
			// two rows fill the page; on a 5-inch screen under the home page's header they would be 145 px, too low for the
			// current chart's three legend lines, so the page scrolls instead
			readonly property real tileH: tab.oneColumn ? 260 : Math.max(200, (tab.height - tab.bodyY - tab.m - gap) / 2)
			columns: tab.oneColumn ? 1 : 2
			spacing: gap

			Repeater {
				model: [
					{ k: "current", title: "Current History", unit: "A", zero: true },
					{ k: "volts", title: "Battery Voltage History", unit: "V", zero: false },
					{ k: "rpm", title: "RPM History", unit: "", zero: true },
					{ k: "temp", title: "Alternator Temperature History", unit: Global.systemSettings.temperatureUnitSuffix, zero: false }
				]
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
						xLo: -(tab.windowS - 1)
						xHi: 0
						xTicks: tab.ticks
						leftFromZero: modelData.zero
						leftUnit: modelData.unit
						rightFixed: [0, 100]
						rightUnit: "%"
						stages: modelData.k === "current" ? tab.d.stages || null : null
						emptyText: "Waiting for data"
						cursorX: tab.cursor
						onScrubbed: function(xValue) { tab.cursor = xValue }
					}
				}
			}
		}
	}
}
