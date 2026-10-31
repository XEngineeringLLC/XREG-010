pragma Singleton
import QtQuick
QtObject {
	enum SourceType { UnknownSource, DBusSource, MqttSource, MockSource }
	property int type: 1
	property bool vrm: false
	function serviceUidFromName(name, inst) { return "mock/" + name }
}
