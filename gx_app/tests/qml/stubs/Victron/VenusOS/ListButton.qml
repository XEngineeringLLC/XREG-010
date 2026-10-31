import QtQuick
ListSetting {
	id: root
	property string secondaryText
	property bool readOnly: false
	signal clicked
	function click() { if (readOnly || !root.clickable) return; root.clicked() }
	interactive: true
	contentItem: Item {
		implicitWidth: Theme.geometry_listItem_width
		implicitHeight: contentLayout.isMultiLine ? contentLayout.implicitHeight : 0
		TwoLabelItemLayout {
			id: contentLayout
			anchors.verticalCenter: parent.verticalCenter
			width: parent.width
			primaryText: root.text
			primaryLabel.font: root.font
			captionText: root.caption
			secondaryComponent: Button {
				leftPadding: Theme.geometry_listItemButton_horizontalPadding
				rightPadding: Theme.geometry_listItemButton_horizontalPadding
				font.pixelSize: Theme.font_listItem_secondary_size
				flat: false
				text: root.secondaryText
				enabled: root.clickable && !root.readOnly
				onClicked: root.click()
			}
		}
	}
}
