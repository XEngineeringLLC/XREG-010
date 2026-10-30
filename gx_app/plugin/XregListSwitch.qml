import QtQuick
import Victron.VenusOS

// gui-v2's ListSwitch for a state the device owns, as XregSwitch: it shows value, a touch on the switch asks for a state
// through commanded(on) (a slide that ends where it started, or a press that leaves the switch without sliding and lifts
// outside it, asks for nothing), and click() or the space key toggles. The caller writes it and calls expect(on). gui-v2
// draws the row, so it looks as its own switches do.
ListSwitch {
	id: root

	property bool value
	property bool _released
	property bool _slid
	property bool _outside
	readonly property bool shown: sync.shown

	signal commanded(bool on)

	function expect(on) { sync.expect(on) }
	function request(on) {
		if (root.checkWriteAccessLevel() && root.clickable)
			root.commanded(on)
	}
	function _track() {
		const s = _switchItem
		if (s.pressed)
			_outside = !s.contains(Qt.point(s.pressX, s.pressY))
	}

	// checkable lets Qt read the touch on gui-v2's switch (_switchItem): a tap toggles it, a slide sets the side it ends on
	checkable: true
	updateDataOnClick: false
	checked: shown
	// ListSwitch.click() copies the switch into checked, which drops this binding: show the device's state again
	onClicked: {
		if (!_released)
			request(!shown)
		_released = false
		checked = Qt.binding(() => root.shown)
	}

	Connections {
		target: root._switchItem ?? null
		function onPressed() {
			root._released = false
			root._slid = false
			root._outside = false
		}
		// as in XregSwitch: where the finger is while pressed, and whether the knob was slid
		function onPressXChanged() { root._track() }
		function onPressYChanged() { root._track() }
		function onPositionChanged() {
			const s = root._switchItem
			if (s.pressed && s.position !== (s.checked ? 1 : 0))
				root._slid = true
		}
		function onReleased() { root._released = true }
		function onToggled() {
			const on = root._switchItem.checked
			if (on !== root.shown && (root._slid || !root._outside))
				root.request(on)
		}
	}

	XregSync {
		id: sync
		value: root.value
	}
}
