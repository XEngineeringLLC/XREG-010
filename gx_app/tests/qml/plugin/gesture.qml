import QtQuick
import QtQuick.Window
import Victron.VenusOS

// Real pointer drags on the home page: page swipes, a chart's cursor, a vertical list scroll.
Window {
	id: win
	width: 1024; height: 544; visible: true
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	QtObject { id: pm; function pushPage() { return null } function popPage() {} }
	property var home: null
	property var swipe: null
	Component.onCompleted: {
		Global.pageManager = pm
		const data = JSON.parse(dataText)
		for (const k in data) MockStore.put(svc + "/" + k, data[k])
		MockStore.put(svc + "/Settings/Schema", schemaText)
		home = Qt.createComponent(pluginDir + "/XregHome.qml").createObject(contentItem, { svc: svc, animationEnabled: true, width: 1024, height: 544 })
		swipe = find(home, x => x.contentModel !== undefined && x.pageInView !== undefined)
	}
	function find(root, pred) {
		for (const c of root.children) { if (pred(c)) return c; const f = find(c, pred); if (f) return f }
		if (root.contentItem && root.contentItem !== root) { const f = find(root.contentItem, pred); if (f) return f }
		return null
	}
	function index() { return swipe.currentIndex }
	function holds() { return home.swipeHolds }
	// centre of the first chart's plot area on the Plots page, in window coordinates
	function chartPoint() {
		const chart = find(home, c => c.scrubbed !== undefined && c.hiddenKeys !== undefined && c.visible)
		const area = chart ? find(chart, c => c.hold !== undefined && c.host !== undefined) : null
		if (!area) return Qt.point(-1, -1)
		return area.mapToItem(contentItem, area.width / 2, area.height / 2)
	}
	function plotsCursor() { const t = find(home, c => c.windowS !== undefined && c.cursor !== undefined); return t ? t.cursor : NaN }
	function settingsList() { return find(home, c => c.contentY !== undefined && c.bottomMargin !== undefined && c.visible && c.contentHeight > c.height && c.parent && c.parent.embedded !== undefined) }
	function listY() { const l = settingsList(); return l ? l.contentY : -1 }
	function listPoint() { const l = settingsList(); return l ? l.mapToItem(contentItem, l.width / 2, l.height / 2) : Qt.point(-1, -1) }
	function setTab(i) { home.setupTab = i }
}
