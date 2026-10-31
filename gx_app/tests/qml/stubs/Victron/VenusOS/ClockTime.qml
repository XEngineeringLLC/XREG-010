pragma Singleton
import QtQuick
QtObject {
	property real clockTime: Math.floor(Date.now() / 1000)
	readonly property int hour: new Date(clockTime * 1000).getUTCHours()
	readonly property int minute: new Date(clockTime * 1000).getUTCMinutes()
}
