import QtQuick
import Victron.VenusOS

// The regulators this GX shows, then every other XREG-010 found on its networks (the boat's own first: the ones a
// regulator here lists under "Regulators on this boat"), then a search and adding one by address. Adding asks first.
Page {
	id: page

	property int waitAfter: -1   // /ProbeResult seq when this page sent an address; -1 = not waiting
	property string probeError
	readonly property var others: regs.found.filter(f => !f.unit)
	readonly property bool scanning: scanningItem.valid && scanningItem.value === 1
	readonly property var probe: {
		if (!probeResultItem.valid || !probeResultItem.value)
			return null
		try {
			return JSON.parse(probeResultItem.value)
		} catch (e) {
			return null
		}
	}

	function confirmAdd(f) {
		const uid = f.uid
		Global.dialogLayer.open(confirmDialog, {
			title: "Add regulator",
			body: "Show " + regs.labelOf(f.uid, f.name) + " (at " + f.host + ") on this GX? The GX will show its "
				  + "data, change its settings and switch its alternator on and off.",
			okFn: () => addItem.setValue(uid)
		})
	}

	onProbeChanged: {
		if (waitAfter < 0 || !probe || probe.seq <= waitAfter)
			return
		waitAfter = -1
		probeError = probe.ok ? "" : probe.message
		if (probe.ok && probe.unit)
			addItem.setValue(probe.uid)   // already on this GX: this only tells the bridge where it is now
		else if (probe.ok)
			confirmAdd(probe)
	}

	XregUnits { id: regs }

	VeQuickItem { id: addItem; uid: regs.mgr + "/Add" }
	VeQuickItem { id: scanItem; uid: regs.mgr + "/Scan" }
	VeQuickItem { id: scanningItem; uid: regs.mgr + "/Scanning" }
	VeQuickItem { id: probeResultItem; uid: regs.mgr + "/ProbeResult" }
	VeQuickItem {
		// the address field's own write: the answer to wait for is the next /ProbeResult
		uid: regs.mgr + "/Probe"
		onValueChanged: {
			if (valid && value) {
				page.waitAfter = page.probe ? page.probe.seq : 0
				page.probeError = ""
			}
		}
	}

	Component {
		id: confirmDialog
		XregCfgDialog {}
	}

	GradientListView {
		id: list
		anchors.fill: parent
		header: Column {
			width: list.width
			ListText {
				text: "Bridge not running"
				preferredVisible: !regs.running
			}
			SectionHeader {
				text: "On this GX"
				visible: regs.units.length > 0
			}
			Repeater {
				model: regs.units
				delegate: ListNavigation {
					width: list.width
					text: regs.labelOf(modelData.uid, modelData.name)
					secondaryText: modelData.connected ? "Connected" : "No data"
					caption: "ID " + modelData.id + (modelData.host ? ", " + modelData.host : "")
					onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregSettings.qml",
							{ title: text, unitUid: modelData.uid })
				}
			}
			SectionHeader {
				text: "Found on the network"
				visible: regs.running
			}
		}
		model: page.others
		delegate: ListButton {
			text: regs.labelOf(modelData.uid, modelData.name)
			caption: "ID " + modelData.id + ", " + modelData.host + (modelData.fw ? ", firmware " + modelData.fw : "")
					 + (modelData.boat ? ". On this boat." : "")
			secondaryText: "Add"
			writeAccessLevel: VenusOS.User_AccessType_User
			onClicked: page.confirmAdd(modelData)
		}
		footer: Column {
			width: list.width
			ListText {
				text: page.scanning ? "Searching" : "No other XREG-010 found"
				preferredVisible: regs.running && page.others.length === 0
			}
			ListButton {
				text: "Search the network"
				secondaryText: page.scanning ? "Searching" : "Search"
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: regs.running
				interactive: !page.scanning
				onClicked: scanItem.setValue(1)
			}
			ListTextField {
				text: "Add by address"
				dataItem.uid: regs.mgr + "/Probe"
				placeholderText: "IP address"
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: regs.running
			}
			ListText {
				text: "Add by address"
				secondaryText: "Not added"
				caption: page.probeError
				preferredVisible: page.probeError.length > 0
			}
		}
	}
}
