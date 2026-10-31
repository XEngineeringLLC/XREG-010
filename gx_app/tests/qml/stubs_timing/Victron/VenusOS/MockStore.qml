pragma Singleton
import QtQuick
QtObject {
	property var values: ({})
	property var writes: []
	property int created: 0
	signal changed(string uid)
	function put(uid, v) { values[uid] = v; changed(uid) }
	function get(uid) { return values[uid] }
	function reset() { values = ({}); writes = [] }
	function write(uid, v) { writes.push({ uid: uid, value: v }); values[uid] = v; changed(uid) }
}
