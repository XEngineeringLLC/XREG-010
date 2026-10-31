import QtQuick
import QtQuick.Window
import Victron.VenusOS

// The GX side of "forget" and of an Orion that draws from the house bank: the Connection page's Power flow devices row,
// the pushed list, Forget on an absent device only, the confirmation and the write to the bridge's /GxForget; the
// "Draws from the house bank" switch on DC-DC chargers only, its write to /GxDrawsHouse, and the state the bridge
// publishes back.
Window {
	id: win
	width: pageW; height: pageH + 56; visible: true
	color: Theme.color_page_background
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	readonly property string mgr: "mock/com.victronenergy.xregulator"
	property int fails: 0
	property var dialogs: []
	function check(cond, msg) { if (cond) console.log("PASS " + msg); else { fails++; console.log("FAIL " + msg) } }
	Item { id: dialogParent; anchors.fill: parent }
	Rectangle { id: pageParent; width: pageW; height: pageH; color: Theme.color_page_background }
	QtObject {
		id: dialogLayer
		function open(comp, props) { const d = comp.createObject(dialogParent, props); win.dialogs.push(d); d.open(); return d }
	}
	QtObject {
		id: pageManager
		property var pushed: []
		function pushPage(url, props) {
			const c = Qt.createComponent(String(url).replace("qrc:/xregulator/", pluginDir + "/"))
			if (c.status !== Component.Ready) { console.log("FAIL push " + url + ": " + c.errorString()); win.fails++; return null }
			const p = c.createObject(pageParent, Object.assign({ width: pageW, height: pageH }, props))
			pushed.push(p)
			return p
		}
		function popPage() {}
	}
	function find(root, pred, out) {
		for (const c of root.children) { if (pred(c) && out.indexOf(c) < 0) out.push(c); find(c, pred, out) }
		if (root.contentItem && root.contentItem !== root) find(root.contentItem, pred, out)
		return out
	}
	function switches() { return find(list, x => x.text === "Draws from the house bank" && x.checked !== undefined, []) }
	function shown(x) { return x.visible && x.height > 0 }
	readonly property var devs: [
		{ key: "charger:280", k: "charger", n: "", p: "Phoenix Smart IP43 Charger 12/30", on: 0 },
		{ key: "dcdc:293", k: "dcdc", n: "Start charger", p: "Orion XS 12/12-50A Charger", on: 1, h: 0 },
		{ key: "dcdc:294", k: "dcdc", n: "", p: "Orion XS 12/24-20A Charger", on: 0, h: 1 },
		{ key: "dcload:279", k: "dcload", n: "Fridge", p: "SmartShunt 500A/50mV", on: 1 },
		{ key: "inverter:281", k: "inverter", n: "Galley", p: "Phoenix Inverter 12/800 Smart", on: 1 },
		// a SmartShunt in DC-DC mode: the bridge sends no "h", its sign decides (2026-10-29)
		{ key: "dcsource:300", k: "dcdc", n: "", p: "SmartShunt 500A/50mV", on: 1 }]
	property var steps: []
	Timer { interval: 150; repeat: true; running: true; onTriggered: { if (!win.steps.length) { console.log("DONE fails=" + win.fails); Qt.quit(); return } win.steps.shift()() } }
	property var conn: null
	property var list: null
	Component.onCompleted: {
		Global.dialogLayer = dialogLayer
		Global.pageManager = pageManager
		MockStore.put(mgr + "/Units", "[]")
		MockStore.put(svc + "/Connected", 1)
		MockStore.put(mgr + "/GxDevices", JSON.stringify(devs))
		steps.push(() => {
			const c = Qt.createComponent(pluginDir + "/XregCfgConnection.qml")
			if (c.status !== Component.Ready) { console.log("FAIL load " + c.errorString()); fails++; return }
			conn = c.createObject(pageParent, { svc: svc, width: pageW, height: pageH })
		})
		steps.push(() => {
			const rows = find(conn, x => x.text === "Power flow devices" && x.preferredVisible !== undefined && x.secondaryText !== undefined, [])
			check(rows.length === 1 && rows[0].secondaryText === "2 not on this GX now", "Connection row: " + (rows[0] ? rows[0].secondaryText : "none"))
			rows[0].clicked()
		})
		steps.push(() => {
			conn.visible = false
			list = pageManager.pushed[pageManager.pushed.length - 1]
			check(!!list && list.devices.length === 6, "pushed page lists the remembered devices")
			const h0 = find(list, x => x.text === "Devices the power flow lists" && x.caption !== undefined, [])[0]
			const cap0 = h0 ? find(h0, x => x.wrapMode !== undefined && x.text === " ", [])[0] : null
			console.log("INFO header height " + (h0 ? h0.height : -1) + ", its blank caption " + (cap0 ? cap0.height : -1))
		})
		steps.push(() => {
			const btns = find(list, x => x.secondaryText === "Forget", [])
			check(btns.length === 6, "a Forget row per device: " + btns.length)
			const charger = btns.find(b => b.text === "Phoenix Smart IP43 Charger 12/30")
			const inv = btns.find(b => b.text === "Galley")
			const orion = btns.find(b => b.text === "Start charger")
			const orion2 = btns.find(b => b.text === "Orion XS 12/24-20A Charger")
			const fridge = btns.find(b => b.text === "Fridge")
			check(!!charger && charger.enabled && charger.caption.indexOf("not on this GX now") >= 0, "absent charger can be forgotten: " + (charger ? charger.caption : ""))
			check(!!inv && !inv.enabled && inv.caption.indexOf("Inverter · Phoenix Inverter 12/800 Smart · on this GX now") === 0, "present inverter cannot: " + (inv ? inv.caption : ""))
			check(!!orion && !orion.enabled && !!orion2 && orion2.enabled, "Forget unchanged for DC-DC chargers: present no, absent yes")
			check(!!fridge && fridge.caption.indexOf("DC load · SmartShunt 500A/50mV") === 0, "DC load named: " + (fridge ? fridge.caption : ""))
			charger.clicked()
		})
		steps.push(() => {
			check(dialogs.length === 1 && dialogs[0].title === "Forget device", "asks first")
			dialogs[0].okFn()
			dialogs[0].visible = false
			const w = MockStore.writes.filter(x => x.uid === mgr + "/GxForget")
			check(w.length === 1 && w[0].value === "charger:280", "writes /GxForget charger:280")
		})
		steps.push(() => {
			// the switch: on the two DC-DC chargers only, present or not, showing the bridge's "h"
			const sw = switches()
			const vis = sw.filter(shown)
			check(sw.length === 6 && vis.length === 2, "switch rows shown for Orions only, not the DC meter in DC-DC mode: " + vis.length + " of " + sw.length)
			const names = vis.map(s => { const b = find(s.parent, x => x.secondaryText === "Forget", [])[0]; return (b ? b.text : "?") + "=" + s.checked })
			check(names.join(",") === "Start charger=false,Orion XS 12/24-20A Charger=true", "switch state: " + names.join(","))
			check(vis.every(s => s.caption.indexOf("For an Orion that charges another battery from the house bank.") >= 0), "switch description")
			check(vis.every(s => s.interactive && s.writeAccessLevel === VenusOS.User_AccessType_User), "switches are live, user level")
			if (renderFile.length) { list.grabToImage(r => r.saveToFile(renderFile)); console.log("INFO render " + renderFile) }
			// on: writes the bridge path, and the switch waits for the bridge's answer
			vis[0].click()
		})
		steps.push(() => {
			const w = MockStore.writes.filter(x => x.uid === mgr + "/GxDrawsHouse")
			check(w.length === 1 && w[0].value === JSON.stringify({ key: "dcdc:293", on: 1 }), "writes /GxDrawsHouse " + (w[0] ? w[0].value : "nothing"))
			// (2026-10-29, toggle lane) the switch shows the state asked for until the bridge answers, not the old one
			check(switches().filter(shown)[0].checked === true, "switch shows the state asked for while the bridge has not answered")
			// the bridge's answer: /GxDevices with h 1, and the write path back to ''
			const after = devs.map(d => d.key === "dcdc:293" ? Object.assign({}, d, { h: 1 }) : d)
			MockStore.put(mgr + "/GxDevices", JSON.stringify(after))
			MockStore.put(mgr + "/GxDrawsHouse", "")
		})
		steps.push(() => {
			const vis = switches().filter(shown)
			check(vis.length === 2 && vis[0].checked && vis[1].checked, "after the reload both show on")
			if (renderFile.length) list.grabToImage(r => r.saveToFile(renderFile.replace(".png", "_on.png")))
			// off on the absent one
			vis[1].click()
		})
		steps.push(() => {
			const w = MockStore.writes.filter(x => x.uid === mgr + "/GxDrawsHouse")
			check(w.length === 2 && w[1].value === JSON.stringify({ key: "dcdc:294", on: 0 }), "absent one switched off: " + (w[1] ? w[1].value : "nothing"))
			// (2026-10-29) a DC system meter on the GX: the bridge adds "m" on each row device and the pinned regulators
			const withMeter = [{ key: "xreg:0000FEEDC0DE1234", k: "alt", n: "Port engine", p: "XREG-010", on: 1, reg: 1, m: 0 }]
				.concat(devs.map(d => Object.assign({}, d, { m: d.k === "dcload" ? 1 : 0 })))
			MockStore.put(mgr + "/GxDevices", JSON.stringify(withMeter))
			// every row built, not only those in view: the page now runs past the window
			const lv = find(list, x => x.cacheBuffer !== undefined, [])[0]
			if (lv) lv.cacheBuffer = 100000
		})
		steps.push(() => {
			const ms = find(list, x => x.text === "Wired through the DC system meter" && x.checked !== undefined, [])
			const vis = ms.filter(shown)
			check(vis.length === 7, "meter switch on every row device and the regulator: " + vis.length + " of " + ms.length)
			const reg = find(list, x => x.text === "Port engine" && x.caption !== undefined, []).filter(shown)
			const regForget = reg.length ? find(reg[0].parent, x => x.secondaryText === "Forget", []).filter(shown) : []
			check(reg.length === 1 && reg[0].caption === "Alternator · XREG-010" && regForget.length === 0,
				  "the regulator's row: no Forget (" + (reg[0] ? reg[0].caption : "none") + ")")
			const forgets = find(list, x => x.secondaryText === "Forget", []).filter(shown)
			check(forgets.length === 6, "Forget rows for the GX devices only: " + forgets.length)
			check(vis.map(s => s.checked).join(",") === "false,false,false,false,true,false,false", "meter switch states: " + vis.map(s => s.checked).join(","))
			check(vis.every(s => s.caption.indexOf("passes through the DC system meter's shunt") > 0 && s.interactive), "meter switch description, live")
			const conn2 = find(conn, x => x.text === "Power flow devices" && x.secondaryText !== undefined, [])[0]
			check(!!conn2 && conn2.secondaryText === "2 not on this GX now", "the regulator row is no absent device: " + (conn2 ? conn2.secondaryText : ""))
			vis[0].click()
		})
		steps.push(() => {
			const w = MockStore.writes.filter(x => x.uid === mgr + "/GxMetered")
			check(w.length === 1 && w[0].value === JSON.stringify({ key: "xreg:0000FEEDC0DE1234", on: 1 }), "writes /GxMetered " + (w[0] ? w[0].value : "nothing"))
			MockStore.put(mgr + "/GxDevices", "[]")
		})
		steps.push(() => {
			// the last device forgotten with the page open: the empty state, its caption, and no binding loop (devices_run
			// counts Qt warnings)
			const hdr = list.children.length ? find(list, x => x.text === "No devices remembered yet" && x.caption !== undefined, []) : []
			check(hdr.length === 1 && hdr[0].caption.indexOf("is remembered the first time") > 0, "empty list: header and its caption")
			if (renderFile.length) list.grabToImage(r => r.saveToFile(renderFile.replace(".png", "_empty.png")))
		})
		steps.push(() => {
			conn.visible = true
			const rows = find(conn, x => x.text === "Power flow devices" && x.preferredVisible !== undefined && x.secondaryText !== undefined, [])
			check(rows.length <= 1 && rows.every(r => r.preferredVisible === false), "no remembered devices: the row hides (" + rows.length + ")")
		})
	}
}
