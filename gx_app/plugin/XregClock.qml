import QtQuick
import Victron.VenusOS

// The GX clock and its offset from UTC. ClockTime follows the GX's time-zone setting; a JS Date follows the process
// (UTC on a GX) or the browser.
QtObject {
	readonly property real nowS: typeof ClockTime !== "undefined" && ClockTime.clockTime > 0 ? ClockTime.clockTime : Date.now() / 1000
	readonly property bool known: typeof ClockTime !== "undefined" && ClockTime.clockTime > 0
	readonly property int tzS: {
		if (!known)
			return -new Date().getTimezoneOffset() * 60
		const u = new Date(ClockTime.clockTime * 1000)
		const d = ((ClockTime.hour * 60 + ClockTime.minute - u.getUTCHours() * 60 - u.getUTCMinutes()) % 1440 + 1440) % 1440
		return (d > 840 ? d - 1440 : d) * 60
	}
	function offsetAt(s) { return known ? tzS : -new Date(s * 1000).getTimezoneOffset() * 60 }
	// A Date whose local fields read the GX's wall clock at s, for formatting
	function date(s) { return new Date((s + offsetAt(s) + new Date(s * 1000).getTimezoneOffset() * 60) * 1000) }
}
