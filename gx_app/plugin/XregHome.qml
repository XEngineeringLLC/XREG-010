import QtQuick
import QtQuick.Templates as T
import Victron.VenusOS

// The home page of one regulator (svc: its service uid), shared by the Integrations page and the alternator's Device
// List page: the header (XregHeader: page tabs and Alternator Enable) over a swipe view of Dashboard, Plots, Settings
// and Cloud, in that order; a swipe or a tab moves between them. A page loads the first time it comes into view or
// 2 s after the home page opens, and is visible only while on screen, so a page swiped away stops asking the bridge
// for its data (/Ui/Want/*). Settings pages pushed from here carry the same header and come back through back().
Item {
	id: root

	property string svc
	property bool animationEnabled
	property Item hostPage            // the gui-v2 page holding this one: pushed pages pop back to it
	property int setupTab             // the Setup sub-tab shown on the Settings page
	// A finger on a control that owns horizontal drags (a chart's cursor, XregChart): no page swipe until it lifts
	property int swipeHolds
	property bool preload
	readonly property int current: swipe.currentIndex
	readonly property bool compact: header.compact

	function go(index) {
		swipe.setCurrentIndex(index)
	}
	function back(index, tab) {
		if (tab !== undefined)
			root.setupTab = tab
		swipe.setCurrentIndex(index)
		if (root.hostPage)
			Global.pageManager.popPage(root.hostPage)
	}

	Timer {
		interval: 2000
		running: true
		onTriggered: root.preload = true
	}

	XregHeader {
		id: header
		width: root.width
		svc: root.svc
		home: root
	}

	// The Templates SwipeView with gui-v2's own list settings: gui-v2's SwipeView took a required animationEnabled in
	// 1.3 that 1.2.13 (this plugin's minimum) does not have, so no one declaration loads on both
	T.SwipeView {
		id: swipe
		readonly property bool moving: view.moving || settle.running
		function pageInView(pageX, pageW) {
			return pageX < view.contentX + pageW - 1 && pageX + pageW > view.contentX + 1
		}
		anchors { top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
		interactive: root.swipeHolds === 0
		clip: true
		contentItem: ListView {
			id: view
			model: swipe.contentModel
			interactive: swipe.interactive
			currentIndex: swipe.currentIndex
			onCurrentIndexChanged: settle.restart()   // a tab tap moves the view without setting moving
			orientation: ListView.Horizontal
			snapMode: ListView.SnapOneItem
			boundsBehavior: Flickable.StopAtBounds
			highlightRangeMode: ListView.StrictlyEnforceRange
			preferredHighlightBegin: 0
			preferredHighlightEnd: 0
			highlightMoveDuration: root.animationEnabled ? 250 : 0
			highlightMoveVelocity: root.animationEnabled ? 400 : -1
			maximumFlickVelocity: 4 * width
			keyNavigationEnabled: false
			Timer {
				id: settle
				interval: view.highlightMoveDuration
			}
		}

		Slot {
			page: Component {
				XregDash {
					svc: root.svc
					animationEnabled: root.animationEnabled && root.current === 0
				}
			}
		}
		Slot {
			page: Component { XregPlots { svc: root.svc } }
		}
		Slot {
			page: Component { XregSetup { svc: root.svc; home: root } }
		}
		Slot {
			page: Component { XregCloud { svc: root.svc } }
		}
	}

	component Slot: Item {
		id: slot
		property Component page
		property bool wanted
		readonly property bool inView: swipe.moving ? swipe.pageInView(x, width) : T.SwipeView.isCurrentItem
		onInViewChanged: if (inView) wanted = true
		Component.onCompleted: if (inView) wanted = true
		visible: inView
		Loader {
			anchors.fill: parent
			asynchronous: !slot.inView
			active: slot.wanted || root.preload
			sourceComponent: slot.page
		}
	}
}
