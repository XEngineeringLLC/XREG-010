import QtQuick
import Victron.VenusOS

// Plots > Wind: wind speed and direction, true or apparent, over the last 5 to 60 minutes, as the web app's Wind
// Trend (direction on the left axis, speed on the right). Data: every wind reading the GX receives (one per CSV4 frame)
// over the last hour, from the bridge's /Plot/Wind (XregWindStore) while this tab asks for it. Drawn as XregChart's
// streamed lines: each update converts only its new samples (one minimum and one maximum per pixel column), and the
// chart steps once per reading rather than scrolling every display frame: a reading moves the 5 min window about two
// pixels, too little for a scroll to show, and scrolling costs the GX a redraw every frame. A tap on the chart opens its
// larger view (XregChartPop), with Autoscale, Lock axes and axis limits kept on this GX, one set for each wind frame (the
// web app keeps its Wind Trend limits in the page); no cursor.
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
	property QtObject timing           // XregHome's bench timing log
	// true wind's left axis is a compass direction, apparent wind's an angle off the bow: each frame keeps its own
	readonly property XregView trueView: XregView { svc: tab.svc; name: "windTrue"; rightDefault: null }
	readonly property XregView appView: XregView { svc: tab.svc; name: "windApp"; rightDefault: null }
	readonly property XregView frameView: trueWind ? trueView : appView

	VeQuickItem { id: want; uid: tab.svc + "/Ui/Want/Wind" }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: tab.visible && !ScreenBlanker.blanked
		onTriggered: want.setValue(Math.floor(Date.now() / 1000))
	}
	XregWindStore {
		id: wind
		svc: tab.svc
		active: tab.visible
		timing: tab.timing
	}
	// a streamed chart takes new lines at its next step: step it once the lines follow the frame
	onTrueWindChanged: Qt.callLater(wind.bump)

	// the home page, which shows the larger view (XregPop)
	property Item home: null
	Component.onCompleted: {
		for (let p = parent; p; p = p.parent) {
			if (p.openPop !== undefined) {
				home = p
				break
			}
		}
	}
	// Back from another tab: the chart draws what came while hidden (XregWindStore), nothing more
	function reopen() {}

	// sk: the series in the store each line draws (the direction line's breaks at 0/360 are in the store)
	readonly property var lines: [
		{ key: "dir", sk: trueWind ? "Twa" : "Awa", label: trueWind ? "Direction" : "Angle", unit: "°", dec: 0,
		  color: Theme.color_orange },
		{ key: "spd", sk: trueWind ? "Tws" : "Aws", label: "Speed", unit: "kn", dec: 1, color: tab.brand, right: true }
	]
	// in seconds before the newest reading, as the streamed lines' x
	readonly property var ticks: {
		const st = windowMin <= 5 ? 1 : windowMin <= 15 ? 3 : windowMin <= 30 ? 5 : 10
		let t = []
		for (let mm = windowMin - st; mm >= 0; mm -= st)
			t.push({ x: -mm * 60, t: mm === 0 ? "now" : mm + " min" })
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
		WindChart {
			id: chart
			anchors { fill: parent; margins: 12 }
			tapOpens: true
			onOpened: if (tab.home) tab.home.openPop(chartPop)
		}
	}

	Component {
		id: chartPop
		XregChartPop {
			view: tab.frameView
			chart: bigChart
			title: tab.trueWind ? "True wind" : "Apparent wind"
			leftName: tab.trueWind ? "Direction" : "Angle"
			leftUnit: "°"
			rightName: "Speed"
			rightUnit: "kn"
			WindChart {
				id: bigChart
				anchors.fill: parent
				editAxes: true
			}
		}
	}

	// The wind chart, small or in its larger view: its axes from the frame's view, autoscale padded 10% (the angle at
	// least 30° wide, the speed from 0 and at least 10 kn), lines 3 px inside the plot's edges, a fixed axis's limits
	// labelled. True wind's direction is 0-360 in quarters under autoscale.
	component WindChart: XregChart {
		title: tab.trueWind ? "True wind" : "Apparent wind"
		lines: tab.lines
		stream: wind.store
		streamVer: wind.ver
		smooth: false
		xLo: -tab.windowMin * 60
		xHi: 0
		xTicks: tab.ticks
		view: tab.frameView
		leftUnit: "°"
		leftFixed: view.leftFixed || (tab.trueWind ? [0, 360] : null)
		leftStep: tab.trueWind && !view.leftFixed ? 90 : 0
		leftCap: [0, 360, 90]
		leftMinSpan: 30
		rightFixed: view.rightFixed
		rightFromZero: true
		rightMinSpan: 10
		rightUnit: "kn"
		emptyText: "No wind data"
		pad: 0.1
		inset: 3
		fixedEnds: true
	}
}
