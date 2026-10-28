import QtQuick
import Victron.VenusOS

// One regulator's link. The bridge finds it by its ID wherever it is on the network; the address is only where it was
// last seen. When it is nowhere and a different XREG-010 answers at that address, "Use this regulator" puts that one
// in its place.
Page {
	id: page

	property string svc
	readonly property bool linkUp: connectedItem.valid && connectedItem.value === 1
	readonly property string uid: serialItem.valid ? String(serialItem.value) : ""
	readonly property string label: regs.labelOf(page.uid, nameItem.valid ? nameItem.value : "")
	readonly property string otherUid: otherUidItem.valid ? String(otherUidItem.value) : ""
	readonly property string otherLabel: regs.labelOf(page.otherUid, otherNameItem.valid ? otherNameItem.value : "")
	readonly property bool otherOnGx: regs.units.some(u => u.uid === page.otherUid)
	readonly property int boatToAdd: regs.found.filter(f => f.boat && !f.unit).length
	readonly property int limitsState: limitsStateItem.valid ? limitsStateItem.value : 0

	VeQuickItem { id: connectedItem; uid: page.svc + "/Connected" }
	VeQuickItem { id: statusItem; uid: page.svc + "/Link/Status" }
	VeQuickItem { id: hostItem; uid: page.svc + "/Link/Host" }
	VeQuickItem { id: channelItem; uid: page.svc + "/Link/Channel" }
	VeQuickItem { id: firmwareItem; uid: page.svc + "/Link/Firmware" }
	VeQuickItem { id: serialItem; uid: page.svc + "/Serial" }
	VeQuickItem { id: nameItem; uid: page.svc + "/CustomName" }
	VeQuickItem { id: otherUidItem; uid: page.svc + "/Link/OtherUid" }
	VeQuickItem { id: otherNameItem; uid: page.svc + "/Link/OtherName" }
	VeQuickItem { id: acceptItem; uid: page.svc + "/Link/AcceptRegulator" }
	VeQuickItem { id: limitsStateItem; uid: page.svc + "/Link/GxLimits/State" }
	VeQuickItem { id: limitsCvlItem; uid: page.svc + "/Link/GxLimits/Cvl" }
	VeQuickItem { id: limitsCclItem; uid: page.svc + "/Link/GxLimits/Ccl" }
	VeQuickItem { id: removeItem; uid: regs.mgr + "/Remove" }

	XregUnits { id: regs }

	Component {
		id: confirmDialog
		XregCfgDialog {}
	}

	GradientListView {
		anchors.fill: parent
		model: VisibleItemModel {
			ListText {
				text: "Regulator link"
				secondaryText: !regs.running ? "Bridge not running" : !connectedItem.valid ? "Not on this GX"
						: page.linkUp ? "Connected" : "No data from regulator"
				caption: statusItem.valid && statusItem.value && statusItem.value !== "Connected"
						 ? String(statusItem.value) : ""
			}
			ListText {
				text: "Regulator"
				secondaryText: nameItem.valid && nameItem.value ? String(nameItem.value) : regs.tagOf(page.uid)
				caption: "ID " + regs.idOf(page.uid)
				preferredVisible: page.uid.length > 0
			}
			ListText {
				text: "Address"
				secondaryText: hostItem.valid ? String(hostItem.value) : ""
				preferredVisible: secondaryText.length > 0
			}
			ListText {
				text: "Regulator firmware"
				secondaryText: firmwareItem.valid ? String(firmwareItem.value) : ""
				preferredVisible: secondaryText.length > 0
			}
			ListText {
				text: "Data stream"
				secondaryText: !channelItem.valid ? ""
						: channelItem.value === "gxevents" ? "GX stream" : channelItem.value === "events" ? "App stream" : String(channelItem.value)
				preferredVisible: secondaryText.length > 0
			}
			ListText {
				// what this GX's DVCC writes to the regulator's alternator service, posted over WiFi (/gxLimits)
				text: "Charge limits from this GX"
				secondaryText: ["", "Not sending", "Sending", "Handled by the GX", "Refused"][page.limitsState] || ""
				caption: page.limitsState === 1 ? "DVCC on this GX has no limit for this alternator"
						: page.limitsState === 2 ? [limitsCvlItem.valid ? limitsCvlItem.value.toFixed(2) + " V" : "",
													limitsCclItem.valid ? limitsCclItem.value.toFixed(1) + " A" : ""]
												   .filter(t => t.length).join(", ")
						: page.limitsState === 3 ? "Another service on this GX controls this alternator directly"
						: page.limitsState === 4 ? "The regulator refused the last post. Sending resumes after it reconnects."
						: ""
				preferredVisible: page.limitsState > 0
			}
			ListButton {
				text: "Use this regulator"
				secondaryText: "Use"
				caption: page.otherLabel + " answers at this address"
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: page.otherUid.length > 0 && !page.otherOnGx
				onClicked: {
					const other = page.otherUid   // the one the owner confirms is the one pinned
					Global.dialogLayer.open(confirmDialog, {
						title: "Use this regulator",
						body: page.otherLabel + " takes the place of " + page.label + " on this GX. The GX will show its "
							  + "data, change its settings and switch its alternator on and off.",
						okFn: () => acceptItem.setValue(other)
					})
				}
			}
			ListNavigation {
				text: "Regulators"
				secondaryText: page.boatToAdd > 0 ? page.boatToAdd + " more on this boat" : ""
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregRegulators.qml", { title: text })
			}
			ListButton {
				text: "Remove from this GX"
				secondaryText: "Remove"
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: connectedItem.valid
				onClicked: {
					const uid = page.uid
					Global.dialogLayer.open(confirmDialog, {
						title: "Remove from this GX",
						body: "The GX will stop showing " + page.label + " and stop changing anything on it. You can "
							  + "add it again on the Regulators page.",
						okFn: () => removeItem.setValue(uid)
					})
				}
			}
		}
	}
}
