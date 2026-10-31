import QtQuick
import QtQuick.Window
import Victron.VenusOS

// Two screens on the Health tab at once, at different windows, over a stand-in bridge that answers as the real one does:
// the bridge before 2026-10-29 kept one trend for the last /Ui/HealthHours written; the bridge after keeps one per window
// (/Ui/Want/HealthTrend/<h> -> /Health/TrendCsv/<h>). A trend's first row is 600 - hours, or 1 for the whole history.
Window {
	id: win
	width: 1024; height: 600; visible: true
	readonly property string svc: "mock/com.victronenergy.switch.xregulator"
	property var screens: []
	function trendFor(h) { const f = h > 0 ? 600 - h : 1; return "engHours,worstPct,overallPct\n" + f.toFixed(3) + ",92.0,95.0\n" + (f + 1).toFixed(3) + ",91.5,94.8\n" }
	Connections {
		target: MockStore
		function onChanged(u) {
			if (u === win.svc + "/Ui/HealthHours")
				MockStore.put(win.svc + "/Health/TrendCsv", win.trendFor(Number(MockStore.get(u))))
			else if (u.indexOf(win.svc + "/Ui/Want/HealthTrend/") === 0)
				MockStore.put(win.svc + "/Health/TrendCsv/" + u.split("/").pop(), win.trendFor(Number(u.split("/").pop())))
		}
	}
	function setup(w1, w2) {
		MockStore.reset()
		MockStore.put(svc + "/Connected", 1)
		const c = Qt.createComponent(pluginDir + "/XregPlotsHealth.qml")
		if (c.status !== Component.Ready) { console.log("FAIL load " + c.errorString()); return false }
		screens = [w1, w2].map((w, i) => c.createObject(contentItem, { svc: svc, windowH: w, x: 0, y: 0, width: 1024, height: 544,
				toolX: 14, toolY: 8, bodyY: 58 }))
		return true
	}
	function timerOf(s) { for (const o of s.data) if (o.interval === 15000 && o.triggeredOnStart !== undefined) return o; return null }
	// the screen's 15 s re-ask, now
	function ask(i) { timerOf(screens[i]).triggered() }
	// the window screen i's chart shows: hours, -1 while empty
	function shows(i) { const t = screens[i].trend; return !t.length ? -1 : t[0].eng === 1 ? 0 : Math.round(600 - t[0].eng) }
	function setWindow(i, w) { screens[i].windowH = w }
	function writes() { return MockStore.writes.map(w => w.uid.replace(svc, "") + "=" + w.value) }
	function writePaths() { return MockStore.writes.map(w => w.uid.replace(svc, "")).filter((p, k, a) => a.indexOf(p) === k).sort().join(" ") }
}
