import QtQuick
import Victron.VenusOS

// The bridge's wind hour, /Plot/Wind (xreg_core.py WindRecorder.message): one sample per CSV4 frame of the last hour,
// the whole hour when first seen or after a gap (asked for on /Plot/WindFull), then the newest samples as they come.
// Held as XregChart's streamed lines read it (store, ver). The page writes its own /Ui/Want/ path. The same protocol
// as XregPlotsShort's /Fast/Short.
Item {
	id: ws

	property string svc
	property bool active               // the page is shown: ver counts updates now, else once on return
	property QtObject timing           // XregHome's bench timing log: "wind" per update, "windLoad" per whole hour

	readonly property var names: ["Tws", "Twa", "Aws", "Awa"]
	readonly property var angleCols: [1, 3]
	// The samples held, oldest first: t in ms, one array per series, sample i at t[i - off], held from off + h. A new
	// history replaces it with reset one higher; ver counts updates (XregChart). Streamed lines draw no angle wrap, so an
	// angle that crosses 0/360 between two samples gets a break sample before the later one: that angle NaN, the rest as
	// the later sample, 1 in b (0 for a reading).
	property var store: blank("", 0)
	property int ver
	property bool behind               // updated while hidden: redraw on return
	property var job: null             // a whole hour being read in slices, and the messages that came meanwhile
	property real askedAt: -1e9
	readonly property int keepMs: 3600 * 1000

	VeQuickItem {
		id: feed
		uid: ws.svc + "/Plot/Wind"
		onValueChanged: ws.take(feed.value)
	}
	VeQuickItem { id: ask; uid: ws.svc + "/Plot/WindFull" }
	Component.onCompleted: take(feed.value)
	onActiveChanged: if (active && behind) bump()

	function blank(epoch, reset) {
		let v = {}
		for (const name of names)
			v[name] = []
		return { t: [], v: v, b: [], off: 0, h: 0, reset: reset, epoch: epoch, n: 0 }
	}
	function bump() {
		behind = !active
		if (active)
			ver++
	}
	// One /Plot/Wind message: "w1;epoch;newest number;count;full;dt,..;<a list per series>"
	function take(msg) {
		if (!msg)
			return
		if (job) {
			job.queue.push(String(msg))
			return
		}
		const t0 = Date.now()
		const r = apply(String(msg))
		if (r === "gap" && Date.now() - askedAt > 2000) {
			askedAt = Date.now()
			ask.setValue(1)
		}
		if (r === "inc" || r === "tail" || r === "new") {
			bump()
			if (timing && active)
				timing.add("wind", Date.now() - t0)
		}
	}
	// the samples after the ones held, a new history, or a gap (the rules of XregPlotsShort.apply)
	function apply(msg) {
		const p = msg.split(";")
		if (p.length !== 6 + names.length || p[0] !== "w1")
			return "bad"
		const epoch = p[1], n = Number(p[2]), k = Number(p[3]), full = p[4] === "1"
		const first = n - k + 1, same = epoch === store.epoch
		if (same && n <= store.n)
			return "old"
		if (same && first <= store.n) {
			append(p, k, store.n - first + 1, n)
			return "tail"
		}
		if (full) {
			job = { p: p, epoch: epoch, n: n, k: k, cols: [], queue: [], ms: 0 }
			loadSlice()
			return "load"
		}
		if (same && first === store.n + 1) {
			append(p, k, 0, n)
			return "inc"
		}
		if (!same && first === 1) {
			store = blank(epoch, store.reset + 1)
			append(p, k, 0, n)
			return "new"
		}
		return "gap"
	}
	// the last `count` of the `total` comma-separated items of str
	function items(str, count, total) {
		if (count >= total)
			return str.split(",")
		let i = str.length
		for (let c = 0; c < count; ++c)
			i = str.lastIndexOf(",", i - 1)
		return str.slice(i + 1).split(",")
	}
	function num(x) { return x === "" ? NaN : +x }
	// one sample at t, after a break sample where an angle wraps
	function add(s, t, row) {
		const n = s.t.length
		if (n) {
			let wrap = null
			for (const j of angleCols) {
				const was = s.v[names[j]][n - 1]
				if (isFinite(was) && isFinite(row[j]) && Math.abs(row[j] - was) > 180)
					(wrap = wrap || {})[j] = true
			}
			if (wrap) {
				s.t.push(t - 1 > s.t[n - 1] ? t - 1 : t)
				s.b.push(1)
				for (let j = 0; j < names.length; ++j)
					s.v[names[j]].push(wrap[j] ? NaN : row[j])
			}
		}
		s.t.push(t)
		s.b.push(0)
		for (let j = 0; j < names.length; ++j)
			s.v[names[j]].push(row[j])
	}
	function append(p, k, from, n) {
		const s = store, c = k - from
		if (c > 0) {
			const dts = items(p[5], c, k)
			const cols = []
			for (let j = 0; j < names.length; ++j)
				cols.push(items(p[6 + j], c, k))
			for (let i = 0; i < c; ++i) {
				const t = (s.t.length ? s.t[s.t.length - 1] : 0) + Number(dts[i])
				add(s, t, cols.map(col => num(col[i])))
			}
			trim(s)
		}
		s.n = n
	}
	// keep the hour, and the sample before it that the lines enter from (the bridge keeps the same)
	function trim(s) {
		const n = s.t.length, lim = s.t[n - 1] - keepMs
		while (n - s.h >= 2 && s.t[s.h + 1] <= lim)
			s.h++
		if (s.h > 2048 && s.h > n / 2) {
			s.t.splice(0, s.h)
			s.b.splice(0, s.h)
			for (const name of names)
				s.v[name].splice(0, s.h)
			s.off += s.h
			s.h = 0
		}
	}
	// A whole hour, read 4 ms at a time (once per animation tick, a Timer) so a tap is never held long: dt first, then
	// each series. Then the messages that came meanwhile.
	Timer {
		id: loadLater
		interval: 0
		onTriggered: ws.loadSlice()
	}
	function loadSlice() {
		const jb = job
		if (!jb)
			return
		const t0 = Date.now()
		do {
			const j = jb.cols.length
			const a = jb.k ? jb.p[5 + j].split(",") : []
			const out = new Array(a.length)
			for (let i = 0; i < a.length; ++i)
				out[i] = j === 0 ? Number(a[i]) : num(a[i])
			jb.cols.push(out)
		} while (jb.cols.length < names.length + 1 && Date.now() - t0 < 4)
		if (jb.cols.length < names.length + 1) {
			jb.ms += Date.now() - t0
			loadLater.restart()
			return
		}
		const s = blank(jb.epoch, store.reset + 1)
		let t = 0
		for (let i = 0; i < jb.k; ++i) {
			t += i ? jb.cols[0][i] : 0
			add(s, t, [jb.cols[1][i], jb.cols[2][i], jb.cols[3][i], jb.cols[4][i]])
		}
		s.n = jb.n
		if (s.t.length)
			trim(s)
		jb.ms += Date.now() - t0
		job = null
		const q = jb.queue
		store = s
		if (timing)
			timing.add("windLoad", jb.ms)
		bump()
		for (const m of q)
			take(m)
	}
}
