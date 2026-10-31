import QtQuick
ListNavigation {
	id: root
	readonly property alias dataItem: dataItem
	property var optionModel: []
	property int currentIndex: {
		if (!optionModel || optionModel.length === undefined || dataItem.uid.length === 0 || !dataItem.valid) return defaultIndex
		for (let i = 0; i < optionModel.length; ++i) if (optionModel[i].value === dataItem.value) return i
		return defaultIndex
	}
	readonly property var currentValue: currentIndex >= 0 && currentIndex < optionModel.length ? optionModel[currentIndex].value : undefined
	property bool updateCurrentIndexOnClick: true
	property bool updateDataOnClick: true
	property var popDestination: null
	property var validatePassword
	property int defaultIndex: -1
	property string defaultSecondaryText: "Unknown"
	property Component optionHeader
	property Component optionFooter
	signal optionClicked(index: int)
	signal aboutToPop()
	secondaryText: currentIndex >= 0 && currentIndex < optionModel.length ? optionModel[currentIndex].display : defaultSecondaryText
	interactive: (dataItem.uid === "" || dataItem.valid)
	function choose(i) {
		if (updateDataOnClick && dataItem.uid.length > 0) dataItem.setValue(optionModel[i].value)
		optionClicked(i)
	}
	VeQuickItem { id: dataItem }
}
