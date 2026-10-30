import QtQuick
import Victron.VenusOS

// gui-v2's Switch for a state the device owns: it shows value, and a touch asks for a state through commanded(on). Qt
// reads the touch (a tap toggles, a slide asks for the side the knob was slid to, a slide that ends where it started
// asks for nothing); a press that leaves the switch without sliding and lifts outside it asks for nothing either. The
// caller writes it and calls expect(on), and the switch shows on until value follows (XregSync).
Switch {
	id: root

	property bool value
	property bool _released
	property bool _slid
	property bool _outside
	readonly property bool shown: sync.shown

	signal commanded(bool on)

	function expect(on) { sync.expect(on) }
	// A touch sets checked from C++, which leaves the binding below in place but no longer applied
	function _show() { checked = Qt.binding(() => root.shown) }

	checkable: true
	checked: shown

	onPressed: {
		_released = false
		_slid = false
		_outside = false
	}
	// pressX/Y follow the finger while pressed, and the release point is set before Qt toggles; a key press moves neither
	onPressXChanged: if (pressed) _outside = !contains(Qt.point(pressX, pressY))
	onPressYChanged: if (pressed) _outside = !contains(Qt.point(pressX, pressY))
	// Qt moves position only while the knob is slid, and to checked's end when checked changes
	onPositionChanged: if (pressed && position !== (checked ? 1 : 0)) _slid = true
	onReleased: _released = true
	onToggled: {
		const on = checked
		if (on !== shown && (_slid || !_outside))
			commanded(on)
		_show()
	}
	// clicked() with no touch before it (a call from code): a tap
	onClicked: {
		if (!_released) {
			commanded(!shown)
			_show()
		}
		_released = false
	}

	XregSync {
		id: sync
		value: root.value
	}
}
