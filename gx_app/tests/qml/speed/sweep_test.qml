import QtQuick
import Victron.VenusOS

// The sliced speed-curve sweep (XregChartPerf) against the inline sweep it replaced, value by value, plus slicing,
// supersede and display checks. Context: oldDir (inline sweep), newDir (sliced), demoText.
Item {
	id: win
	width: 1024
	height: 600
	property int fails: 0
	readonly property var demo: JSON.parse(demoText)
	function check(ok, msg) { if (!ok) fails++; console.log((ok ? "PASS " : "FAIL ") + msg) }

	QtObject {
		id: counter
		property var vals: ({})
		function add(name, ms) { (vals[name] = vals[name] || []).push(ms) }
		function list(name) { return vals[name] || [] }
	}
	Item { id: area; width: 430; height: 348 }

	property var steps: []
	Timer { id: stepper; interval: 1; onTriggered: win.next() }
	function after(ms, f) { steps.push({ ms: ms, f: f }) }
	// repeat f every ms until it returns true (or limit polls)
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

	property var oldComp: Qt.createComponent(oldDir + "/XregChartPerf.qml")
	property var newComp: Qt.createComponent(newDir + "/XregChartPerf.qml")
	function put(k, v) { MockStore.put("mock/" + k, v) }
	function seedDemo() {
		for (const k in demo)
			if (k.startsWith("Perf/") || k === "Live/Rpm")
				MockStore.put("mock/" + k, demo[k])
	}
	function make(comp, sc) {
		return comp.createObject(area, { svc: "mock", linkUp: true, active: true, width: 430, height: 348, timing: counter,
			pinned: sc.view, trueFrame: sc.trueFrame })
	}
	function same(a, b) {
		if (!a || !b || a.pts.length !== b.pts.length) return "lengths " + (a ? a.pts.length : "-") + " / " + (b ? b.pts.length : "-")
		for (let i = 0; i < a.pts.length; ++i)
			for (let k = 0; k < 2; ++k)
				if (!Object.is(a.pts[i][k], b.pts[i][k])) return "point " + i + "." + k + ": " + a.pts[i][k] + " vs " + b.pts[i][k]
		if (!Object.is(a.maxS, b.maxS) || !Object.is(a.maxR, b.maxR)) return "max " + a.maxS + "/" + a.maxR + " vs " + b.maxS + "/" + b.maxR
		return ""
	}

	// synthetic fronts
	function csvSail(rows) { return "BEFRONT1,SAIL,3,0,aws,awa,sea,spd\n" + rows.map(r => r.join(",")).join("\n") + "\n" }
	function csvBoth(sail, motor) {
		return csvSail(sail) + "BEFRONT1,MOTOR,3,0,rpm,hw,sea,spd\n" + motor.map(r => r.join(",")).join("\n") + "\n"
	}
	property real seed: 12345
	function rnd() { seed = (seed * 16807) % 2147483647; return (seed - 1) / 2147483646 }

	property var scenarios: []
	property int compared
	property var oldObj
	property var newObj

	function scenarioList() {
		let out = []
		for (const v of [0, 1]) for (const t of [false, true])
			out.push({ name: "demo view " + v + (t ? " true" : " apparent"), view: v, trueFrame: t, set: () => {} })
		for (let i = 0; i < 24; ++i) {
			const ws = (rnd() * 30).toFixed(3), hw = (rnd() * 35 - 10).toFixed(3), sea = (rnd() * 8).toFixed(3)
			const ridge = [0, 0.05, 0.1, 0.3][Math.floor(rnd() * 4)], idw = [1, 1.5, 2, 3][Math.floor(rnd() * 4)]
			const v = Math.floor(rnd() * 2), t = rnd() < 0.5
			out.push({ name: "random " + i + " view " + v + " ws " + ws + " hw " + hw + " sea " + sea + " ridge " + ridge + " idw " + idw + (t ? " true" : ""),
				view: v, trueFrame: t, set: () => {
					put("Perf/Live/ws", ws); put("Perf/Motor/headwind", hw)
					put("Perf/Live/pitchStd", sea); put("Perf/Motor/pitchStd", sea)
					put("Perf/RidgeFrac", ridge); put("Perf/IdwPower", idw)
				} })
		}
		// an asymmetric (unfolded) polar: angles -175..175
		let asym = [], motorSmall = []
		for (let a = -175; a <= 175; a += 7)
			for (const w of [6, 12, 18])
				asym.push([w, a, 1, (2 + w * 0.25 + Math.abs(Math.sin(a * Math.PI / 180)) * 3 + (a > 0 ? 0.4 : 0)).toFixed(3)])
		for (const r of [600, 1200, 1900]) motorSmall.push([r, 0, 1, (r / 400).toFixed(3)])
		for (const t of [false, true])
			out.push({ name: "unfolded polar" + (t ? " true" : ""), view: 0, trueFrame: t, set: () => {
				put("Perf/CurveCsv", csvBoth(asym, motorSmall)); put("Perf/Live/ws", "12.3"); put("Perf/Live/pitchStd", "1.04")
				put("Perf/RidgeFrac", 0.1); put("Perf/IdwPower", 2) } })
		out.push({ name: "motor front of 3 points", view: 1, trueFrame: false, set: () => {
			put("Perf/CurveCsv", csvBoth(asym, motorSmall)); put("Perf/Motor/headwind", "4.4"); put("Perf/Motor/pitchStd", "0.5") } })
		// collinear front hit exactly by the sweep, no ridge: singular fits, so the inverse-distance and exact-point paths
		let line = []
		for (let k = 0; k <= 30; ++k) line.push([10, 30 + k * 5, 0, (3 + k * 0.1).toFixed(2)])
		out.push({ name: "singular fits, exact hits, ridge 0", view: 0, trueFrame: false, set: () => {
			put("Perf/CurveCsv", csvBoth(line, motorSmall)); put("Perf/Live/ws", "10"); put("Perf/Live/pitchStd", "0")
			put("Perf/RidgeFrac", 0); put("Perf/IdwPower", 1.5) } })
		out.push({ name: "sail front of 1 point (no sweep)", view: 0, trueFrame: false, set: () => {
			put("Perf/CurveCsv", csvBoth([[8, 40, 1, 4]], motorSmall)); put("Perf/RidgeFrac", 0.1); put("Perf/IdwPower", 2) } })
		return out
	}

	function runScenario(sc) {
		after(1, () => {
			seedDemo()
			sc.set()
			win.oldObj = make(oldComp, sc)
			win.newObj = make(newComp, sc)
			check(!!oldObj && !!newObj, sc.name + ": both load")
		})
		until(2, () => !newObj.sweeping)
		after(1, () => {
			const d = same(oldObj.curve, newObj.curve)
			check(d === "" && newObj.shown === newObj.curve && newObj.curve.view === newObj.view,
				sc.name + ": sliced = inline, " + newObj.curve.pts.length + " points" + (d ? " (" + d + ")" : ""))
			if (d === "") compared += newObj.curve.pts.length
			oldObj.destroy(); newObj.destroy()
		})
	}

	property var curves: []
	property var c0
	property var jobB
	property real xB
	property int slicesBefore
	property var oldC

	Component.onCompleted: {
		check(oldComp.status === Component.Ready && newComp.status === Component.Ready, "components load " + oldComp.errorString() + newComp.errorString())
		for (const sc of scenarioList())
			runScenario(sc)
		after(1, () => console.log("INFO " + compared + " curve points compared value by value"))

		// -- first open: no "No sailing data yet" while the first sweep runs, then the curve
		after(1, () => {
			seedDemo()
			put("Perf/RidgeFrac", 0.1); put("Perf/IdwPower", 2)
			counter.vals = {}
			win.newObj = make(newComp, { view: 0, trueFrame: false })
			const lbl = labelAll(newObj, "No sailing")
			check(newObj.sweeping && !newObj.haveCurve && lbl.length === 1 && !lbl[0].visible, "opening: sweep pending, no curve and no \"No sailing data yet\"")
		})
		until(2, () => !newObj.sweeping)
		after(1, () => {
			check(newObj.haveCurve && !labelAll(newObj, "No sailing")[0].visible, "opening: curve shown when the sweep ends")
			const sl = counter.list("perfSlice"), tot = counter.list("perf"), wall = counter.list("perfWall")
			console.log("INFO slices of one sweep (ms, Date.now): " + sl.join(" ") + "; total " + tot + ", wall " + wall)
			check(sl.length >= 2 && Math.max.apply(null, sl) <= 5, "sweep in " + sl.length + " slices, longest " + Math.max.apply(null, sl) + " ms (Mac)")
			check(tot.length === 1 && wall.length === 1, "timing log gets the total (perf), each slice (perfSlice) and wall time (perfWall)")
		})

		// -- supersede: a new input during a sweep replaces it; the old curve stays until the new one is complete
		after(1, () => {
			win.c0 = newObj.curve
			win.curves = []
			newObj.curveChanged.connect(() => win.curves.push(newObj.curve))
			put("Perf/Live/ws", "20.1")
			newObj.sweptAt = -Infinity      // the 15 s hold has run out
			newObj.refresh()
			check(newObj.sweeping, "supersede: sweep B started")
		})
		after(4, () => {
			win.jobB = newObj.job
			win.xB = jobB ? jobB.x : NaN
			check(!!jobB && newObj.curve === c0, "supersede: B under way, the chart still shows the previous curve")
			put("Perf/Live/ws", "3.2")
			newObj.sweptAt = -Infinity
			newObj.refresh()
			check(newObj.job !== jobB && newObj.curve === c0, "supersede: sweep C replaces B")
		})
		until(1, () => {
			if (newObj.sweeping && newObj.curve !== c0) { check(false, "a curve was shown before its sweep ended"); return true }
			return !newObj.sweeping
		})
		after(1, () => {
			check(jobB.x === xB, "supersede: B did no step after C started")
			check(curves.length === 1, "supersede: one curve shown, C's (" + curves.length + " assignments)")
			win.oldC = make(oldComp, { view: 0, trueFrame: false })
			const d = same(oldC.curve, newObj.curve)
			check(d === "", "supersede: C's curve = the inline sweep's for C's inputs" + (d ? " (" + d + ")" : ""))
			oldC.destroy()
		})

		// -- view change: the sail curve is never drawn on the motoring plot
		after(1, () => {
			newObj.pinned = 1
			check(newObj.view === 1 && newObj.sweeping && !newObj.haveCurve && newObj.curve.view === 0, "view change: the other view's curve is not drawn while the motoring sweep runs")
			check(!labelAll(newObj, "No motoring")[0].visible, "view change: no \"No motoring data yet\" while it runs")
		})
		until(2, () => !newObj.sweeping)
		after(1, () => {
			check(newObj.haveCurve && newObj.curve.view === 1, "view change: motoring curve shown when its sweep ends")
			newObj.destroy()
		})

		// -- a destroyed tile mid-sweep: no error
		after(1, () => { win.newObj = make(newComp, { view: 0, trueFrame: false }) })
		after(3, () => { check(newObj.sweeping, "destroy mid-sweep: sweep under way"); newObj.destroy() })
		after(60, () => check(true, "destroy mid-sweep: no error after it"))

		holdCases()
		cacheCases()
		stepper.start()
	}

	// -- the hold: a wind (headwind) or sea-state change alone starts a sweep at most 15 s after the last one started;
	// a view, frame, curve, ridge or power change starts one at once
	property var starts: []
	property real at0
	property int n0
	function holdTimer(o) {
		for (const c of o.data)
			if (c.interval !== undefined && c.repeat === false && c.triggeredOnStart === false)
				return c
		return null
	}
	function sameAsInline(label, sc) {
		const ref = make(oldComp, sc)
		const d = same(ref.curve, newObj.curve)
		if (d !== "") console.log("INFO keys new " + newObj.curveKey + " / inline " + ref.curveKey)
		check(d === "" && newObj.curve.view === sc.view, label + ": curve = the inline sweep's for the inputs now" + (d ? " (" + d + ")" : ""))
		ref.destroy()
	}
	function openTile(view) {
		after(1, () => {
			seedDemo()
			put("Perf/RidgeFrac", 0.1); put("Perf/IdwPower", 2); put("Perf/Live/ws", "12.0"); put("Perf/Live/pitchStd", "1.0")
			put("Perf/Motor/headwind", "3.0"); put("Perf/Motor/pitchStd", "1.0")
			win.newObj = make(newComp, { view: view, trueFrame: false })
			win.starts = []
			newObj.jobChanged.connect(() => { if (newObj.job) win.starts.push(Date.now()) })
		})
		until(2, () => !newObj.sweeping)
	}
	// a change the hold should take: the last sweep started 14 s ago, so the hold runs out about 1 s from now
	function heldChange(label, f) {
		after(1, () => {
			win.c0 = newObj.curve
			win.n0 = starts.length
			newObj.sweptAt = Date.now() - 14000
			f()
			newObj.refresh()
			win.at0 = Date.now()
			const h = holdTimer(newObj)
			check(!newObj.sweeping && newObj.curve === c0 && starts.length === n0 && !!h && h.running && h.interval > 900 && h.interval <= 1000,
				label + ": no sweep, the hold waits " + (h ? h.interval : "?") + " ms")
		})
	}
	function holdRunsOut(label, sc) {
		until(5, () => newObj.sweeping)
		after(1, () => {
			const late = Date.now() - at0
			check(starts.length === n0 + 1 && late >= 900 && late < 1250 && newObj.curve === c0,
				label + ": one sweep when the hold runs out, " + late + " ms on; the old curve stays meanwhile")
		})
		until(2, () => !newObj.sweeping)
		after(1, () => sameAsInline(label, sc))
	}
	// a change that starts a sweep at once although a wind change is held
	function atOnce(label, f, sc) {
		after(1, () => {
			win.n0 = starts.length
			const k = newObj.view ? "Perf/Motor/headwind" : "Perf/Live/ws"
			put(k, String(Number(MockStore.get("mock/" + k)) + 2.5))
			newObj.refresh()
			check(!newObj.sweeping && holdTimer(newObj).running && starts.length === n0, label + ": a wind change first, held")
			f()
			check(newObj.sweeping && starts.length === n0 + 1 && !holdTimer(newObj).running, label + ": starts a sweep at once and ends the hold")
		})
		until(2, () => !newObj.sweeping)
		after(1, () => sameAsInline(label, sc))
	}
	function holdCases() {
		openTile(0)
		heldChange("wind change", () => put("Perf/Live/ws", "14.0"))
		holdRunsOut("wind change", { view: 0, trueFrame: false })

		heldChange("sea-state change", () => put("Perf/Live/pitchStd", "2.3"))
		after(200, () => {
			const h = holdTimer(newObj), iv = h.interval
			put("Perf/Live/ws", "15.0"); newObj.refresh()
			put("Perf/Live/ws", "16.0"); newObj.refresh()
			check(!newObj.sweeping && h.running && h.interval === iv && starts.length === n0, "more wind changes during the hold: still no sweep, the hold not restarted")
		})
		holdRunsOut("sea state then two wind changes", { view: 0, trueFrame: false })
		after(1500, () => check(starts.length === n0 + 1, "one sweep for the three changes held, none after: " + (starts.length - n0)))

		after(1, () => {
			win.n0 = starts.length
			put("Perf/Live/ws", "21.0")
			newObj.sweptAt = Date.now() - 15001
			newObj.refresh()
			check(newObj.sweeping && starts.length === n0 + 1, "a wind change 15 s after the last sweep started: a sweep at once")
		})
		until(2, () => !newObj.sweeping)
		after(1, () => sameAsInline("wind change after 15 s", { view: 0, trueFrame: false }))

		heldChange("wind back to the swept speed", () => put("Perf/Live/ws", "23.0"))
		after(1, () => { put("Perf/Live/ws", "21.0"); newObj.refresh() })
		after(1300, () => check(!newObj.sweeping && starts.length === n0 && newObj.curve === c0, "wind back to the swept speed during the hold: no sweep"))

		heldChange("hidden tile", () => put("Perf/Live/ws", "25.0"))
		after(1, () => { newObj.visible = false })
		after(1300, () => {
			check(!newObj.sweeping && starts.length === n0, "the hold runs out while the tile is hidden: no sweep")
			newObj.visible = true
		})
		after(5, () => check(newObj.sweeping && starts.length === n0 + 1, "shown again: the held change sweeps at once"))
		until(2, () => !newObj.sweeping)
		after(1, () => sameAsInline("hidden tile", { view: 0, trueFrame: false }))

		atOnce("frame tap", () => { newObj.trueFrame = true }, { view: 0, trueFrame: true })
		atOnce("ridge change", () => { put("Perf/RidgeFrac", 0.12); newObj.refresh() }, { view: 0, trueFrame: true })
		atOnce("power change", () => { put("Perf/IdwPower", 3); newObj.refresh() }, { view: 0, trueFrame: true })
		atOnce("new curve data", () => put("Perf/CurveCsv", demo["Perf/CurveCsv"] + "\n"), { view: 0, trueFrame: true })
		atOnce("view tap", () => { newObj.pinned = 1 }, { view: 1, trueFrame: true })
		heldChange("headwind change (motoring)", () => put("Perf/Motor/headwind", "9.0"))
		holdRunsOut("headwind change (motoring)", { view: 1, trueFrame: true })
		after(1, () => newObj.destroy())

		// gusty wind for 16.5 s, the tile's own 2 s check only: one sweep at opening, one 15 s later
		openTile(0)
		after(1, () => { win.at0 = Date.now(); win.gust = 0 })
		until(250, () => {
			gust++
			put("Perf/Live/ws", String(12 + (gust % 2 ? 1.5 : -1.5)))
			put("Perf/Live/pitchStd", String(1 + (gust % 3) * 0.3))
			return Date.now() - at0 >= 16500
		}, 100)
		after(1, () => {
			const rel = starts.map(t => t - at0)
			console.log("INFO gusty wind, sweep starts after opening (ms): " + rel.join(" "))
			check(starts.length === 1 && rel[0] > 14500 && rel[0] < 15300,
				"gusty wind (a 3 kn and sea-state change every 250 ms for 16.5 s): one sweep, 15 s after the opening one")
			newObj.destroy()
		})
	}
	property int gust
	// -- curves kept this visit: a frame or view tap back, or a wind back to a swept speed, shows one at once
	property var cKept
	function instant(label, f, sc, sameObj) {
		after(1, () => {
			win.n0 = starts.length
			const before = newObj.curve
			f()
			check(!newObj.sweeping && starts.length === n0 && newObj.curve !== before && newObj.curve.view === sc.view
				&& newObj.haveCurve && (!sameObj || newObj.curve === sameObj()),
				label + ": the curve at once, no sweep")
			sameAsInline(label, sc)
		})
	}
	function cacheCases() {
		openTile(0)
		after(1, () => { win.cKept = newObj.curve })
		instant("frame tap, nothing else changed", () => { newObj.trueFrame = true }, { view: 0, trueFrame: true })
		instant("frame tap back", () => { newObj.trueFrame = false }, { view: 0, trueFrame: false }, () => cKept)
		after(1, () => {
			win.n0 = starts.length
			newObj.pinned = 1
			check(newObj.sweeping && starts.length === n0 + 1, "view tap to motoring, never swept this visit: a sweep")
		})
		until(2, () => !newObj.sweeping)
		instant("view tap back to sailing", () => { newObj.pinned = 0 }, { view: 0, trueFrame: false }, () => cKept)
		instant("view tap to motoring again", () => { newObj.pinned = 1 }, { view: 1, trueFrame: false })
		after(1, () => {
			win.n0 = starts.length
			const before = newObj.curve
			newObj.trueFrame = true
			check(!newObj.sweeping && starts.length === n0 && newObj.curve === before, "frame change while motoring: the motoring curve stays, no sweep")
		})
		after(1, () => { newObj.trueFrame = false; newObj.pinned = 0 })
		after(1, () => {
			check(newObj.curve === cKept, "back on sailing, apparent: the opening curve")
			put("Perf/Live/ws", "18.0")
			newObj.sweptAt = -Infinity
			newObj.refresh()
			check(newObj.sweeping, "a wind change to another speed: a sweep")
		})
		until(2, () => !newObj.sweeping)
		instant("wind back to a speed swept this visit, within the hold", () => { put("Perf/Live/ws", "12.0"); newObj.refresh() },
			{ view: 0, trueFrame: false }, () => cKept)
		after(1, () => {
			win.n0 = starts.length
			put("Perf/CurveCsv", demo["Perf/CurveCsv"] + "\n\n")
			const rev = String(newObj.curveRev)
			check(newObj.sweeping && starts.length === n0 + 1 && Object.keys(newObj.swept).every(k => k.split("|")[1] === rev),
				"new curve data: the kept curves dropped, a sweep")
		})
		until(2, () => !newObj.sweeping)
		instant("frame tap after new curve data", () => { newObj.trueFrame = true }, { view: 0, trueFrame: true })
		after(1, () => {
			for (let k = 0; k < 12; ++k) { put("Perf/Live/ws", String(4 + k)); newObj.sweptAt = -Infinity; newObj.refresh() }
		})
		until(2, () => !newObj.sweeping)
		after(1, () => {
			check(newObj.sweptKeys.length <= 8 && Object.keys(newObj.swept).length === newObj.sweptKeys.length,
				"at most 8 curves kept: " + newObj.sweptKeys.length)
			newObj.destroy()
		})
	}
	function labelAll(root, prefix, out) {
		out = out || []
		for (const c of root.children) {
			if (c.text !== undefined && String(c.text).startsWith(prefix)) out.push(c)
			labelAll(c, prefix, out)
		}
		return out
	}
}
