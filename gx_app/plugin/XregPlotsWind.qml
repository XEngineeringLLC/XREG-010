import QtQuick
import Victron.VenusOS

// Plots > Wind: wind speed and direction, true or apparent, over the last 5 to 60 minutes, as the web app's Wind
// Trend (direction on the left axis, speed on the right). Data: the bridge's 10 s means of the last hour
// (/Plot/Wind/*, angles averaged as vectors), sent every 10 s while this tab asks for them.
Item {
	id: tab

	property string svc
	property real m: 14
	property real toolX
	property real toolY
	property real bodyY
	property int windowMin: 30
	property bool trueWind: true
	signal windowPicked(int minutes)
	signal framePicked(bool isTrue)

	readonly property var windows: [5, 15, 30, 60]
	readonly property color brand: "#00a19a"    // web app --accent, the app's wind speed trace
	property real cursor: NaN
	property var d: ({})
	property bool dirty: true

	VeQuickItem { id: want; uid: tab.svc + "/Ui/Want/Wind" }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: tab.visible && !ScreenBlanker.blanked
		onTriggered: want.setValue(Math.floor(Date.now() / 1000))
	}
	VeQuickItem { id: tws; uid: tab.svc + "/Plot/Wind/Tws"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: twa; uid: tab.svc + "/Plot/Wind/Twa"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: aws; uid: tab.svc + "/Plot/Wind/Aws"; onValueChanged: tab.dirty = true }
	VeQuickItem { id: awa; uid: tab.svc + "/Plot/Wind/Awa"; onValueChanged: tab.dirty = true }

	onWindowMinChanged: snap()
	onTrueWindChanged: snap()

	// 10 s samples, newest last; sample i sits at -(n-1-i)/6 minutes
	function series(item) {
		const v = item.valid && item.value ? String(item.value).split(",").map(s => s.length ? parseFloat(s) : NaN) : []
		return v.slice(Math.max(0, v.length - windowMin * 6))
	}
	function snap() {
		dirty = false
		const dir = series(trueWind ? twa : awa)
		const spd = series(trueWind ? tws : aws)
		d = {
			lines: [
				{ key: "dir", label: trueWind ? "Direction" : "Angle", unit: "°", dec: 0, color: Theme.color_orange, wrap: true,
					x: dir.map((q, i) => (i - (dir.length - 1)) / 6), v: dir },
				{ key: "spd", label: "Speed", unit: "kn", dec: 1, color: tab.brand, right: true,
					x: spd.map((q, i) => (i - (spd.length - 1)) / 6), v: spd }
			]
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
		const st = windowMin <= 5 ? 1 : windowMin <= 15 ? 3 : windowMin <= 30 ? 5 : 10
		let t = []
		for (let mm = windowMin - st; mm >= 0; mm -= st)
			t.push({ x: -mm, t: mm === 0 ? "now" : mm + " min" })
		return t
	}

	Row {
		x: tab.toolX
		y: tab.toolY
		height: 40
		spacing: 12
		SegmentedButtonRow {
			width: 200
			height: 40
			fontPixelSize: Theme.font_size_body1
			model: [{ value: "True" }, { value: "Apparent" }]
			currentIndex: tab.trueWind ? 0 : 1
			onButtonClicked: function(buttonIndex) { tab.framePicked(buttonIndex === 0) }
		}
		SegmentedButtonRow {
			width: Math.min(260, tab.width - tab.toolX - tab.m - 212)
			height: 40
			fontPixelSize: Theme.font_size_body1
			model: [{ value: "5m" }, { value: "15m" }, { value: "30m" }, { value: "60m" }]
			currentIndex: tab.windows.indexOf(tab.windowMin)
			onButtonClicked: function(buttonIndex) { tab.windowPicked(tab.windows[buttonIndex]) }
		}
	}

	Rectangle {
		x: tab.m
		y: tab.bodyY
		width: tab.width - 2 * tab.m
		height: tab.height - tab.bodyY - tab.m
		radius: Theme.geometry_overviewPage_widget_radius
		color: Theme.color_background_secondary
		XregChart {
			anchors { fill: parent; margins: 12 }
			title: tab.trueWind ? "True wind" : "Apparent wind"
			lines: tab.d.lines || []
			xLo: -(tab.windowMin - 1 / 6)
			xHi: 0
			xTicks: tab.ticks
			leftUnit: "°"
			leftFixed: tab.trueWind ? [0, 360] : null
			leftStep: tab.trueWind ? 90 : 0
			rightFromZero: true
			rightUnit: "kn"
			emptyText: "No wind data"
			cursorX: tab.cursor
			onScrubbed: function(xValue) { tab.cursor = xValue }
		}
	}
}
