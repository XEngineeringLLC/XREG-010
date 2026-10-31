import QtQuick
import Victron.VenusOS

// Long Term redrawn chart by chart (new) against the one-block redraw (old): every chart's inputs and the page's d equal
// value by value over the data sets in casesText, in both temperature units; the redraw's order, slicing and skips.
// Context: oldDir, newDir, casesText.
Item {
	id: win
	width: 1024
	height: 600
	property int fails: 0
	property int compared
	readonly property var cases: JSON.parse(casesText)
	function check(ok, msg) { if (!ok) fails++; console.log((ok ? "PASS " : "FAIL ") + msg) }

	QtObject {
		id: counter
		property var vals: ({})
		function add(name, ms) { (vals[name] = vals[name] || []).push(ms) }
		function list(name) { return vals[name] || [] }
	}

	property var steps: []
	Timer { id: stepper; interval: 1; onTriggered: win.next() }
	function after(ms, f) { steps.push({ ms: ms, f: f }) }
	function until(ms, f, limit) { steps.push({ ms: ms, f: f, until: true, left: limit || 4000 }) }
	function next() {
		if (!steps.length) { console.log("DONE fails=" + fails); Qt.callLater(Qt.quit); return }
		const s = steps[0]
		let again = false
		try {
			const r = s.f()
			again = s.until && r !== true && --s.left > 0
			if (s.until && r !== true && s.left <= 0) check(false, "timed out waiting")
		} catch (e) { check(false, "exception " + e + " " + e.stack) }
		if (!again) steps.shift()
		stepper.interval = again ? s.ms : steps.length ? Math.max(1, steps[0].ms) : 1
		stepper.start()
	}

	property var oldComp: Qt.createComponent(oldDir + "/XregPlotsLong.qml")
	property var newComp: Qt.createComponent(newDir + "/XregPlotsLong.qml")
	property var oldP
	property var newP
	function put(k, v) { MockStore.put("mock/" + k, v) }
	function load(c) { for (const k in c.data) put(k, c.data[k]) }
	function make(comp) {
		return comp.createObject(win, { svc: "mock", width: 1024, height: 544, toolX: 492, toolY: 8, bodyY: 58, timing: comp === newComp ? counter : null })
	}
	function charts(root, out) {
		out = out || []
		for (const c of root.children) {
			if (c.hiddenKeys !== undefined && c.resets !== undefined) out.push(c)
			charts(c, out)
		}
		return out
	}

	function deq(a, b, path) {
		if (typeof a === "number" || typeof b === "number")
			return Object.is(a, b) ? (compared++, "") : path + ": " + a + " vs " + b
		if (a === b)
			return ""
		if (a === null || b === null || a === undefined || b === undefined || typeof a !== typeof b)
			return path + ": " + a + " vs " + b
		if (typeof a === "object") {
			if (Array.isArray(a) !== Array.isArray(b))
				return path + ": array vs not"
			if (!Array.isArray(a) && String(a).charAt(0) === "#")
				return String(a) === String(b) ? "" : path + ": " + a + " vs " + b
			const ka = Object.keys(a).sort(), kb = Object.keys(b).sort()
			if (ka.join() !== kb.join())
				return path + ": keys " + ka.join() + " vs " + kb.join()
			for (const k of ka) {
				const r = deq(a[k], b[k], path + "." + k)
				if (r)
					return r
			}
			return ""
		}
		return a === b ? "" : path + ": " + a + " vs " + b
	}
	function compare(label) {
		const co = charts(oldP), cn = charts(newP)
		let diff = co.length === 7 && cn.length === 7 ? "" : "chart count " + co.length + "/" + cn.length
		const props = ["title", "lines", "xLo", "xHi", "xTicks", "stages", "dashNote", "emptyText", "leftFromZero", "leftUnit", "rightFixed", "rightUnit"]
		for (let i = 0; i < 7 && !diff; ++i)
			for (const p of props) {
				diff = deq(co[i][p], cn[i][p], co[i].title + "." + p)
				if (diff)
					break
			}
		if (!diff)
			diff = deq(oldP.d, newP.d, "d")
		const pts = cn.reduce((s, c) => s + c.lines.reduce((t, l) => t + (l.v ? l.v.length : 0), 0), 0)
		check(diff === "", label + ": every chart's inputs and d equal the old redraw's (" + pts + " line points)" + (diff ? " (" + diff + ")" : ""))
	}
	function settled() { return !newP.busy && !newP.dirty && !oldP.dirty }

	// order of chart updates in one redraw, and the redraw step each came in (charts left to do then)
	property var updates: []
	function watch() {
		const cn = charts(newP)
		for (let i = 0; i < cn.length; ++i) {
			const c = cn[i]
			c.linesChanged.connect(() => { if (c.lines.length) win.updates.push({ i: i, left: newP.job ? newP.job.todo.length : -1 }) })
		}
	}

	function runUnit(unit) {
		after(1, () => {
			Global.systemSettings.temperatureUnit = unit
			load(cases[0])
			win.oldP = make(oldComp)
			win.newP = make(newComp)
			check(!!oldP && !!newP, "unit " + unit + ": pages load")
			watch()
			counter.vals = {}
		})
		until(2, settled)
		after(1, () => {
			compare("unit " + unit + ", opening, " + cases[0].name)
			const order = updates.map(u => u.i)
			check(order.join(",") === "0,1,2,3,4,5,6", "opening: the on-screen charts (two rows of two) first, one chart per update: " + order.join(","))
			const left = updates.map(u => u.left)
			check(left.join(",") === "6,5,4,3,2,1,0", "opening: one chart per step: charts left after each " + left.join(","))
			check(counter.list("long").length === 1 && counter.list("longSlice").length === 8 && counter.list("longWall").length === 1,
				"timing log: long once, longSlice per step (header + 7), longWall: " + counter.list("longSlice").join(" "))
		})
		for (let k = 1; k < cases.length; ++k) {
			const c = cases[k]
			after(1, () => { win.updates = []; load(c) })
			until(2, settled)
			after(1, () => {
				compare("unit " + unit + ", " + c.name)
				if (c.name === "the same 24h data again")
					check(updates.length === 0, "the same data again: no chart redrawn (" + updates.length + ")")
			})
		}
		// a return to the tab with nothing new: no chart redrawn; with a legend entry off and a cursor, as on a new chart
		after(1, () => {
			win.updates = []
			oldP.visible = false; newP.visible = false
		})
		after(20, () => {
			oldP.visible = true; newP.visible = true
			oldP.reopen(); newP.reopen()
		})
		until(2, settled)
		after(1, () => {
			check(updates.length === 0, "return, nothing new: no chart redrawn (" + updates.length + ")")
			compare("unit " + unit + ", return")
			const co = charts(oldP), cn = charts(newP)
			for (const cs of [co, cn]) {
				cs[1].toggle(cs[1].lines[0])
				cs[4].scrub(40)
			}
			oldP.cursor = 12345; newP.cursor = 12345
			check(Object.keys(cn[1].hiddenKeys).length === 1 && !isNaN(cn[4].ownCursor), "a legend entry off on one chart, a cursor on another")
			win.updates = []
			oldP.reopen(); newP.reopen()
		})
		until(2, settled)
		after(1, () => {
			const cn = charts(newP)
			check(cn.every(c => Object.keys(c.hiddenKeys).length === 0 && isNaN(c.ownCursor)) && isNaN(newP.cursor),
				"return: legend and cursor reset on every chart")
			check(updates.length === 0, "return: no chart given new lines (" + updates.length + ")")
			compare("unit " + unit + ", return after a legend tap and a cursor")
		})
		// the temperature unit changes with the data unchanged
		after(1, () => { Global.systemSettings.temperatureUnit = unit === 1 ? 2 : 1 })
		until(2, settled)
		after(5, () => { oldP.snap(); newP.snap(false) })
		until(2, settled)
		after(1, () => {
			compare("unit " + unit + " -> " + Global.systemSettings.temperatureUnit + ", data unchanged")
			oldP.destroy(); newP.destroy()
			win.updates = []
		})
	}

	Component.onCompleted: {
		check(oldComp.status === Component.Ready && newComp.status === Component.Ready, "components load " + oldComp.errorString() + newComp.errorString())
		runUnit(2)
		runUnit(1)
		// a redraw replaced mid-way: the second data set's charts all arrive, the first's leftovers none
		after(1, () => {
			Global.systemSettings.temperatureUnit = 1
			load(cases[0])
			win.oldP = make(oldComp)
			win.newP = make(newComp)
			watch()
		})
		until(2, settled)
		after(1, () => { win.updates = []; load(cases[1]) })
		until(1, () => updates.length >= 2)
		after(0, () => load(cases[4]))
		until(2, settled)
		after(1, () => {
			compare("a new range while a redraw runs")
			oldP.destroy(); newP.destroy()
		})
		// scrolled to the foot of the page: the charts on screen there go first
		after(1, () => {
			load(cases[0])
			win.oldP = make(oldComp)
			win.newP = make(newComp)
			watch()
		})
		until(2, settled)
		after(1, () => {
			const fl = []
			const find = (it) => { for (const c of it.children) { if (c.flickableDirection !== undefined) fl.push(c); find(c) } }
			find(newP)
			fl[0].contentY = fl[0].contentHeight - fl[0].height
			win.updates = []
			load(cases[1])
		})
		until(2, settled)
		after(1, () => {
			const order = updates.map(u => u.i)
			check(order.slice(0, 3).sort().join(",") === "4,5,6" && order.length === 7, "scrolled to the foot: its charts first: " + order.join(","))
			compare("scrolled, new range")
			oldP.destroy(); newP.destroy()
		})
		// hidden mid-redraw: stops, and the return draws it
		after(1, () => {
			load(cases[0])
			win.oldP = make(oldComp)
			win.newP = make(newComp)
		})
		until(2, settled)
		after(1, () => { win.updates = []; load(cases[1]) })
		until(1, () => newP.busy && charts(newP).some(c => c.xHi === cases[1].data["Plot/Long/StartEpoch"] + 300 * cases[1].data["Plot/Long/StepS"]))
		after(0, () => { oldP.visible = false; newP.visible = false })
		after(30, () => {
			check(!newP.busy && newP.dirty, "hidden mid-redraw: the redraw stops and waits for the return")
			oldP.visible = true; newP.visible = true; oldP.reopen(); newP.reopen()
		})
		until(2, settled)
		after(1, () => {
			compare("hidden mid-redraw, then back")
			oldP.destroy(); newP.destroy()
		})
		after(1, () => console.log("INFO " + compared + " numbers compared value by value"))
		stepper.start()
	}
}
