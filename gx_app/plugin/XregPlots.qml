import QtQuick
import Victron.VenusOS

// Plots page, pushed from the home page: Short Term and Long Term as in the web app, alternator Health, and Wind.
// Each tab is loaded only while shown, so only its data is requested from the bridge (/Ui/Want/*).
Page {
	id: page

	property string svc
	readonly property real m: 14
	readonly property bool narrow: width < 760
	readonly property real barW: narrow ? width - 2 * m : Math.min(464, (width - 3 * m) * 0.48)
	readonly property real toolX: narrow ? m : m + barW + m
	readonly property real toolY: narrow ? 58 : 8
	readonly property real bodyY: narrow ? 108 : 58
	property int tab: 0
	// Window choices survive switching tabs while the page is open
	property int shortWindow: 30
	property int windWindow: 30
	property bool windTrue: true
	property int healthWindow: 0

	title: "Plots"

	Loader {
		anchors.fill: parent
		sourceComponent: [shortTab, longTab, healthTab, windTab][page.tab]
	}

	TabBar {
		x: page.m
		y: 8
		width: page.barW
		height: 40
		fontPixelSize: page.width < 900 ? Theme.font_size_caption : Theme.font_button_size
		model: [{ value: "Short Term" }, { value: "Long Term" }, { value: "Health" }, { value: "Wind" }]
		currentIndex: page.tab
		onButtonClicked: function(buttonIndex) { page.tab = buttonIndex }
	}

	Component {
		id: shortTab
		XregPlotsShort {
			svc: page.svc
			m: page.m
			toolX: page.toolX
			toolY: page.toolY
			bodyY: page.bodyY
			windowS: page.shortWindow
			onWindowPicked: function(seconds) { page.shortWindow = seconds }
		}
	}
	Component {
		id: longTab
		XregPlotsLong {
			svc: page.svc
			m: page.m
			toolX: page.toolX
			toolY: page.toolY
			bodyY: page.bodyY
		}
	}
	Component {
		id: healthTab
		XregPlotsHealth {
			svc: page.svc
			m: page.m
			toolX: page.toolX
			toolY: page.toolY
			bodyY: page.bodyY
			windowH: page.healthWindow
			onWindowPicked: function(hours) { page.healthWindow = hours }
		}
	}
	Component {
		id: windTab
		XregPlotsWind {
			svc: page.svc
			m: page.m
			toolX: page.toolX
			toolY: page.toolY
			bodyY: page.bodyY
			windowMin: page.windWindow
			trueWind: page.windTrue
			onWindowPicked: function(minutes) { page.windWindow = minutes }
			onFramePicked: function(isTrue) { page.windTrue = isTrue }
		}
	}
}
