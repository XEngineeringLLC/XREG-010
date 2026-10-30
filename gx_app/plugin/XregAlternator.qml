import QtQuick
import Victron.VenusOS

// Attached to product id 0xB091 (generic N2K alternator), so it also appears on other makes' alternators. Shows the
// regulator whose uid this alternator's N2K serial carries: all 16 hex, or the last 8 from older firmware (used only
// when one regulator on this GX matches). With no match, only the way to the Regulators page.
DeviceListPluginPage {
	id: page
	title: "Dashboard"

	readonly property string serial: serialItem.valid ? String(serialItem.value).toUpperCase() : ""
	readonly property var match: {
		if (page.serial.length < 8)
			return null
		const m = regs.units.filter(u => u.uid.toUpperCase().endsWith(page.serial))
		return m.length === 1 ? m[0] : null
	}
	// The regulator once shown stays while it is on this GX: demo mode turns NMEA 2000 off, which removes the VE.Can
	// alternator this page was opened from, and with it the serial
	property string shownUid
	onMatchChanged: if (match) shownUid = match.uid
	Component.onCompleted: if (match) shownUid = match.uid
	readonly property var unit: page.match || regs.units.find(u => u.uid === page.shownUid) || null

	VeQuickItem { id: serialItem; uid: page.device.serviceUid + "/Serial" }

	XregUnits { id: regs }

	Loader {
		anchors.fill: parent
		active: !!page.unit
		sourceComponent: XregHome {
			svc: regs.svcOf(page.unit)
			animationEnabled: page.animationEnabled
			hostPage: page
		}
	}

	GradientListView {
		anchors.fill: parent
		visible: !page.unit
		model: VisibleItemModel {
			ListText {
				text: "Bridge not running"
				preferredVisible: !regs.running
			}
			ListNavigation {
				text: "Regulators"
				secondaryText: regs.units.length === 0 ? "Find and add" : "Add or remove"
				preferredVisible: regs.running
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregRegulators.qml", { title: text })
			}
		}
	}
}
