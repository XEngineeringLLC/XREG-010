import QtQuick
import Victron.VenusOS

// The GX devices the power flow remembers (the bridge's /GxDevices, shared by every regulator on this GX), pushed from
// the Connection page. A device that drops off the GX when unpowered stays on the power flow at 0.0 A; Forget takes one
// the boat no longer has off it (/GxForget). One still on the GX would come straight back, so it has no Forget. An Orion
// DC-DC charger (the bridge sends "h" for those alone) has a switch for the other way to wire it, drawing from the house
// bank (/GxDrawsHouse): the GX reports both the same way, so the owner says which. A DC meter in DC-DC mode needs none:
// its sign says which. With a DC system meter on the GX, every row device and each pinned regulator's own alternator
// ("reg", listed then only for this) has a switch for whether its current passes through that meter (/GxMetered, the
// bridge's "m"), which no GX reading shows.
Page {
	id: page

	property string svc
	property Item home
	property string setupId
	readonly property var kindNames: ({
		alt: "Alternator", solar: "Solar charger", charger: "Shore charger", multi: "Inverter/charger", inverter: "Inverter",
		dcgenset: "DC generator", fuelcell: "Fuel cell", altx: "Alternator", dcdc: "DC-DC charger",
		wind: "Wind generator", water: "Water generator", shaft: "Shaft generator", source: "DC source",
		dcload: "DC load"
	})
	readonly property var devices: {
		if (!devicesItem.valid || !devicesItem.value)
			return []
		try {
			return JSON.parse(devicesItem.value)
		} catch (e) {
			return []
		}
	}

	XregUnits { id: regs }

	VeQuickItem { id: devicesItem; uid: regs.mgr + "/GxDevices" }
	VeQuickItem { id: forgetItem; uid: regs.mgr + "/GxForget" }
	VeQuickItem { id: drawsItem; uid: regs.mgr + "/GxDrawsHouse" }
	VeQuickItem { id: meteredItem; uid: regs.mgr + "/GxMetered" }

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
		id: list
		anchors { top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
		// The caption is " " until the header is created, as in XregCfgSection: gui-v2's CaptionLabel reports a binding loop
		// on its height the first time a caption created empty gets text (here, the last device forgotten with the page
		// open), never on one created with some.
		header: ListText {
			property bool built
			width: list.width
			text: page.devices.length ? "Devices the power flow lists" : "No devices remembered yet"
			caption: !built ? " " : page.devices.length ? "" : "A charger, inverter, MPPT, or DC meter is remembered the "
															  + "first time the power flow sees it on this GX."
			Component.onCompleted: built = true
		}
		model: page.devices
		delegate: Column {
			id: device
			required property var modelData
			width: list.width
			// a pinned regulator: no Forget, only its meter switch
			ListText {
				text: device.modelData.n
				caption: "Alternator · XREG-010"
				preferredVisible: !!device.modelData.reg
			}
			ListButton {
				text: device.modelData.n || device.modelData.p || page.kindNames[device.modelData.k] || device.modelData.k
				caption: (page.kindNames[device.modelData.k] || device.modelData.k)
						 + (device.modelData.n && device.modelData.p ? " · " + device.modelData.p : "")
						 + (device.modelData.on ? " · on this GX now" : " · not on this GX now, listed at 0.0 A")
				secondaryText: "Forget"
				preferredVisible: !device.modelData.reg
				enabled: !device.modelData.on
				writeAccessLevel: VenusOS.User_AccessType_User
				onClicked: {
					const key = device.modelData.key
					const name = text
					Global.dialogLayer.open(confirmDialog, {
						title: "Forget device",
						body: name + " leaves the power flow. If it comes back on this GX, it is listed again.",
						okFn: () => forgetItem.setValue(key)
					})
				}
			}
			// shows what the bridge saved, /GxDevices "h"; after a touch, the state asked for until "h" comes back with it
			XregListSwitch {
				id: draws
				text: "Draws from the house bank"
				caption: "For an Orion that charges another battery from the house bank. The power flow lists it under "
						 + "Loads."
				preferredVisible: device.modelData.h !== undefined
				value: device.modelData.h === 1
				writeAccessLevel: VenusOS.User_AccessType_User
				onCommanded: function(on) {
					drawsItem.setValue(JSON.stringify({ key: device.modelData.key, on: on ? 1 : 0 }))
					draws.expect(on)
				}
			}
			// the same for /GxDevices "m", sent only while the GX has a DC system meter
			XregListSwitch {
				id: metered
				text: "Wired through the DC system meter"
				caption: "On when its current passes through the DC system meter's shunt, so the meter's reading already "
						 + "includes it."
				preferredVisible: device.modelData.m !== undefined
				value: device.modelData.m === 1
				writeAccessLevel: VenusOS.User_AccessType_User
				onCommanded: function(on) {
					meteredItem.setValue(JSON.stringify({ key: device.modelData.key, on: on ? 1 : 0 }))
					metered.expect(on)
				}
			}
		}
	}
}
