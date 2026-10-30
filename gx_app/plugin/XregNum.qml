import QtQuick
import Victron.VenusOS

// A numeric readout that holds still as its value changes, the way the web app's readouts do (tabular-nums, a min-width
// in ch, right alignment): tabular digits, and a fixed box as wide as `template`, the widest text the quantity can show
// (sign, digits, decimals and unit), with the text right-aligned in it. A minus sign appearing, 9.9 becoming 10.0 or a
// third digit arriving then changes only the text inside the box, never the box or what sits beside it.
Label {
	id: num

	property string template          // e.g. "−188.8 A"; empty: the box follows the text, like a plain Label
	readonly property real boxWidth: Math.ceil(metrics.advanceWidth) + 1

	width: template.length ? boxWidth : implicitWidth
	horizontalAlignment: Text.AlignRight
	font.features: { "tnum": 1 }

	TextMetrics {
		id: metrics
		font: num.font
		text: num.template
	}
}
