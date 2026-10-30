import QtQuick
import Victron.VenusOS

// One regulator's link, a GX-only page: its row sits where the app shows the GX link (settings_keys.txt @connection,
// the app's "Victron GX" card). The bridge finds the regulator by its ID wherever it is on the network; the address is
// only where it was last seen. When it is nowhere and a different XREG-010 answers at that address, "Use this
// regulator" puts that one in its place. "Power flow devices" opens the GX devices the power flow remembers
// (XregFlowDevices), to forget one or to say how one is wired (a DC-DC charger drawing from the house bank, a device
// wired through the DC system meter).
Page {
	id: page

	property string svc
	property Item home                // the home page this was opened from (XregHome), for its header
	property string setupId           // the Setup sub-tab it sits under, lit in the header
	readonly property bool linkUp: connectedItem.valid && connectedItem.value === 1
	readonly property string uid: serialItem.valid ? String(serialItem.value) : ""
	readonly property string label: regs.labelOf(page.uid, nameItem.valid ? nameItem.value : "")
	readonly property string otherUid: otherUidItem.valid ? String(otherUidItem.value) : ""
	readonly property string otherLabel: regs.labelOf(page.otherUid, otherNameItem.valid ? otherNameItem.value : "")
	readonly property bool otherOnGx: regs.units.some(u => u.uid === page.otherUid)
	readonly property int boatToAdd: regs.found.filter(f => f.boat && !f.unit).length
	readonly property int limitsState: limitsStateItem.valid ? limitsStateItem.value : 0
	readonly property var flowDevices: regs.parse(flowDevicesItem)

	// figure spaces (a digit's width) in front, so the amps after the volts hold still as the limits change
	function figPad(text, chars) {
		let t = String(text)
		while (t.length < chars)
			t = "\u2007" + t
		return t
	}

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
	VeQuickItem { id: flowDevicesItem; uid: regs.mgr + "/GxDevices" }

	XregUnits { id: regs }

	Component {
		id: confirmDialog
		XregCfgDialog {}
	}

	Loader {
		id: header
		width: page.width
		active: !!page.home
		sourceComponent: XregHeader {
			svc: page.svc
			home: page.home
			pushed: true
			setupRow: true
			setupId: page.setupId
		}
	}

	GradientListView {
		anchors { top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
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
						: page.limitsState === 2 ? [limitsCvlItem.valid ? page.figPad(limitsCvlItem.value.toFixed(2), 5) + " V" : "",
													limitsCclItem.valid ? page.figPad(limitsCclItem.value.toFixed(1), 6) + " A" : ""]
												   .filter(t => t.length).join(", ")
						: page.limitsState === 3 ? "Another service on this GX controls this alternator directly"
						: page.limitsState === 4 ? "The regulator refused the last post. Sending resumes after it reconnects."
						: ""
				preferredVisible: page.limitsState > 0
			}
			ListNavigation {
				// the GX devices the power flow remembers at 0.0 A while they are off the GX, forgetting one, which side of
				// the house bank a DC-DC charger is on, and what is wired through a DC system meter
				text: "Power flow devices"
				secondaryText: {
					const gone = page.flowDevices.filter(d => !d.on).length
					return gone ? gone + " not on this GX now" : ""
				}
				preferredVisible: page.flowDevices.length > 0
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregFlowDevices.qml",
						{ title: text, svc: page.svc, home: page.home, setupId: page.setupId })
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
				onClicked: Global.pageManager.pushPage("qrc:/xregulator/XregRegulators.qml",
						{ title: text, svc: page.svc, home: page.home, setupId: page.setupId })
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
