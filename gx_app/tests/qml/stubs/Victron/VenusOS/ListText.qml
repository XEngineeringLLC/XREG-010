import QtQuick
ListSetting {
	id: root
	readonly property alias dataItem: dataItem
	property string secondaryText: dataItem.valid ? dataItem.value : ""
	property color secondaryTextColor: Theme.color_listItem_secondaryText
	contentItem: Item {
		implicitWidth: Theme.geometry_listItem_width
		implicitHeight: labelLayout.implicitHeight
		ThreeLabelLayout {
			id: labelLayout
			anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
			primaryText: root.text
			primaryLabel.font: root.font
			secondaryText: root.secondaryText
			secondaryLabel.color: root.secondaryTextColor
			captionText: root.caption
		}
	}
	VeQuickItem { id: dataItem }
}
