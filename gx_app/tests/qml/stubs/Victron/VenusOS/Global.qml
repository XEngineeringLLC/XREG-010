pragma Singleton
import QtQuick
QtObject {
	property var dialogLayer: null
	property var pageManager: null
	property var mainView: null
	property var toasts: []
	property string fontFamily: "Roboto"
	property QtObject systemSettings: QtObject {
		property int temperatureUnit: 2
		readonly property string temperatureUnitSuffix: temperatureUnit === 1 ? "°C" : "°F"
		property string serviceUid: "mock/com.victronenergy.settings"
		function canAccess(level) { return true }
	}
	function showToastNotification(type, text, ms) { toasts.push(text); console.log("TOAST: " + text); return 1 }
}
