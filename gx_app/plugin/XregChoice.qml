import QtQuick
import Victron.VenusOS

// One choice made on a screen, kept in localsettings at /Settings/Xregulator/<name> (the bridge creates it) so it
// survives a reboot. A pick shows at once, before localsettings answers. follow: take what another screen on this GX
// writes. Off for where a screen is (a page or a tab): it takes the saved value as it opens, and after that moves only
// when this screen moves it, so one person's tap never moves another's page.
VeQuickItem {
	id: c

	property string name
	property var shown               // set the default here: a saved value of another type is ignored
	property bool follow: true
	property bool settled

	uid: BackendConnection.serviceUidFromName("com.victronenergy.settings", 0) + "/Settings/Xregulator/" + name
	onValueChanged: take()
	Component.onCompleted: take()

	function take() {
		if ((follow || !settled) && typeof value === typeof shown) {
			settled = true
			shown = value
		}
	}
	function pick(v) {
		settled = true
		shown = v
		setValue(v)
	}
}
