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
	property bool trueWind: false
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

	// Frame and window buttons share the tool row at the body size where each label keeps 12 px a side (7-inch), at the
	// caption size where only 10 px a side fits (5-inch); narrower still (a phone held upright) the window buttons take
	// a row of their own under the frame buttons.
	TextMetrics { id: apparentBody; font.family: Global.fontFamily; font.pixelSize: Theme.font_size_body1; text: "Apparent" }
	TextMetrics { id: apparentCaption; font.family: Global.fontFamily; font.pixelSize: Theme.font_size_caption; text: "Apparent" }
	TextMetrics { id: sixtyBody; font.family: Global.fontFamily; font.pixelSize: Theme.font_size_body1; text: "60m" }
	TextMetrics { id: sixtyCaption; font.family: Global.fontFamily; font.pixelSize: Theme.font_size_caption; text: "60m" }
	function frameW(tm, pad) { return Math.ceil(2 * (tm.advanceWidth + 2 * pad)) }
	function windowsW(tm, pad) { return Math.ceil(4 * (tm.advanceWidth + 2 * pad)) }
	readonly property real toolW: width - toolX - m
	readonly property bool bodyRow: Math.max(200, frameW(apparentBody, 12)) + 12 + windowsW(sixtyBody, 12) <= toolW
	readonly property bool captionRow: !bodyRow && frameW(apparentCaption, 10) + 12 + windowsW(sixtyCaption, 10) <= toolW
	readonly property bool twoRows: !bodyRow && !captionRow
	readonly property real chartY: bodyY + (twoRows ? 50 : 0)

	SegmentedButtonRow {
		id: frameBar
		x: tab.toolX
		y: tab.toolY
		width: tab.captionRow ? tab.frameW(apparentCaption, 10) : 200
		height: 40
		fontPixelSize: tab.captionRow ? Theme.font_size_caption : Theme.font_size_body1
		model: [{ value: "True" }, { value: "Apparent" }]
		currentIndex: tab.trueWind ? 0 : 1
		onButtonClicked: function(buttonIndex) { tab.framePicked(buttonIndex === 0) }
	}
	SegmentedButtonRow {
		x: tab.twoRows ? tab.toolX : frameBar.x + frameBar.width + 12
		y: tab.twoRows ? tab.toolY + 50 : tab.toolY
		width: Math.min(260, tab.twoRows ? tab.toolW : tab.toolW - frameBar.width - 12)
		height: 40
		fontPixelSize: frameBar.fontPixelSize
		model: [{ value: "5m" }, { value: "15m" }, { value: "30m" }, { value: "60m" }]
		currentIndex: tab.windows.indexOf(tab.windowMin)
		onButtonClicked: function(buttonIndex) { tab.windowPicked(tab.windows[buttonIndex]) }
	}

	Rectangle {
		x: tab.m
		y: tab.chartY
		width: tab.width - 2 * tab.m
		height: tab.height - tab.chartY - tab.m
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
			leftCap: [0, 360, 90]
			rightFromZero: true
			rightUnit: "kn"
			emptyText: "No wind data"
			cursorX: tab.cursor
			onScrubbed: function(xValue) { tab.cursor = xValue }
		}
	}
}
