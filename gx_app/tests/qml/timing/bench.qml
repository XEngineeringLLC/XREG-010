import QtQuick
import Victron.VenusOS

Item {
	id: root
	width: 1024
	height: 480
	property Item page: loader.item

	Loader { id: loader; anchors.fill: parent }

	function load(src) {
		loader.setSource(src, { svc: "mock", toolX: 500, toolY: 8, bodyY: 58 })
	}
	function unload() { loader.source = "" }
	function feed(m) {
		for (const k in m)
			MockStore.put("mock/Plot/Short/" + k, m[k])
	}
	function created() { return MockStore.created }

	// the page's own parse, as snap() calls it: 12 line() parses (Field % four times) plus the stage strip
	function parseLikeSnap(m, w, n) {
		const keys = ["Bcur", "MeasuredAmps", "iiout", "dutyCycle", "pidAltPV", "BatteryV", "dutyCycle", "IBV", "RPM",
					  "dutyCycle", "AlternatorTemperatureC", "dutyCycle", "chargeStage"]
		const t0 = Date.now()
		for (let r = 0; r < n; r++)
			for (const k of keys) {
				let v = String(m[k]).split(",").map(s => s.length ? parseFloat(s) : NaN)
				v = v.slice(Math.max(0, v.length - w))
			}
		return (Date.now() - t0) / n
	}
	// parse only the window's tail, each series once
	function parseTail(m, w, n) {
		const keys = ["Bcur", "MeasuredAmps", "iiout", "dutyCycle", "pidAltPV", "BatteryV", "IBV", "RPM",
					  "AlternatorTemperatureC", "chargeStage"]
		const t0 = Date.now()
		for (let r = 0; r < n; r++)
			for (const k of keys) {
				const s = String(m[k])
				let i = s.length
				for (let c = 0; c < w && i > 0; c++)
					i = s.lastIndexOf(",", i - 1)
				const v = s.slice(i + 1).split(",").map(q => q.length ? parseFloat(q) : NaN)
			}
		return (Date.now() - t0) / n
	}
	function snapLoop(n) {
		const t0 = Date.now()
		for (let r = 0; r < n; r++)
			page.snap()
		return (Date.now() - t0) / n
	}
}
