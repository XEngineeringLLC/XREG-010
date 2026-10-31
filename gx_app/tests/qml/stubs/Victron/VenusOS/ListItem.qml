import QtQuick
import QtQuick.Templates as T
T.Control {
	id: root
	property int textFormat: Text.AutoText
	property bool flat
	property bool hasSubMenu
	property bool preferredVisible: true
	property bool effectiveVisible: preferredVisible
	readonly property real horizontalContentPadding: flat ? Theme.geometry_listItem_flat_content_horizontalMargin : Theme.geometry_listItem_content_horizontalMargin
	leftInset: flat ? 0 : Theme.geometry_page_content_horizontalMargin
	rightInset: flat ? 0 : Theme.geometry_page_content_horizontalMargin
	bottomInset: Theme.geometry_gradientList_spacing
	leftPadding: leftInset + horizontalContentPadding
	rightPadding: rightInset + horizontalContentPadding
	topPadding: topInset + Theme.geometry_listItem_content_verticalMargin
	bottomPadding: bottomInset + Theme.geometry_listItem_content_verticalMargin
	implicitWidth: effectiveVisible ? Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding) : 0
	implicitHeight: effectiveVisible ? Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding) : 0
	width: parent ? parent.width : Theme.geometry_listItem_width
	spacing: Theme.geometry_listItem_content_spacing
	visible: effectiveVisible
	font.pixelSize: flat ? Theme.font_listItem_flat_primary_size_flat : Theme.font_listItem_primary_size
	font.family: Global.fontFamily
	background: Rectangle {
		implicitWidth: Theme.geometry_listItem_width
		implicitHeight: Theme.geometry_listItem_height
		color: root.flat ? "transparent" : Theme.color_listItem_background
		radius: Theme.geometry_listItem_radius
	}
}
