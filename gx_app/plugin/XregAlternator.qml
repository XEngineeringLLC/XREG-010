import QtQuick
import Victron.VenusOS

// Attached to product id 0xB091 (generic N2K alternator), so it also appears on other makes' alternators. Shows the
// regulator whose uid this alternator's N2K serial carries: all 16 hex, or the last 8 from older firmware (used only
// when one regulator on this GX matches).
DeviceListPluginPage {
	id: page
	title: "Dashboard"

	readonly property string serial: serialItem.valid ? String(serialItem.value).toUpperCase() : ""
	readonly property var unit: {
		if (page.serial.length < 8)
			return null
		const m = regs.units.filter(u => u.uid.toUpperCase().endsWith(page.serial))
		return m.length === 1 ? m[0] : null
	}

	VeQuickItem { id: serialItem; uid: page.device.serviceUid + "/Serial" }

	XregUnits { id: regs }

	Loader {
		anchors.fill: parent
		active: !!page.unit
		sourceComponent: XregDash {
			svc: regs.svcOf(page.unit)
			animationEnabled: page.animationEnabled
		}
	}

	GradientListView {
		anchors.fill: parent
		visible: !page.unit
		model: VisibleItemModel {
			ListText {
				text: "XREG-010"
				secondaryText: regs.running ? "Not on this GX" : "Bridge not running"
				caption: page.serial ? "Serial " + page.serial : ""
			}
			ListNavigation {
				text: "Regulators"
				preferredVisible: regs.running
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregRegulators.qml", { title: text })
			}
		}
	}
}
