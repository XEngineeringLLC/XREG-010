import QtQuick
import Victron.VenusOS

// Settings > Integrations entry: the home page of the GX's one regulator, or of the one picked (unitUid); with none
// or several, the list to pick from and the Regulators page.
Page {
	id: page

	property string unitUid
	readonly property var unit: {
		const us = regs.units
		if (page.unitUid)
			return us.find(u => u.uid === page.unitUid) || null
		return us.length === 1 ? us[0] : null
	}

	title: "XREG-010"

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
		id: list
		anchors.fill: parent
		visible: !page.unit
		header: ListText {
			width: list.width
			text: !regs.running ? "Bridge not running"
					: page.unitUid ? "This regulator is no longer on this GX"
					: regs.units.length === 0 ? "No regulator on this GX yet" : "Regulators on this GX"
		}
		model: page.unitUid ? [] : regs.units
		delegate: ListNavigation {
			text: regs.labelOf(modelData.uid, modelData.name)
			secondaryText: modelData.connected ? "Connected" : "No data"
			onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregSettings.qml",
					{ title: text, unitUid: modelData.uid })
		}
		footer: ListNavigation {
			width: list.width
			text: "Regulators"
			secondaryText: regs.units.length === 0 ? "Find and add" : "Add or remove"
			preferredVisible: regs.running
			onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregRegulators.qml", { title: text })
		}
	}
}
