import QtQuick
import Victron.VenusOS

// What a control shows for a value it has written: after expect(v), v until the device reports it, or until the confirm
// time passes without that (a refused or lost write); otherwise what the device reports.
QtObject {
	id: root

	property var value                // what the device reports
	readonly property var shown: _busy ? _want : value
	property var _want
	property bool _busy

	function expect(v) {
		_want = v
		_busy = v !== value
		if (_busy)
			_confirm.restart()
		else
			_confirm.stop()
	}

	onValueChanged: {
		if (_busy && value === _want) {
			_busy = false
			_confirm.stop()
		}
	}

	// The bridge answers a D-Bus write at once and /GxDevices within its 500 ms tick; the VRM remote console adds seconds.
	readonly property Timer _confirm: Timer {
		interval: BackendConnection.vrm ? 4000 : 2000
		onTriggered: root._busy = false
	}
}
