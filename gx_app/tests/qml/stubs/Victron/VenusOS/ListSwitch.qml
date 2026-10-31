import QtQuick
ListSetting {
	id: root
	readonly property alias dataItem: dataItem
	readonly property bool defaultInteractive: (dataItem.uid === "" || dataItem.valid)
	property bool checked: invertSourceValue ? dataItem.value === valueFalse : dataItem.value === valueTrue
	property bool checkable
	property string secondaryText
	property bool updateDataOnClick: true
	property bool invertSourceValue
	property var valueTrue: 1
	property var valueFalse: 0
	signal clicked
	function click() {
		if (!root.checkWriteAccessLevel() || !root.clickable) return
		if (root.updateDataOnClick && dataItem.uid.length > 0) dataItem.setValue(checked ? valueFalse : valueTrue)
		if (checkable) checked = !checked
		root.clicked()
	}
	rightPadding: rightInset
	topPadding: topInset
	bottomPadding: bottomInset
	interactive: defaultInteractive
	contentItem: Item {
		implicitWidth: Theme.geometry_listItem_width
		implicitHeight: labelLayout.implicitHeight
		ThreeLabelLayout {
			id: labelLayout
			anchors.verticalCenter: parent.verticalCenter
			width: parent.width - switchItem.width - root.spacing
			primaryText: root.text
			primaryLabel.font: root.font
			secondaryText: root.secondaryText
			captionText: root.caption
			stretchSecondaryText: true
			topPadding: Theme.geometry_listItem_content_verticalMargin
			bottomPadding: Theme.geometry_listItem_content_verticalMargin
		}
		Switch {
			id: switchItem
			anchors { right: parent.right; verticalCenter: parent.verticalCenter }
			topInset: Theme.geometry_listItem_content_verticalMargin
			bottomInset: Theme.geometry_listItem_content_verticalMargin
			leftInset: root.spacing
			rightInset: root.horizontalContentPadding
			topPadding: topInset
			bottomPadding: bottomInset
			leftPadding: leftInset
			rightPadding: rightInset
			checked: root.checked
			enabled: root.clickable
			onClicked: root.click()
		}
	}
	VeQuickItem { id: dataItem }
}
