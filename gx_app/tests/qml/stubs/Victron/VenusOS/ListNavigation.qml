import QtQuick
import QtQuick.Controls.impl as CP
ListSetting {
	id: root
	property string secondaryText
	property color secondaryTextColor: Theme.color_listItem_secondaryText
	property string iconSource: Qt.resolvedUrl("icon_chevron_right_32.svg")
	property color iconColor: Theme.color_listItem_forwardIcon
	signal clicked
	function click() { if (interactive) clicked() }
	interactive: true
	hasSubMenu: interactive
	contentItem: Item {
		implicitWidth: Theme.geometry_listItem_width
		implicitHeight: labelLayout.implicitHeight
		ThreeLabelLayout {
			id: labelLayout
			anchors { left: parent.left; right: parent.right; rightMargin: arrowIcon.visible ? arrowIcon.width + root.spacing : 0; verticalCenter: parent.verticalCenter }
			primaryText: root.text
			primaryLabel.font: root.font
			secondaryText: root.secondaryText
			secondaryLabel.color: root.secondaryTextColor
			captionText: root.caption
			stretchSecondaryText: true
		}
		CP.ColorImage {
			id: arrowIcon
			anchors { right: parent.right; verticalCenter: parent.verticalCenter }
			source: root.iconSource
			color: root.iconColor
			visible: root.interactive
		}
	}
	MouseArea { anchors.fill: parent; enabled: root.interactive; onClicked: root.click() }
}
