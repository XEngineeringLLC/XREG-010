import QtQuick
import Victron.VenusOS

// Plots, the home page's second page (XregHome): Short Term and Long Term as in the web app, alternator Health, and
// Wind (opening on apparent wind). A tab is built on its first visit and kept, hidden, while another shows: a hidden tab
// stops its timers, so only the shown tab's data is requested from the bridge (/Ui/Want/*), and redraws nothing. On
// its return it resets to how it first opened (reopen()) and redraws.
Item {
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
	property bool windTrue: false
	property int healthWindow: 0
	property QtObject timing           // XregHome's bench timing log
	property int built                 // one bit per tab built so far

	Component.onCompleted: built |= 1 << tab
	onTabChanged: {
		if (!(built & (1 << tab))) {
			built |= 1 << tab
			return
		}
		const it = [shortSlot, longSlot, healthSlot, windSlot][tab].item
		if (it)
			it.reopen()
	}
	component TabSlot: Loader {
		required property int index
		anchors.fill: parent
		active: (page.built & (1 << index)) !== 0
		visible: page.tab === index
	}
	TabSlot { id: shortSlot; index: 0; sourceComponent: shortTab }
	TabSlot { id: longSlot; index: 1; sourceComponent: longTab }
	TabSlot { id: healthSlot; index: 2; sourceComponent: healthTab }
	TabSlot { id: windSlot; index: 3; sourceComponent: windTab }

	XregTabs {
		x: page.m
		y: 8
		width: page.barW
		model: ["Short Term", "Long Term", "Health", "Wind"]
		currentIndex: page.tab
		onPicked: function(index) { page.tab = index }
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
			timing: page.timing
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
			timing: page.timing
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
			current: page.tab === 2
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
			timing: page.timing
			onWindowPicked: function(minutes) { page.windWindow = minutes }
			onFramePicked: function(isTrue) { page.windTrue = isTrue }
		}
	}
}
