pragma Singleton
import QtQuick
QtObject {
	function formatNumber(n, prec) {
		if (typeof n !== "number" || !isFinite(n)) { console.warn("formatNumber got " + n); return "--" }
		return Number(n).toFixed(prec === undefined ? 0 : prec)
	}
	function convert(v, from, to) { return from === to ? v : from === 1 ? v * 9 / 5 + 32 : (v - 32) * 5 / 9 }
	function unitToVeUnit(u) { return u }
}
