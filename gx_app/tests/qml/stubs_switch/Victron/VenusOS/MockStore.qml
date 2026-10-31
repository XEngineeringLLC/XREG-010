pragma Singleton
import QtQuick
QtObject {
	property var values: ({})
	property var writes: []
	// uid -> true: a write is recorded but the value stays (the bridge refused it, or its answer has not come yet)
	property var noEcho: ({})
	signal changed(string uid)
	function put(uid, v) { values[uid] = v; changed(uid) }
	function get(uid) { return values[uid] }
	function reset() { values = ({}); writes = []; noEcho = ({}) }
	function write(uid, v) { writes.push({ uid: uid, value: v, t: Date.now() }); if (noEcho[uid]) return; values[uid] = v; changed(uid) }
}
