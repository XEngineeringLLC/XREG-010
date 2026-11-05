import QtQuick
import QtQuick.Window
import Victron.VenusOS

// Real pointer drags on the home page: page swipes, charts that open a larger view (Short Term's and the Wind tab's: a tap
// opens, a drag moves the page), a vertical list scroll.
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
	function holds() { return swipe.interactive ? 0 : 1 }
	// centre of the Wind tab's chart, in window coordinates (-1, -1: not found)
	function chartPoint() {
		const chart = find(home, c => c.editAxes !== undefined && c.tapOpens !== undefined && c.visible && c.tapOpens && /wind/.test(c.title))
		return chart ? chart.mapToItem(contentItem, chart.width / 2, chart.height / 2) : Qt.point(-1, -1)
	}
	// centre of a chart that opens a larger view (Short Term's first chart)
	function openerPoint() {
		const chart = find(home, c => c.editAxes !== undefined && c.tapOpens !== undefined && c.tapOpens && c.visible)
		return chart ? chart.mapToItem(contentItem, chart.width / 2, chart.height / 2) : Qt.point(-1, -1)
	}
	function popShown() { return home.popShown }
	// the Plots page's tab (0 Short Term ... 3 Wind)
	function plotsTab(i) { const p = find(home, c => c.built !== undefined && c.tab !== undefined && c.barW !== undefined); if (p) p.tab = i }
	function bigTitle() { const b = find(home, c => c.editAxes !== undefined && c.tapOpens !== undefined && c.editAxes && c.visible); return b ? b.title : "" }
	function settingsList() { return find(home, c => c.contentY !== undefined && c.bottomMargin !== undefined && c.visible && c.contentHeight > c.height && c.parent && c.parent.embedded !== undefined) }
	function listY() { const l = settingsList(); return l ? l.contentY : -1 }
	function listPoint() { const l = settingsList(); return l ? l.mapToItem(contentItem, l.width / 2, l.height / 2) : Qt.point(-1, -1) }
	function setTab(i) { home.setupTab = i }
}
