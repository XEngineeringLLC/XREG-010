import QtQuick
import QtQuick.Window
import QtQuick.Templates as T
import Victron.VenusOS

// Fixtures for toggle_test.py, which drives real mouse and touch input through this offscreen window: the header's
// Alternator switch, a settings toggle row, "Draws from the house bank", "Show descriptions", the Long Term range row,
// and a bare Templates Switch for the Qt mechanism itself.
Window {
	id: win
	width: 1024; height: 600; visible: true
	color: "black"
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	readonly property string mgr: "mock/com.victronenergy.xregulator"
	property var fixture: null
	property string kind
	property string uid
	property var row: null          // the list row (rows only)
	property var dialogs: []
	property int drawsEchoMs: -1    // -1: the bridge never answers a /GxDrawsHouse write; else it answers after this long
	property string orionKey: "dcdc:293"
	property var drawsDevs: []
	property int drawsExtra           // more remembered devices under the Orion, so the list scrolls
	property real pageH: 600
	property int helpWrites          // "Show descriptions" is local: its writes are changes of the page's showHelp

	QtObject { id: pm; function pushPage() { return null } function popPage() {} }
	QtObject {
		id: dl
		function open(comp, props) { const d = comp.createObject(win.contentItem, props); win.dialogs.push(d); d.open(); return d }
	}
	Item { id: holder; anchors.fill: parent }

	// The bridge answering a /GxDrawsHouse write by republishing /GxDevices with the new "h" (its 500 ms tick)
	Timer {
		id: drawsEcho
		property int h
		onTriggered: win.putDraws(h)
	}
	Connections {
		target: MockStore
		function onChanged(u) {
			if (u === win.mgr + "/GxDrawsHouse" && win.drawsEchoMs >= 0) {
				const req = JSON.parse(MockStore.get(u))
				drawsEcho.h = req.on
				drawsEcho.interval = Math.max(1, win.drawsEchoMs)
				drawsEcho.restart()
			}
		}
	}

	Component.onCompleted: { Global.pageManager = pm; Global.dialogLayer = dl }

	function find(root, pred) {
		for (const c of root.children) { if (pred(c)) return c; const f = find(c, pred); if (f) return f }
		if (root.contentItem && root.contentItem !== root && root.contentItem.children) { const f = find(root.contentItem, pred); if (f) return f }
		return null
	}
	function isSwitch(x) { return x.position !== undefined && x.toggle !== undefined && x.checkable !== undefined }
	function putDraws(h) {
		drawsDevs = [{ key: orionKey, k: "dcdc", n: "Start charger", p: "Orion XS 12/12-50A Charger", on: 1, h: h }]
		for (let i = 0; i < drawsExtra; i++)
			drawsDevs.push({ key: "charger:" + (300 + i), k: "charger", n: "", p: "Phoenix Smart IP43 Charger 12/30", on: 0 })
		MockStore.put(mgr + "/GxDevices", JSON.stringify(drawsDevs))
	}
	function clear() {
		if (fixture) fixture.destroy()
		fixture = null
		row = null
		for (const d of dialogs) d.destroy()
		dialogs = []
		drawsEcho.stop()
		drawsEchoMs = -1
		MockStore.reset()
	}
	function make(file, props) {
		const c = Qt.createComponent(pluginDir + "/" + file)
		if (c.status !== Component.Ready) { console.log("FAIL load " + file + ": " + c.errorString()); return null }
		return c.createObject(holder, props)
	}
	// kind: header | row | confirmrow | draws | help | bare; initial: the device's state (0/1)
	// scroll: a page shorter than its list, so a vertical drag from the switch scrolls it
	function setup(k, initial, scroll) {
		clear()
		kind = k
		drawsExtra = scroll && k === "draws" ? 10 : 0
		pageH = scroll && (k === "row" || k === "help") ? 200 : 600
		MockStore.put(svc + "/Connected", 1)
		if (k === "header") {
			uid = svc + "/SwitchableOutput/0/State"
			MockStore.put(uid, initial)
			fixture = make("XregHeader.qml", { svc: svc, width: 1024 })
		} else if (k === "row" || k === "confirmrow") {
			uid = svc + "/Settings/Value/IgnitionOverride"
			MockStore.put(svc + "/Settings/Schema", k === "row" ? schemaText : confirmSchemaText)
			MockStore.put(uid, initial)
			fixture = make("XregCfgSection.qml", { svc: svc, nodeId: "engine", width: 1024, height: pageH })
			row = find(fixture, x => x.text === "Ignition Override" && x.checked !== undefined)
		} else if (k === "draws") {
			uid = mgr + "/GxDrawsHouse"
			drawsEchoMs = 0
			MockStore.put(mgr + "/Units", "[]")
			putDraws(initial)
			fixture = make("XregFlowDevices.qml", { svc: svc, width: 1024, height: 600 })
		} else if (k === "help") {
			uid = ""
			MockStore.put(svc + "/Settings/Schema", schemaText)
			fixture = make("XregCfgSection.qml", { svc: svc, nodeId: "engine", width: 1024, height: pageH })
			fixture.showHelp = initial === 1
			helpWrites = 0
			fixture.showHelpChanged.connect(() => win.helpWrites++)
			row = find(fixture, x => x.text === "Show descriptions" && x.checked !== undefined)
		} else if (k === "bare") {
			uid = ""
			fixture = Qt.createQmlObject('import QtQuick; import Victron.VenusOS; Item { property int v: ' + initial + '; property int clicks; '
					+ 'Switch { id: s; objectName: "bare"; x: 400; y: 280; scale: 1.5; checkable: false; checked: parent.v === 1; onClicked: parent.clicks++ } }', holder)
		}
		return !!fixture
	}
	function rowFor() {
		if (kind === "draws")
			return find(fixture, x => x.text === "Draws from the house bank" && x.checked !== undefined && x.visible)
		return row
	}
	function sw() { return kind === "header" || kind === "bare" ? find(fixture, x => isSwitch(x) && (x.scale > 1)) : (rowFor() ? find(rowFor(), isSwitch) : null) }
	// window points: the knob's centre, the groove's two ends (2 px inside) and its centre
	function pt(where) {
		const s = sw()
		if (!s) return Qt.point(-1, -1)
		const ind = s.indicator, bg = s.background
		if (where === "knob") return ind.mapToItem(null, ind.width / 2, ind.height / 2)
		if (where === "left") return bg.mapToItem(null, 2, bg.height / 2)
		if (where === "right") return bg.mapToItem(null, bg.width - 2, bg.height / 2)
		if (where === "center") return bg.mapToItem(null, bg.width / 2, bg.height / 2)
		if (where === "bottom") return s.mapToItem(null, s.width / 2, s.height + 4)   // just below the control
		return Qt.point(-1, -1)
	}
	// What the switch shows: its checked state and where its knob is
	function shown() { const s = sw(); return s ? (s.checked ? 1 : 0) : -1 }
	function knobOn() { const s = sw(); return s ? (s.indicator.x + s.indicator.width / 2 > s.background.x + s.background.width / 2 ? 1 : 0) : -1 }
	function rowChecked() { const r = rowFor(); return r ? (r.checked ? 1 : 0) : -1 }
	function device() {
		if (kind === "help") return fixture.showHelp ? 1 : 0
		if (kind === "draws") return drawsDevs.length ? drawsDevs[0].h : -1
		if (kind === "bare") return fixture.v
		const v = MockStore.get(uid)
		return v === undefined ? -1 : Number(v)
	}
	function writeCount() {
		if (kind === "bare") return fixture.clicks
		if (kind === "help") return helpWrites
		return MockStore.writes.filter(w => w.uid === uid).length
	}
	// the state the last write asked for: 0/1, or -1 for none
	function lastSent() {
		if (kind === "help") return helpWrites ? (fixture.showHelp ? 1 : 0) : -1
		const w = MockStore.writes.filter(x => x.uid === uid)
		if (!w.length) return -1
		const v = w[w.length - 1].value
		return kind === "draws" ? JSON.parse(v).on : Number(v)
	}
	function setNoEcho(on) { const n = Object.assign({}, MockStore.noEcho); n[uid] = on; MockStore.noEcho = n }
	function putDevice(v) {
		if (kind === "draws") putDraws(v)
		else if (kind === "help") fixture.showHelp = v === 1
		else if (kind === "bare") fixture.v = v
		else MockStore.put(uid, v)
	}
	function setDrawsEcho(ms) { drawsEchoMs = ms }
	function dialogCount() { return dialogs.filter(d => d.visible).length }
	function rejectTop() { const d = dialogs.filter(x => x.visible).pop(); if (d) d.reject() }
	function acceptTop() { const d = dialogs.filter(x => x.visible).pop(); if (d) d.accept() }
	function list() { return find(fixture, x => x.contentY !== undefined && x.flickableDirection !== undefined && x.contentHeight > x.height) }
	function listY() { const l = list(); return l ? l.contentY : -1 }
	// brings the row into view in a list that scrolls; the finger direction that scrolls it: -1 up, 1 down, 0 none
	function scrollToRow() {
		const l = list(), r = rowFor()
		if (!l || !r) return 0
		const y = r.mapToItem(l.contentItem, 0, 0).y
		l.contentY = Math.max(0, Math.min(l.contentHeight - l.height, y - 10))
		return l.contentY < l.contentHeight - l.height - 1 ? -1 : l.contentY > 1 ? 1 : 0
	}
	function cancelLink() { MockStore.put(svc + "/Connected", 0) }
	function restoreLink() { MockStore.put(svc + "/Connected", 1) }
	// click() from code (gui-v2 calls it for the space key), and keyboard focus for a real space key
	function codeClick() { if (kind === "header") sw().click(); else rowFor().click() }
	function focusForKeys() { if (kind === "header") sw().forceActiveFocus(); else rowFor().forceActiveFocus(); return (kind === "header" ? sw() : rowFor()).activeFocus }
	function swPressed() { const s = sw(); return s ? s.pressed : false }
	function position() { const s = sw(); return s ? s.position : -1 }

	// Long Term range row
	function setupRange(range) {
		clear()
		kind = "range"
		uid = svc + "/Ui/LongRange"
		MockStore.put(uid, range)
		fixture = make("XregPlotsLong.qml", { svc: svc, width: 1024, height: 544, toolX: 14, toolY: 8, bodyY: 58 })
		return !!fixture
	}
	function rangeRow() { return find(fixture, x => x.buttonClicked !== undefined && x.currentIndex !== undefined && x.model && x.model.length === 5) }
	function rangeIndex() { const r = rangeRow(); return r ? r.currentIndex : -2 }
	function rangePt(i) { const r = rangeRow(); return r.mapToItem(null, r.width * (i + 0.5) / 5, r.height / 2) }
	function lastRange() { const w = MockStore.writes.filter(x => x.uid === uid); return w.length ? w[w.length - 1].value : "" }
}
