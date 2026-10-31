import QtQuick
import QtQuick.Window
import Victron.VenusOS

Window {
	id: win
	width: pageW; height: pageH; visible: true
	readonly property string mgr: "mock/com.victronenergy.xregulator"
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	property int fails: 0
	property var openDialogs: []
	property var page: null

	function check(cond, msg) { if (cond) console.log("PASS " + layoutName + ": " + msg); else { fails++; console.log("FAIL " + layoutName + ": " + msg) } }
	function writes(path) { return MockStore.writes.filter(w => w.uid === mgr + path) }
	function put(path, v) { MockStore.put(mgr + path, v) }
	readonly property string hooksUid: "mock/com.victronenergy.platform/ModificationChecks/SystemHooksState"
	readonly property string hooksText: "Custom startup scripts are disabled on this GX, so the bridge does not start after a restart. Re-enable them in Settings > General > Support status."
	property var settingsPage: null
	function settingsHeader() {
		return findAll(settingsPage, x => x.caption !== undefined
					   && ["Bridge not running", "No regulator on this GX yet", "Regulators on this GX"].indexOf(x.text) >= 0)[0] || null
	}

	Item { id: dialogParent; anchors.fill: parent }
	Item { id: stage; width: pageW; height: 3000 }
	Item { id: homeParent; width: pageW; height: pageH }
	Page { id: hostPage; title: "XREG-010" }

	QtObject {
		id: dialogLayer
		function open(comp, props) {
			const d = comp.createObject(dialogParent, props)
			if (!d) { console.log("FAIL dialog create: " + comp.errorString()); fails++; return null }
			d.closed.connect(function() { const i = win.openDialogs.indexOf(d); if (i >= 0) win.openDialogs.splice(i, 1); d.destroy() })
			win.openDialogs.push(d)
			d.open()
			return d
		}
	}
	QtObject {
		id: pageManager
		property var pushed: []
		function pushPage(url, props) {
			const u = String(url).replace("qrc:/xregulator/", pluginDir + "/")
			props = props || {}
			props.width = pageW
			props.height = 3000
			const c = Qt.createComponent(u)
			if (c.status !== Component.Ready) { console.log("FAIL push " + url + ": " + c.errorString()); win.fails++; return null }
			const p = c.createObject(stage, props)
			pushed.push({ url: String(url), props: props, page: p })
			return p
		}
	}

	function findAll(root, pred, out) {
		out = out || []
		for (const c of root.children) { if (pred(c) && out.indexOf(c) < 0) out.push(c); findAll(c, pred, out) }
		if (root.contentItem && root.contentItem !== root) { if (pred(root.contentItem) && out.indexOf(root.contentItem) < 0) out.push(root.contentItem); findAll(root.contentItem, pred, out) }
		return out
	}
	function rowsOf(p) {
		return findAll(p, x => x.preferredVisible !== undefined && x.text !== undefined && x.visible && x.height > 0)
			.sort((a, b) => a.mapToItem(null, 0, 0).y - b.mapToItem(null, 0, 0).y)
	}
	function rows() { return rowsOf(page) }
	function row(t) { return rows().find(r => r.text === t) || null }
	function names() { return rows().map(r => r.text).join(" | ") }
	function geometry(tag) {
		let bad = []
		for (const r of rows()) {
			const p = r.mapToItem(win.contentItem, 0, 0)
			if (p.x < -0.5 || p.x + r.width > pageW + 0.5) bad.push("row '" + r.text + "' x " + p.x + " w " + r.width)
			const labels = findAll(r, x => x.contentWidth !== undefined && x.truncated !== undefined && x.visible && x.text && x.width > 0)
			for (const l of labels) {
				const q = l.mapToItem(win.contentItem, 0, 0)
				if (q.x < p.x - 0.5 || q.x + l.width > p.x + r.width + 0.5 || l.truncated || l.contentWidth > l.width + 1)
					bad.push("'" + String(l.text).slice(0, 30) + "' x " + Math.round(q.x) + " w " + Math.round(l.width) + " content " + Math.round(l.contentWidth))
			}
		}
		check(bad.length === 0, tag + ": every row and text inside the page width " + pageW + (bad.length ? " (" + bad.join("; ") + ")" : ""))
	}
	function acceptTop() { const d = openDialogs[openDialogs.length - 1]; if (!d) return false; d.accept(); return true }

	Timer { id: driver; interval: 40; repeat: true; running: false; onTriggered: win.stepOnce() }
	property var queue: []
	function stepOnce() {
		if (queue.length === 0) { driver.stop(); console.log("DONE fails=" + fails); Qt.callLater(Qt.quit); return }
		const f = queue.shift()
		try { f() } catch (e) { fails++; console.log("FAIL exception " + e + "\n" + e.stack) }
	}
	function later() { for (const f of arguments) queue.push(f) }

	Component.onCompleted: {
		Global.dialogLayer = dialogLayer
		Global.pageManager = pageManager
		page = pageManager.pushPage("qrc:/xregulator/XregGxApp.qml", { title: "GX app" })
		later(() => {
			check(names() === "Bridge not running" && writes("/Update/Check").length === 0,
				  "no bridge: only 'Bridge not running', no check asked (" + names() + ")")
			check(row("Bridge not running").caption === "", "no bridge, startup-script state unknown: no caption")
			settingsPage = pageManager.pushPage("qrc:/xregulator/XregSettings.qml", { title: "XREG-010" })
			MockStore.put(hooksUid, 1)
		}, () => {
			// rc.local.disabled and no rc.local
			check(row("Bridge not running").caption === hooksText, "startup scripts disabled: the page says why (" + row("Bridge not running").caption + ")")
			const h = settingsHeader()
			check(!!h && h.caption === hooksText, "startup scripts disabled: Settings > Integrations > XREG-010 says why")
			geometry("startup scripts disabled")
			MockStore.put(hooksUid, 5)
		}, () => {
			// rc.local back beside the .disabled copy: Venus runs rc.local
			check(row("Bridge not running").caption === "" && settingsHeader().caption === "", "rc.local there (5): no caption")
			MockStore.put(hooksUid, 20)
		}, () => {
			check(row("Bridge not running").caption === "" && settingsHeader().caption === "", "rc.local loaded at startup (20): no caption")
			MockStore.put(hooksUid, 1)
			put("/Units", "[]")
			put("/Update/State", 0); put("/Update/Latest", ""); put("/Update/Last", ""); put("/Update/Progress", 0)
			put("/Update/Size", 0); put("/Update/Message", ""); put("/Update/InstalledBuild", 1761800000); put("/Update/LatestBuild", 0)
			put("/Update/Installed", "0.3")
		}, () => {
			check(writes("/Update/Check").length === 1 && writes("/Update/Check")[0].value === 1,
				  "bridge up: the page asks for one check (" + writes("/Update/Check").length + ")")
			check(names() === "Installed version | Latest version | Check for updates | Remove the GX app", "idle rows: " + names())
			check(settingsHeader().text === "No regulator on this GX yet" && settingsHeader().caption === "",
				  "bridge up, startup scripts still disabled: Settings shows no caption (" + settingsHeader().text + ")")
			check(row("Installed version").secondaryText === "0.3" && row("Latest version").secondaryText === "--"
				  && row("Installed version").caption === "Build 2025-10-30 04:53 UTC" && row("Latest version").caption === "",
				  "idle: 0.3 installed (" + row("Installed version").caption + "), latest --")
			put("/Update/State", 1)
		}, () => {
			check(row("Latest version").secondaryText === "Checking" && row("Status").secondaryText === "Checking"
				  && !row("Check for updates").interactive && !row("Remove the GX app").interactive, "checking: Checking, buttons off")
			put("/Update/Latest", "0.4"); put("/Update/LatestBuild", 1761900000); put("/Update/Size", 163840); put("/Update/State", 3)
		}, () => {
			const u = row("Update to version 0.4")
			check(!!u && row("Status").secondaryText === "Update available" && row("Check for updates").interactive,
				  "available: Update to version 0.4 shown (" + names() + ")")
			geometry("available")
			u.click()
		}, () => {
			const d = openDialogs[0]
			check(openDialogs.length === 1 && d.title === "Update the GX app" && d.body.indexOf("Version 0.4 (160 KB)") === 0
				  && d.body.indexOf("replaces version 0.3") > 0 && writes("/Update/Install").length === 0,
				  "Update asks first, size and versions in the question, nothing written yet")
			acceptTop()
		}, () => {}, () => {
			check(writes("/Update/Install").length === 1 && writes("/Update/Install")[0].value === "0.4+1761900000",
				  "confirmed: writes /Update/Install 0.4+1761900000 (" + writes("/Update/Install")[0].value + ")")
			put("/Update/Latest", "0.3"); put("/Update/LatestBuild", 1761800000)
		}, () => {
			check(!names().includes("Update to"), "the same release as installed: no Update row")
			put("/Update/LatestBuild", 1761700000)
		}, () => {
			check(!names().includes("Update to"), "an older build of the installed version: no Update row")
			put("/Update/LatestBuild", 1761886400)
		}, () => {
			const u = row("Update to version 0.3, build 2025-10-31 04:53 UTC")
			check(!!u, "the installed version issued again with a newer build: offered with its build (" + names() + ")")
			geometry("reissue")
			u.click()
		}, () => {
			const d = openDialogs[0]
			check(openDialogs.length === 1 && d.body.indexOf("Version 0.3, build 2025-10-31 04:53 UTC (160 KB) downloads") === 0
				  && d.body.indexOf("replaces version 0.3, build 2025-10-30 04:53 UTC.") > 0, "reissue: the question names both builds")
			acceptTop()
		}, () => {}, () => {
			check(writes("/Update/Install").length === 2 && writes("/Update/Install")[1].value === "0.3+1761886400",
				  "reissue confirmed: writes /Update/Install 0.3+1761886400")
			put("/Update/LatestBuild", 0)
			put("/Update/Latest", "0.10")
		}, () => {
			check(!!row("Update to version 0.10"), "0.10 is newer than 0.3 (field by field)")
			// builds past 2038 (2^31 s): a QML int would wrap the newer one negative
			put("/Update/Latest", "0.3"); put("/Update/InstalledBuild", 2147483647); put("/Update/LatestBuild", 2147483648)
		}, () => {
			const u = row("Update to version 0.3, build 2038-01-19 03:14 UTC")
			check(!!u && row("Installed version").caption === "Build 2038-01-19 03:14 UTC",
				  "2038: build 2147483648 is newer than 2147483647 (" + names() + ")")
			u.click()
		}, () => {
			acceptTop()
		}, () => {}, () => {
			const w = writes("/Update/Install")
			check(w.length === 3 && w[2].value === "0.3+2147483648", "2038: writes /Update/Install 0.3+2147483648 (" + (w[2] ? w[2].value : "-") + ")")
			put("/Update/InstalledBuild", 1761800000)
			put("/Update/Latest", "0.4"); put("/Update/LatestBuild", 1761900000); put("/Update/State", 4); put("/Update/Progress", 45)
		}, () => {
			check(row("Status").secondaryText === "Downloading 45 %" && !names().includes("Update to")
				  && !row("Check for updates").interactive && !row("Remove the GX app").interactive, "downloading: 45 %, nothing to tap")
			put("/Update/State", 5)
		}, () => {
			check(row("Status").secondaryText === "Checking signature", "verifying: Checking signature")
			put("/Update/State", 6)
		}, () => {
			check(row("Status").secondaryText === "Installing" && row("Status").caption.indexOf("puts this one back") > 0, "installing: caption says the old version comes back")
			geometry("installing")
			put("/Update/Message", "The package signature does not match. Nothing was installed."); put("/Update/State", 7)
		}, () => {
			check(row("Status").secondaryText === "Failed" && row("Status").caption === "The package signature does not match. Nothing was installed."
				  && !!row("Update to version 0.4"), "failed: message shown, Update offered again")
			geometry("failed")
			put("/Update/Last", JSON.stringify({ result: "ok", to: "0.4", toBuild: 1761900000, from: "0.3", fromBuild: 1761800000, at: 1761780000, note: "" })); put("/Update/State", 2)
		}, () => {
			const l = row("Last update")
			check(!!l && l.secondaryText === "Version 0.3 to 0.4" && /^\d{4}-\d\d-\d\d \d\d:\d\d$/.test(l.caption)
				  && row("Status").secondaryText === "Up to date" && !names().includes("Update to"), "last ok: Version 0.3 to 0.4, " + (l ? l.caption : "?"))
			put("/Update/Last", JSON.stringify({ result: "rolledback", to: "0.5", toBuild: 1761900000, from: "0.4", fromBuild: 1761800000,
				at: 1761780000, note: "The new version did not start within 600 s. The previous version is back and running." }))
		}, () => {
			const l = row("Last update")
			check(!!l && l.secondaryText === "Version 0.5 not installed" && l.caption.endsWith(". The new version did not start within 600 s. The previous version is back and running."),
				  "last rolled back: not installed, the reason under it")
			geometry("rolled back")
			put("/Update/Last", JSON.stringify({ result: "ok", to: "0.3", toBuild: 1761800000, from: "", fromBuild: 0, at: 1761780000,
				note: "Custom startup scripts are disabled on this GX, so the app will not start after a restart until they are re-enabled in Settings > General > Support status." }))
		}, () => {
			check(row("Last update").secondaryText === "Version 0.3 installed" && row("Last update").caption.indexOf("Custom startup scripts are disabled") > 0,
				  "first install: Version 0.3 installed, the startup-scripts note under it")
			geometry("startup scripts note")
			put("/Update/Last", JSON.stringify({ result: "ok", to: "0.3", toBuild: 1761886400, from: "0.3", fromBuild: 1761800000, at: 1761780000, note: "" }))
		}, () => {
			check(row("Last update").secondaryText === "Version 0.3, build 2025-10-31 04:53 UTC installed", "reissue installed: " + row("Last update").secondaryText)
			row("Remove the GX app").click()
		}, () => {
			const d = openDialogs[0]
			check(openDialogs.length === 1 && d.title === "Remove the GX app" && writes("/Update/Remove").length === 0, "Remove asks first")
			acceptTop()
		}, () => {}, () => {
			check(writes("/Update/Remove").length === 1 && writes("/Update/Remove")[0].value === 1, "confirmed: writes /Update/Remove 1")
			check(writes("/Update/Check").length === 1, "still one check in all (" + writes("/Update/Check").length + ")")
			// the Connection page's row
			MockStore.put(svc + "/Connected", 1)
			MockStore.put(svc + "/Serial", "0000FEEDC0DE1234")
			const c = pageManager.pushPage("qrc:/xregulator/XregCfgConnection.qml", { title: "Connection", svc: svc, setupId: "system" })
			const r = rowsOf(c).find(x => x.text === "GX app")
			check(!!r && r.secondaryText === "Version 0.3", "Connection page: GX app row, Version 0.3")
			const n = pageManager.pushed.length
			r.click()
			const p = pageManager.pushed[n]
			check(!!p && p.url === "qrc:/xregulator/XregGxApp.qml" && p.props.svc === svc && p.props.setupId === "system" && !!p.page,
				  "Connection page: GX app opens XregGxApp with the regulator's svc and sub-tab")
			page = c
			geometry("connection page")
			// Settings > Integrations with no regulator on the GX
			const s = pageManager.pushPage("qrc:/xregulator/XregSettings.qml", { title: "XREG-010" })
			page = s
		}, () => {
			const g = row("GX app")
			check(!!row("Regulators") && !!g && g.secondaryText === "Version 0.3", "Integrations, no regulator: Regulators and GX app rows (" + names() + ")")
			geometry("integrations")
			const n = pageManager.pushed.length
			g.click()
			check(pageManager.pushed.length === n + 1 && pageManager.pushed[n].url === "qrc:/xregulator/XregGxApp.qml", "Integrations: GX app opens XregGxApp")
			// from a regulator's home page (Settings > System > Connection > GX app): the home page's header on top
			MockStore.put(svc + "/Settings/Schema", schemaText)
			const hc = Qt.createComponent(pluginDir + "/XregHome.qml")
			const home = hc.createObject(homeParent, { svc: svc, animationEnabled: false, hostPage: hostPage, width: pageW, height: pageH })
			check(!!home, "home page created: " + hc.errorString())
			page = pageManager.pushPage("qrc:/xregulator/XregGxApp.qml", { title: "GX app", svc: svc, home: home, setupId: "system" })
		}, () => {}, () => {
			const hdr = findAll(page, x => x.setupTabs !== undefined && x.pick !== undefined)[0]
			const list = findAll(page, x => x.model !== undefined && x.contentY !== undefined)[0]
			check(!!hdr && !!list && list.y >= hdr.height - 0.5 && hdr.width === pageW,
				  "with home: header " + (hdr ? Math.round(hdr.height) : "?") + " px over the list at y " + (list ? Math.round(list.y) : "?"))
			geometry("with home header")
		})
		driver.running = true
	}
}
