import QtQuick
import Victron.VenusOS

// The GX app itself, plugin and bridge under one version number (xreg_update.py, /Update/* on the bridge's manager
// service): the version installed, the newest on X Engineering's OTA host, Update and Remove. A version issued again
// carries a newer build (epoch seconds, shown as its UTC date), which counts as newer. Opening the page has the bridge
// read the release list once, as Check does; nothing else reads it. An update restarts the bridge, and gui-v2 reloads
// the plugin when it changed, so every state shown is the bridge's.
Page {
	id: page

	property string svc               // with home: the regulator whose home page this was opened from, for its header
	property Item home
	property string setupId
	property bool checkAsked
	// gui-v2's CaptionLabel reports a binding loop when a caption created empty first gets text, never on one created
	// with some: captions are " " until the page is complete
	property bool built
	readonly property bool bridgeUp: installedItem.valid
	// rc.local.disabled there and no rc.local (bits RcLocalDisabled 1, RcLocal 4): the bridge's startup line cannot run
	readonly property bool hooksOff: hooksItem.valid && (hooksItem.value & 1) !== 0 && (hooksItem.value & 4) === 0
	// a Check tap shows Checking at once: the bridge reports its state at its next tick, up to 0.5 s later
	property bool checkTapped
	readonly property int updState: checkTapped ? 1 : stateItem.valid ? stateItem.value : 0
	readonly property string installedVersion: installedItem.valid ? String(installedItem.value) : ""
	// builds are epoch seconds: past 2038 they no longer fit a QML int
	readonly property real installedBuild: installedBuildItem.valid ? installedBuildItem.value : 0
	readonly property string latestVersion: latestItem.valid ? String(latestItem.value) : ""
	readonly property real latestBuild: latestBuildItem.valid ? latestBuildItem.value : 0
	readonly property bool sameVersion: page.latestVersion === page.installedVersion
	// checking, downloading, checking the signature, installing, removing
	readonly property bool updBusy: [1, 4, 5, 6, 8].indexOf(page.updState) >= 0
	readonly property bool updOffer: !page.updBusy && (page.updState === 3 || page.updState === 7)
			&& page.isNewer(page.latestVersion, page.latestBuild, page.installedVersion, page.installedBuild)
	readonly property var lastResult: {
		if (!lastItem.valid || !lastItem.value)
			return null
		try {
			return JSON.parse(lastItem.value)
		} catch (e) {
			return null
		}
	}

	// release a (version, build) newer than b: version field by field, then build
	function isNewer(a, ab, b, bb) {
		if (!/^[0-9]+(\.[0-9]+)*$/.test(a))
			return false
		const x = a.split(".").map(Number)
		const y = /^[0-9]+(\.[0-9]+)*$/.test(b) ? b.split(".").map(Number) : []
		for (let i = 0; i < Math.max(x.length, y.length); i++) {
			if ((x[i] || 0) !== (y[i] || 0))
				return (x[i] || 0) > (y[i] || 0)
		}
		return ab > bb
	}
	function dateText(b) {
		return new Date(b * 1000).toISOString().slice(0, 16).replace("T", " ") + " UTC"
	}
	function buildText(b) {
		return b > 0 ? "Build " + page.dateText(b) : ""
	}
	// "version 0.3", or with withBuild "version 0.3, build 2026-10-30 01:23 UTC"
	function releaseText(v, b, withBuild) {
		return "version " + v + (withBuild && b > 0 ? ", build " + page.dateText(b) : "")
	}
	function cap(t) {
		return t.charAt(0).toUpperCase() + t.slice(1)
	}
	function sizeText(n) {
		return n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : Math.max(1, Math.round(n / 1024)) + " KB"
	}
	function askOnce() {
		if (page.bridgeUp && !page.checkAsked && !page.updBusy) {
			page.checkAsked = true
			checkItem.setValue(1)
		}
	}

	onBridgeUpChanged: askOnce()
	Component.onCompleted: {
		built = true
		askOnce()
	}

	XregUnits { id: regs }

	VeQuickItem { id: installedItem; uid: regs.mgr + "/Update/Installed" }
	// Venus OS's custom startup scripts, which start the bridge at boot (venus-platform ModificationChecks)
	VeQuickItem { id: hooksItem; uid: BackendConnection.serviceUidFromName("com.victronenergy.platform", 0) + "/ModificationChecks/SystemHooksState" }
	VeQuickItem { id: installedBuildItem; uid: regs.mgr + "/Update/InstalledBuild" }
	VeQuickItem { id: latestItem; uid: regs.mgr + "/Update/Latest" }
	VeQuickItem { id: latestBuildItem; uid: regs.mgr + "/Update/LatestBuild" }
	VeQuickItem {
		id: stateItem
		uid: regs.mgr + "/Update/State"
		onValueChanged: page.checkTapped = false
	}
	Timer {
		id: checkWait
		interval: 2000
		onTriggered: page.checkTapped = false
	}
	VeQuickItem { id: progressItem; uid: regs.mgr + "/Update/Progress" }
	VeQuickItem { id: sizeItem; uid: regs.mgr + "/Update/Size" }
	VeQuickItem { id: messageItem; uid: regs.mgr + "/Update/Message" }
	VeQuickItem { id: lastItem; uid: regs.mgr + "/Update/Last" }
	VeQuickItem { id: checkItem; uid: regs.mgr + "/Update/Check" }
	VeQuickItem { id: installItem; uid: regs.mgr + "/Update/Install" }
	VeQuickItem { id: removeItem; uid: regs.mgr + "/Update/Remove" }

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
				text: "Bridge not running"
				caption: !page.built ? " " : page.hooksOff ? "Custom startup scripts are disabled on this GX, so the bridge "
						+ "does not start after a restart. Re-enable them in Settings > General > Support status." : ""
				preferredVisible: !page.bridgeUp
			}
			ListText {
				text: "Installed version"
				secondaryText: page.installedVersion || "--"
				caption: !page.built ? " " : page.buildText(page.installedBuild)
				preferredVisible: page.bridgeUp
			}
			ListText {
				text: "Latest version"
				secondaryText: page.updState === 1 ? "Checking" : page.latestVersion || "--"
				caption: !page.built ? " " : page.updState === 1 || !page.latestVersion ? "" : page.buildText(page.latestBuild)
				preferredVisible: page.bridgeUp
			}
			ListText {
				text: "Status"
				secondaryText: (["", "Checking", "Up to date", "Update available", "Downloading", "Checking signature",
								 "Installing", "Failed", "Removing"][page.updState] || "")
						+ (page.updState === 4 && progressItem.valid ? " " + progressItem.value + " %" : "")
				caption: !page.built ? " " : page.updState === 6 ? "The app restarts. If the new version does not start, the GX puts this one back."
						: page.updState === 7 && messageItem.valid ? String(messageItem.value)
						: page.updState === 8 ? "The app's pages close once it is removed."
						: ""
				preferredVisible: page.bridgeUp && page.updState > 0
			}
			ListButton {
				text: "Check for updates"
				secondaryText: "Check"
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: page.bridgeUp
				interactive: !page.updBusy
				onClicked: {
					checkItem.setValue(1)
					page.checkTapped = true
					checkWait.restart()
				}
			}
			ListButton {
				text: "Update to " + page.releaseText(page.latestVersion, page.latestBuild, page.sameVersion)
				secondaryText: "Update"
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: page.bridgeUp && page.updOffer
				onClicked: {
					// the release the owner confirms is the one asked for
					const want = page.latestVersion + "+" + page.latestBuild
					const next = page.releaseText(page.latestVersion, page.latestBuild, page.sameVersion)
					const now = page.releaseText(page.installedVersion, page.installedBuild, page.sameVersion)
					const size = sizeItem.valid && sizeItem.value > 0 ? " (" + page.sizeText(sizeItem.value) + ")" : ""
					Global.dialogLayer.open(confirmDialog, {
						title: "Update the GX app",
						body: page.cap(next) + size + " downloads from X Engineering, its signature is checked, and it "
							  + "replaces " + now + ". The app's pages and its link to the regulator restart. If the new "
							  + "version does not start, the GX puts this one back.",
						okFn: () => installItem.setValue(want)
					})
				}
			}
			ListText {
				text: "Last update"
				secondaryText: {
					const r = page.lastResult
					if (!r)
						return ""
					const same = r.from === r.to
					const to = page.releaseText(r.to, r.toBuild, same)
					if (r.result === "ok")
						return r.from && !same ? "Version " + r.from + " to " + r.to : page.cap(to) + " installed"
					return page.cap(to) + " not installed"
				}
				caption: !page.built ? " " : page.lastResult ? Qt.formatDateTime(new Date(page.lastResult.at * 1000), "yyyy-MM-dd hh:mm")
										   + (page.lastResult.note ? ". " + page.lastResult.note : "") : ""
				preferredVisible: page.bridgeUp && !!page.lastResult
			}
			ListButton {
				text: "Remove the GX app"
				secondaryText: "Remove"
				writeAccessLevel: VenusOS.User_AccessType_User
				preferredVisible: page.bridgeUp
				interactive: !page.updBusy
				onClicked: {
					Global.dialogLayer.open(confirmDialog, {
						title: "Remove the GX app",
						body: "Removes the XREG-010 app from this GX: its pages, its switch pane controls and the bridge "
							  + "that links the GX to the regulator. The regulator itself does not change. To install the "
							  + "app again, use the USB stick or SD card.",
						okFn: () => removeItem.setValue(1)
					})
				}
			}
		}
	}
}
