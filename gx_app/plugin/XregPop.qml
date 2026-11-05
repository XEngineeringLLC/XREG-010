import QtQuick
import Victron.VenusOS

// The larger view of a tile or chart, over the whole home page (XregHome), over a lightly dimmed page. A chart's panel is
// 80% of the width (94% on a phone held upright) and 84% of the height; a panel whose root sets `fits` is as big as its
// content (implicitWidth/implicitHeight, which must not follow its own width) and no bigger. A tap on the page around
// the panel closes it; what was set in the panel is in the view's own object (XregView), so the small tile shows it too.
// open(component): the panel's content, made in its page's context, so it binds to that page's data; the page under it
// keeps running. openSheet(component): a small box over the panel (an axis limit), closed by a tap around it; while its
// root's `typing` is set and an on-screen keyboard shows, the box sits at the top, clear of the keyboard.
Item {
	id: pop

	property Component panel: null
	property Component sheet: null
	readonly property bool shown: panel !== null
	readonly property real dim: 0.3
	function open(c) { sheet = null; panel = c }
	function close() { closeSheet(); panel = null }
	function openSheet(c) { sheet = c }
	function closeSheet() {
		if (sheet !== null)
			Qt.inputMethod.hide()
		sheet = null
	}

	visible: shown
	z: 100

	MouseArea {
		anchors.fill: parent
		onClicked: pop.close()
	}
	Rectangle {
		anchors.fill: parent
		color: Qt.rgba(0, 0, 0, pop.dim)
	}
	Rectangle {
		id: box
		readonly property Item content: loader.item
		readonly property bool fits: !!content && content.fits === true
		readonly property real maxW: Math.round(pop.width * (pop.width < 600 ? 0.94 : 0.8))
		anchors.centerIn: parent
		width: fits ? Math.min(pop.width - 16, Math.ceil(content.implicitWidth) + 2 * loader.anchors.margins) : maxW
		height: fits ? Math.min(pop.height - 16, Math.ceil(content.implicitHeight) + 2 * loader.anchors.margins)
			: Math.round(pop.height * 0.84)
		radius: Theme.geometry_overviewPage_widget_radius
		color: Theme.color_background_secondary
		border.color: Theme.color_card_separator
		border.width: 1
		// taps inside the panel stay in it
		MouseArea { anchors.fill: parent }
		Loader {
			id: loader
			anchors { fill: parent; margins: 14 }
			active: pop.shown
			sourceComponent: pop.panel
		}
	}

	Item {
		anchors.fill: parent
		visible: pop.sheet !== null
		MouseArea {
			anchors.fill: parent
			onClicked: pop.closeSheet()
		}
		Rectangle {
			anchors.fill: parent
			color: Qt.rgba(0, 0, 0, pop.dim)
		}
		Rectangle {
			readonly property bool lifted: !!sheetLoader.item && sheetLoader.item.typing === true && Qt.inputMethod.visible
			x: Math.round((parent.width - width) / 2)
			y: lifted ? 8 : Math.round((parent.height - height) / 2)
			Behavior on y {
				enabled: !!sheetLoader.item && sheetLoader.item.typing === true   // not as the box opens
				NumberAnimation { duration: 180; easing.type: Easing.InOutQuad }
			}
			width: sheetLoader.item ? Math.ceil(sheetLoader.item.implicitWidth) : 0
			height: sheetLoader.item ? Math.ceil(sheetLoader.item.implicitHeight) : 0
			radius: Theme.geometry_overviewPage_widget_radius
			color: Theme.color_background_secondary
			border.color: Theme.color_card_separator
			border.width: 1
			MouseArea { anchors.fill: parent }
			Loader {
				id: sheetLoader
				anchors.fill: parent
				active: pop.sheet !== null
				sourceComponent: pop.sheet
			}
		}
	}
}
