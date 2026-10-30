import QtQuick
import Victron.VenusOS

// A row of tabs (or of any buttons that pick one of several) that never elides a title: the largest of three font sizes,
// up to maxPx, at which every title fits its button, in as few rows as that takes (three for the eight Setup sub-tabs on
// a phone held upright).
Item {
	id: root

	property var model: []            // titles
	property int currentIndex: -1
	property real rowH: 40
	property real maxPx: Theme.font_button_size   // gui-v2's own tab size
	signal picked(int index)

	readonly property var metrics: [m0, m1, m2].filter(m => m.font.pixelSize <= maxPx)
	// the tab bar's margin inside the border, each side: gui-v2's Button stretches the label over the whole tab
	readonly property real pad: 2 * (Theme.geometry_tabBar_horizontalMargin + Theme.geometry_button_border_width)
	readonly property var layout: {
		const n = Math.max(1, model.length)
		for (let rows = 1; rows <= n; rows++) {
			const per = Math.ceil(n / rows)
			for (const m of metrics)
				if (m.implicitWidth + pad <= width / per)
					return { rows: Math.ceil(n / per), per: per, px: m.font.pixelSize }
		}
		return { rows: 1, per: n, px: m2.font.pixelSize }
	}

	implicitHeight: model.length === 0 ? 0 : layout.rows * rowH + (layout.rows - 1) * 6

	// The titles drawn as a tab draws them (gui-v2's Label), one per line, so implicitWidth is the widest at that size
	Label { id: m0; visible: false; font.pixelSize: Theme.font_size_body1; text: root.model.join("\n") }
	Label { id: m1; visible: false; font.pixelSize: Theme.font_size_caption; text: root.model.join("\n") }
	Label { id: m2; visible: false; font.pixelSize: Theme.font_size_tiny; text: root.model.join("\n") }

	Repeater {
		model: root.model.length ? root.layout.rows : 0
		delegate: TabBar {
			id: bar
			required property int index
			readonly property int first: index * root.layout.per
			readonly property var titles: root.model.slice(first, first + root.layout.per)
			y: index * (root.rowH + 6)
			width: root.width * titles.length / root.layout.per
			height: root.rowH
			fontPixelSize: root.layout.px
			model: titles.map(t => ({ value: t }))
			currentIndex: root.currentIndex - first
			onButtonClicked: function(buttonIndex) {
				root.picked(bar.first + buttonIndex)
				Qt.callLater(() => bar.currentIndex = Qt.binding(() => root.currentIndex - bar.first))
			}
		}
	}
}
