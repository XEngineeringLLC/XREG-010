import QtQuick
ListSetting {
	readonly property alias dataItem: dataItem
	property string secondaryText: dataItem.valid ? dataItem.value : ""
	property string placeholderText
	property string suffix
	interactive: (dataItem.uid === "" || dataItem.valid)
	VeQuickItem { id: dataItem }
}
