import QtQuick
import Victron.VenusOS

// Checks of the 2026-10-30 speed set: redraw on arrival (one per update), tabs kept alive and idle while hidden,
// reopen state, the bench timing log; and Short Term's smooth scroll (2026-10-31). Context: pluginDir, demoText.
Item {
	id: win
	width: 1024
	height: 544
	property int fails: 0
	readonly property string svc: "mock"
	readonly property var demo: JSON.parse(demoText)
	function check(ok, msg) { if (!ok) fails++; console.log((ok ? "PASS " : "FAIL ") + msg) }

	QtObject {
		id: counter
		property var n: ({})
		property var at: ({})
		function add(name, ms) { n[name] = (n[name] || 0) + 1; at[name] = Date.now() }
		function count(name) { return n[name] || 0 }
	}

	Item { id: area; width: 1024; height: 544 }

	// steps run one after another, each after its own wait (ms)
	property var steps: []
	Timer { id: stepper; interval: 1; onTriggered: win.next() }
	function after(ms, f) { steps.push({ ms: ms, f: f }) }
	function next() {
		if (!steps.length) { console.log("DONE fails=" + fails); Qt.callLater(Qt.quit); return }
		const s = steps.shift()
		try { s.f() } catch (e) { check(false, "exception " + e + " " + e.stack) }
		stepper.interval = steps.length ? Math.max(1, steps[0].ms) : 1
		stepper.start()
	}

	function findAll(root, pred, out) {
		out = out || []
		for (const c of root.children) { if (pred(c) && out.indexOf(c) < 0) out.push(c); findAll(c, pred, out) }
		if (root.contentItem && root.contentItem !== root) { if (pred(root.contentItem) && out.indexOf(root.contentItem) < 0) out.push(root.contentItem); findAll(root.contentItem, pred, out) }
		return out
	}
	// every QObject child (Timers, VeQuickItems are not visual children)
	function objects(root, pred, out) {
		out = out || []
		const kids = (root.resources ? Array.from(root.resources) : []).concat(root.children ? Array.from(root.children) : [])
		for (const c of kids) {
			if (!c || out.indexOf(c) >= 0) continue
			if (pred(c)) out.push(c)
			objects(c, pred, out)
		}
		if (root.contentItem && root.contentItem !== root) objects(root.contentItem, pred, out)
		if (root.item) objects(root.item, pred, out)
		return out
	}
	function itemCount(root) { return findAll(root, c => true).length }
	function isTimer(o) { return o && o.interval !== undefined && o.triggeredOnStart !== undefined && o.repeat !== undefined }

	readonly property var shortKeys: ["Bcur", "MeasuredAmps", "iiout", "dutyCycle", "pidAltPV", "BatteryV", "IBV", "RPM", "AlternatorTemperatureC", "chargeStage"]
	// one bridge update: every series one sample on, the newest sample k
	function shortText(key, k) {
		const a = String(demo["Plot/Short/" + key]).split(",")
		const v = key === "chargeStage" ? "1" : String(1000 + k)
		return a.slice(1).concat([v]).join(",")
	}
	function putShort(key, k) { MockStore.put(svc + "/Plot/Short/" + key, shortText(key, k)) }
	function burst(k) { for (const key of shortKeys) putShort(key, k) }
	function newest(tab, group, i) { const l = tab.d[group][i]; return l.v[l.v.length - 1] }
	function writesTo(path) { return MockStore.writes.filter(w => w.uid === svc + path).length }
	// /Fast/Short (xreg_core.py ShortRecorder.message): samples n-k+1..n, 100 ms apart, every reading 1000 + its number
	readonly property var fastOrder: ["MeasuredAmps", "Bcur", "iiout", "dutyCycle", "pidAltPV", "BatteryV", "IBV", "RPM", "AlternatorTemperatureC", "chargeStage"]
	function shortMsg(n, k, full) {
		let dts = [], cols = fastOrder.map(() => [])
		for (let i = 0; i < k; ++i) {
			dts.push(full && i === 0 ? 0 : 100)
			fastOrder.forEach((key, j) => cols[j].push(key === "chargeStage" ? "1" : String(1000 + n - k + 1 + i)))
		}
		return ["s1", "E1", n, k, full ? 1 : 0, dts.join(",")].concat(cols.map(c => c.join(","))).join(";")
	}
	function putFast(m) { MockStore.put(svc + "/Fast/Short", m) }
	function newestOf(key) { const a = shortTab.store.v[key]; return a[a.length - 1] }

	function seedLongHealthWind() {
		const now = Math.floor(Date.now() / 1000)
		MockStore.put(svc + "/Ui/LongRange", "24h")
		MockStore.put(svc + "/Plot/Long/Range", "24h")
		MockStore.put(svc + "/Plot/Long/Fields", "battVolt,altCurr")
		MockStore.put(svc + "/Plot/Long/StartEpoch", now - 86400)
		MockStore.put(svc + "/Plot/Long/StepS", 300)
		let bv = [], ac = []
		for (let i = 0; i < 288; ++i) { bv.push((13 + i / 1000).toFixed(2)); ac.push((50 + i % 7).toFixed(1)) }
		MockStore.put(svc + "/Plot/Long/battVolt", bv.join(","))
		MockStore.put(svc + "/Plot/Long/altCurr", ac.join(","))
		let tr = ["eng,worst,overall"]
		for (let i = 0; i < 20; ++i) tr.push((i * 0.5) + "," + (90 + i % 3) + "," + (95 + i % 2))
		MockStore.put(svc + "/Health/TrendCsv/0", tr.join("\n"))
		MockStore.put(svc + "/Health/SessionCsv", "#nowMs=100000\nt,pct,amps,measAmps,pred,rpm,fieldPct,tempF,vbus,tSlope,state,ring\n"
			+ "90000,97,80,80,82,1800,60,150,13.9,0.1,0,1\n95000,98,81,81,82,1800,61,151,13.9,0.1,0,1\n")
		MockStore.put(svc + "/Health/Live/valid", 1)
		for (const k of ["Tws", "Twa", "Aws", "Awa"])
			MockStore.put(svc + "/Plot/Wind/" + k, demo["Plot/Wind/" + k])
	}

	property var shortTab: null
	property var plots: null
	property var kept: ({})
	property real fedAt: 0

	Component.onCompleted: {
		BackendConnection.type = BackendConnection.DBusSource
		seedLongHealthWind()

		// -- 1. Short Term over D-Bus (2026-10-30 fast lane): the whole history once, then one update per message, at once
		after(1, () => {
			const c = Qt.createComponent(pluginDir + "/XregPlotsShort.qml")
			check(c.status === Component.Ready, "XregPlotsShort loads " + c.errorString())
			shortTab = c.createObject(area, { svc: svc, timing: counter, width: 1024, height: 544, toolX: 500, toolY: 8, bodyY: 58 })
		})
		after(60, () => {
			check(writesTo("/Ui/Want/Short") === 1, "open: one want write: " + writesTo("/Ui/Want/Short"))
			check(counter.count("short") === 0 && counter.count("shortLoad") === 0, "open: nothing on /Fast/Short yet, nothing drawn")
			const t = objects(shortTab, isTimer)
			check(!t.some(x => x.interval === 1000 && x.repeat), "no free-running 1 s redraw timer")
			win.chartsS = findAll(shortTab, c => c.sStep !== undefined)
			check(win.chartsS.length === 4, "four charts")
			putFast(shortMsg(300, 300, true))                 // the bridge's answer to the first ask
		})
		after(200, () => {
			check(counter.count("shortLoad") === 1, "open: the whole history read once: " + counter.count("shortLoad"))
			check(shortTab.store.n === 300 && shortTab.store.t.length === 300, "open: 300 samples held: " + shortTab.store.n)
			check(win.chartsS.every(c => c.hasData && !c.sBuilding), "open: every chart drawn")
			check(newestOf("MeasuredAmps") === 1300, "open: newest sample held: " + newestOf("MeasuredAmps"))
			win.v0 = win.chartsS.map(c => c.sVer)
			win.fedAt = Date.now()
			putFast(shortMsg(301, 1, false))
			check(counter.count("short") === 1, "D-Bus: one update, drawn inside the delivery: " + counter.count("short"))
			check(win.chartsS.every((c, i) => c.sVer === win.v0[i] + 1), "D-Bus: every chart stepped once")
			check(shortKeys.every(k => newestOf(k) === (k === "chargeStage" ? 1 : 1301)), "D-Bus: every series' newest sample held")
			check(win.chartsS[0].valueText(win.chartsS[0].lines[1]) === "1301.0 A", "D-Bus: the legend shows it: " + win.chartsS[0].valueText(win.chartsS[0].lines[1]))
			putFast(shortMsg(302, 1, false))
		})
		after(40, () => putFast(shortMsg(303, 1, false)))
		after(40, () => putFast(shortMsg(305, 2, false)))
		after(40, () => check(counter.count("short") === 4, "three more updates 40 ms apart: one each: " + counter.count("short")))
		after(1, () => {
			const c = counter.count("short"), n = writesTo("/Fast/ShortFull")
			putFast(shortMsg(310, 1, false))                  // samples 306-309 missed
			check(counter.count("short") === c && writesTo("/Fast/ShortFull") === n + 1, "a gap: nothing drawn, the whole history asked for")
			putFast(shortMsg(310, 310, true))
		})
		// the answer overlaps what is held: only its newest samples are taken, no second load
		after(200, () => check(shortTab.store.n === 310 && shortTab.store.t.length === 310 && counter.count("shortLoad") === 1
							   && newestOf("MeasuredAmps") === 1310, "the answer: its newest 5 samples taken: " + shortTab.store.n))

		// -- 2. Short Term over MQTT: each message is a whole update (one path), drawn as it comes
		after(1, () => {
			BackendConnection.type = BackendConnection.MqttSource
			check(!objects(shortTab, isTimer).some(x => x.interval === 100 && !x.repeat), "MQTT: no gathering window needed")
			win.c0 = counter.count("short")
			putFast(shortMsg(311, 1, false))
		})
		for (let i = 2; i <= 10; ++i) {
			const n = 310 + i
			after(7, () => putFast(shortMsg(n, 1, false)))
		}
		after(10, () => {
			check(counter.count("short") === win.c0 + 10, "MQTT: ten messages, ten updates: " + (counter.count("short") - win.c0))
			check(newestOf("MeasuredAmps") === 1320, "MQTT: the newest held: " + newestOf("MeasuredAmps"))
		})
		// a continuous stream (one message every 20 ms for 1.5 s): every one taken, in order
		after(1, () => { win.streamStart = counter.count("short"); win.streamN = 0; streamer.start() })
		after(1600, () => {
			const got = counter.count("short") - win.streamStart
			check(win.streamN === 75 && got === 75 && shortTab.store.n === 395, "MQTT stream: " + win.streamN + " messages in 1.5 s, " + got + " updates, newest " + shortTab.store.n)
			BackendConnection.type = BackendConnection.DBusSource
		})

		// -- 3. hidden: samples kept, nothing drawn, no want writes; shown: one step at once and a want write
		after(150, () => {
			win.c0 = counter.count("short")
			win.v0 = win.chartsS.map(c => c.sVer)
			win.w0 = writesTo("/Ui/Want/Short")
			shortTab.visible = false
			putFast(shortMsg(396, 1, false))
		})
		after(60, () => {
			check(counter.count("short") === win.c0 && win.chartsS.every((c, i) => c.sVer === win.v0[i]), "hidden: an update is not drawn")
			check(shortTab.behind === true && shortTab.store.n === 396, "hidden: the sample is kept, the tab notes it is behind")
			check(objects(shortTab, isTimer).every(x => !x.running || (x.interval === 4000 && !x.repeat)), "hidden: no timer running")
			putFast(shortMsg(397, 1, false))
			shortTab.visible = true
		})
		after(30, () => {
			check(win.chartsS.every((c, i) => c.sVer === win.v0[i] + 1), "shown again: one step per chart: " + win.chartsS.map((c, i) => c.sVer - win.v0[i]))
			check(newestOf("MeasuredAmps") === 1397 && win.chartsS[0].valueText(win.chartsS[0].lines[1]) === "1397.0 A", "shown again: the newest update drawn")
			check(writesTo("/Ui/Want/Short") === win.w0 + 1, "shown again: one want write at once")
		})

		// -- 3b. smooth scroll (2026-10-31): the lines and stage strip move every display frame, up to sBehindMs behind the
		// newest sample by the samples' clock; a sample that comes on time moves nothing on screen; hidden, nothing runs.
		// A new history first: the stream above came five times faster than real time, which no link does
		after(1, () => putFast(shortMsg(400, 400, true).replace(";E1;", ";E2;")))
		after(100, () => putFast(shortMsg(401, 1, false).replace(";E1;", ";E2;")))
		after(100, () => putFast(shortMsg(402, 1, false).replace(";E1;", ";E2;")))
		after(100, () => {
			const at = c => c.sOffX + c.sLagS * c.sKx
			win.sx = win.chartsS.map(at)
			putFast(shortMsg(403, 1, false).replace(";E1;", ";E2;"))
			const moved = win.chartsS.map((c, i) => Math.abs(at(c) - win.sx[i]))
			check(moved.every(d => d < 0.3), "smooth: a sample on time moves no line: " + moved.map(d => d.toFixed(2)) + " px")
			check(win.chartsS.every(c => c.sSmoothOn && c.sLagS > 0 && c.sLagS <= 0.1001),
				  "smooth: the right edge up to 0.1 s behind the newest: " + win.chartsS.map(c => c.sLagS.toFixed(3)) + " s")
			const bulk = findAll(win.chartsS[0], c => c.text === "Bulk"), strip = bulk.length ? bulk[0].parent.parent : null
			check(!!strip && Math.abs(strip.x - win.chartsS[0].sLagS * win.chartsS[0].sKx) < 0.01, "smooth: the stage strip moves with the lines")
			win.sl = win.chartsS[0].sLagS
		})
		after(300, () => {
			const d = win.sl - win.chartsS[0].sLagS
			check(d > 0.2 && d < 0.45, "smooth: 0.3 s with no sample, the lines kept moving: " + d.toFixed(3) + " s")
			shortTab.visible = false
		})
		after(30, () => {
			check(win.chartsS.every(c => !c.sSmoothOn), "smooth: hidden, nothing redrawn frame by frame")
			shortTab.visible = true
		})
		after(30, () => {
			check(win.chartsS.every(c => c.sSmoothOn), "smooth: shown again, scrolling")
			shortTab.destroy()
		})

		// -- 4. Plots: tabs kept after their first visit, idle while hidden, reset on return
		after(20, () => {
			const c = Qt.createComponent(pluginDir + "/XregPlots.qml")
			check(c.status === Component.Ready, "XregPlots loads " + c.errorString())
			plots = c.createObject(area, { svc: svc, timing: counter, width: 1024, height: 544 })
			win.items = [itemCount(plots)]
		})
		after(60, () => { kept.s = slot(0).item; plots.tab = 1 })
		after(60, () => { win.items.push(itemCount(plots)); kept.l = slot(1).item; plots.tab = 2 })
		after(60, () => { win.items.push(itemCount(plots)); kept.h = slot(2).item; plots.tab = 3 })
		after(60, () => {
			win.items.push(itemCount(plots)); kept.w = slot(3).item
			console.log("INFO items under Plots after visiting Short, +Long, +Health, +Wind: " + win.items.join(", "))
			check(kept.s && kept.l && kept.h && kept.w, "every tab built once visited")
			win.c0 = counter.count("short")
			win.chartsS = findAll(kept.s, c => c.sStep !== undefined)
			win.sv = win.chartsS.map(c => c.sVer)
			win.nObj = itemCount(plots)
			plots.tab = 0
		})
		after(1, () => {
			check(slot(0).item === kept.s, "Short Term not rebuilt on return")
			check(win.chartsS.every((c, i) => !c.sBuilding && c.sVer - win.sv[i] <= 1), "Short Term: return, nothing rebuilt (no new samples: no step)")
			check(itemCount(plots) === win.nObj, "switching tabs creates no items: " + itemCount(plots) + " vs " + win.nObj)
			const visibleTabs = [0, 1, 2, 3].filter(i => slot(i).visible)
			check(visibleTabs.length === 1 && visibleTabs[0] === 0, "only the shown tab is visible")
			// hidden tabs: no timer running, no want writes, nothing redrawn while their data changes
			for (const [name, it] of [["Long", kept.l], ["Health", kept.h], ["Wind", kept.w]]) {
				const run = objects(it, isTimer).filter(x => x.running)
				check(run.length === 0, name + " hidden: no timer running (" + run.map(x => x.interval).join(",") + ")")
			}
			win.cl = counter.count("long"); win.cw = counter.count("wind")
			win.wl = writesTo("/Ui/Want/Long"); win.wh = writesTo("/Ui/Want/Health"); win.ww = writesTo("/Ui/Want/Wind")
			win.hAt = kept.h.sessAt
			win.hTrend = kept.h.trend.length
			MockStore.put(svc + "/Plot/Long/StepS", 301)
			MockStore.put(svc + "/Plot/Long/altCurr", "1,2,3")
			MockStore.put(svc + "/Plot/Wind/Aws", "1.0,2.0,3.0")
			MockStore.put(svc + "/Health/SessionCsv", "#nowMs=200000\n190000,99,80,80,82,1800,60,150,13.9,0.1,0,1\n")
			MockStore.put(svc + "/Health/TrendCsv/0", "eng,worst,overall\n1,90,95\n2,91,96\n3,92,97\n4,93,98\n")
		})
		after(300, () => {
			check(counter.count("long") === win.cl && counter.count("wind") === win.cw, "hidden Long and Wind: their updates not drawn")
			check(kept.h.sessAt === win.hAt, "hidden Health: session not parsed")
			check(kept.h.trend.length === 0, "hidden Health: trend not parsed (" + kept.h.trend.length + " points held)")
			check(writesTo("/Ui/Want/Long") === win.wl && writesTo("/Ui/Want/Health") === win.wh && writesTo("/Ui/Want/Wind") === win.ww,
				"hidden tabs: no want writes")
			// a tapped legend entry and a cursor on Short Term, then away and back
			const ch = findAll(kept.s, c => c.hiddenKeys !== undefined && c.resets !== undefined)
			win.chartsS = ch
			ch[0].toggle(ch[0].lines[0])
			kept.s.cursor = -5
			check(Object.keys(ch[0].hiddenKeys).length === 1 && kept.s.cursor === -5, "Short Term: a legend entry hidden and a cursor set")
			plots.tab = 1
		})
		after(1, () => {
			check(slot(1).item === kept.l, "Long Term not rebuilt on return")
			check(Math.abs(kept.l.openedAt - Date.now()) < 50, "Long Term: its loading grace restarts on return")
		})
		// agentQ: Long Term redraws chart by chart, one per event-loop turn
		after(60, () => {
			check(!kept.l.busy && counter.count("long") === win.cl + 1, "Long Term: one redraw on return, with the data sent while hidden")
			check(kept.l.d.xHi - kept.l.d.xLo === 288 * 301 && kept.l.d.cur[0].v.length === 3, "Long Term: the redraw holds the update sent while hidden: " + (kept.l.d.xHi - kept.l.d.xLo))
			plots.tab = 2
		})
		after(1, () => {
			check(slot(2).item === kept.h, "Health not rebuilt on return")
			check(kept.h.trend.length === 4, "Health: trend parsed on return: " + kept.h.trend.length)
			check(Math.abs(kept.h.sessAt - Date.now() / 1000) < 0.05 && kept.h.sessRows.length === 1, "Health: session adopted on return, as on opening")
			check(kept.h.liveRows.length === 0, "Health: live dots start again on return, as on opening")
			plots.tab = 0
		})
		after(1, () => {
			check(win.chartsS.every(c => Object.keys(c.hiddenKeys).length === 0 && isNaN(c.ownCursor)), "Short Term on return: legend as on opening")
			check(isNaN(kept.s.cursor), "Short Term on return: no cursor")
			plots.destroy()
		})

		// -- 5. bench timing log on the home page
		after(30, () => {
			const c = Qt.createComponent(pluginDir + "/XregHome.qml")
			check(c.status === Component.Ready, "XregHome loads " + c.errorString())
			win.home = c.createObject(area, { svc: svc, animationEnabled: false, width: 1024, height: 544 })
		})
		after(100, () => {
			const t = objects(win.home, o => isTimer(o) && o.interval === 60000)
			check(t.length === 1, "home: one 60 s timing timer")
			win.logTimer = t[0]
			win.home.timing.runs = {}
			t[0].stop()
			check(!t[0].running, "home: the timing timer idles until something runs")
			win.home.timing.add("probe", 4)
			win.home.timing.add("probe", 10)
			win.home.timing.add("other", 1)
			check(t[0].running && !t[0].repeat, "home: the first run starts the 60 s window (single shot)")
			t[0].triggered()
			t[0].stop()
			check(Object.keys(win.home.timing.runs).length === 0, "home: the window's runs cleared after its line")
			const dash = findAll(win.home, c => c.timing !== undefined && c.svc !== undefined && c.linkUp !== undefined && c.view !== undefined)
			check(dash.length === 1 && dash[0].timing === win.home.timing, "home: the speed tile reports to the home page's timing")
			win.home.preload = true
		})
		after(600, () => {
			const plotsPage = findAll(win.home, c => c.timing !== undefined && c.shortWindow !== undefined)
			check(plotsPage.length === 1 && plotsPage[0].timing === win.home.timing, "home: Plots reports to the home page's timing")
			win.home.destroy()
		})
		stepper.start()
	}
	function slot(i) { return findAll(plots, c => c.sourceComponent !== undefined && c.index === i)[0] }
	property int streamStart
	property int streamN
	property var items
	property int c0
	property int w0
	property int cl
	property int cw
	property int wl
	property int wh
	property int ww
	property real hAt
	property int hTrend
	property int nObj
	property var chartsS
	property var sx
	property real sl
	property var sv
	property var v0
	property var home
	property var logTimer
	Timer {
		id: streamer
		interval: 20
		repeat: true
		onTriggered: {
			win.streamN++
			win.putFast(win.shortMsg(320 + win.streamN, 1, false))
			if (win.streamN >= 75) stop()
		}
	}
}
