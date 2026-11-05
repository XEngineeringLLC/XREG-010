pragma Singleton
import QtQuick
QtObject {
	property real clockTime: Math.floor(Date.now() / 1000)
	property int offsetMin: 0   // the GX's time zone
	readonly property int hour: new Date((clockTime + offsetMin * 60) * 1000).getUTCHours()
	readonly property int minute: new Date((clockTime + offsetMin * 60) * 1000).getUTCMinutes()
}
