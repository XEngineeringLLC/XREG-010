import QtQuick
import Victron.VenusOS

// The bridge's regulator lists, from com.victronenergy.xregulator: /Units (the ones this GX shows, each its own
// service) and /Found (every XREG-010 on the GX's networks). A regulator's ID is uid.slice(-12, -6), as in the app.
QtObject {
	id: root

	readonly property string mgr: BackendConnection.serviceUidFromName("com.victronenergy.xregulator", 0)
	readonly property bool running: unitsItem.valid
	readonly property var units: parse(unitsItem)
	readonly property var found: parse(foundItem)

	property VeQuickItem unitsItem: VeQuickItem { uid: root.mgr + "/Units" }
	property VeQuickItem foundItem: VeQuickItem { uid: root.mgr + "/Found" }

	function parse(item) {
		if (!item.valid || !item.value)
			return []
		try {
			return JSON.parse(item.value)
		} catch (e) {
			return []
		}
	}
	function svcOf(u) { return u ? BackendConnection.serviceUidFromName(u.service, u.instance) : "" }
	function idOf(uid) { return String(uid || "").slice(-12, -6).toUpperCase() }
	function tagOf(uid) { return "XREG-" + idOf(uid) }
	function labelOf(uid, name) {
		const tag = tagOf(uid)
		return name && String(name).toUpperCase() !== tag ? name + " (" + tag + ")" : tag
	}
}
