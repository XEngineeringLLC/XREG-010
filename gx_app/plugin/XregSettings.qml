import QtQuick
import Victron.VenusOS

// Settings > Integrations entry: the home page of the GX's one regulator, or of the one picked (unitUid); with none
// or several, the list to pick from, the Regulators page and the GX app page.
Page {
	id: page

	property string unitUid
	// gui-v2's CaptionLabel reports a binding loop when a caption created empty first gets text: " " until complete
	property bool built
	// rc.local.disabled there and no rc.local (venus-platform's SystemHooksState bits RcLocalDisabled 1, RcLocal 4): the
	// bridge's startup line cannot run
	readonly property bool hooksOff: hooksItem.valid && (hooksItem.value & 1) !== 0 && (hooksItem.value & 4) === 0
	readonly property var unit: {
		const us = regs.units
		if (page.unitUid)
			return us.find(u => u.uid === page.unitUid) || null
		return us.length === 1 ? us[0] : null
	}

	title: "XREG-010"

	XregUnits { id: regs }

	VeQuickItem { id: appVersionItem; uid: regs.mgr + "/Update/Installed" }
	VeQuickItem { id: hooksItem; uid: BackendConnection.serviceUidFromName("com.victronenergy.platform", 0) + "/ModificationChecks/SystemHooksState" }

	Component.onCompleted: built = true

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
			caption: !page.built ? " " : !regs.running && page.hooksOff ? "Custom startup scripts are disabled on this GX, "
					+ "so the bridge does not start after a restart. Re-enable them in Settings > General > Support status."
					: ""
		}
		model: page.unitUid ? [] : regs.units
		delegate: ListNavigation {
			text: regs.labelOf(modelData.uid, modelData.name)
			secondaryText: modelData.connected ? "Connected" : "No data"
			onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregSettings.qml",
					{ title: text, unitUid: modelData.uid })
		}
		footer: Column {
			width: list.width
			ListNavigation {
				text: "Regulators"
				secondaryText: regs.units.length === 0 ? "Find and add" : "Add or remove"
				preferredVisible: regs.running
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregRegulators.qml", { title: text })
			}
			ListNavigation {
				text: "GX app"
				secondaryText: appVersionItem.valid ? "Version " + appVersionItem.value : ""
				preferredVisible: regs.running
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregGxApp.qml", { title: text })
			}
		}
	}
}
