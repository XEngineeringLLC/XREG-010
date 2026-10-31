import QtQuick
QtObject {
	id: root
	property string uid
	property var value: undefined
	readonly property bool valid: value !== undefined && value !== null
	property real min: NaN
	property real max: NaN
	property int sourceUnit
	property int displayUnit
	function setValue(v) {
		if (uid.length === 0) { console.warn("VeQuickItem.setValue with empty uid"); return }
		MockStore.write(uid, v)
	}
	function read() {
		const v = MockStore.get(uid)
		return (typeof v === "number" && sourceUnit === 1 && displayUnit === 2) ? v * 9 / 5 + 32 : v
	}
	onUidChanged: value = read()
	property Connections _c: Connections {
		target: MockStore
		function onChanged(u) { if (u === root.uid) root.value = root.read() }
	}
	Component.onCompleted: value = read()
}
