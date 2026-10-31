import QtQuick
Label {
	readonly property bool effectiveVisible: preferredVisible
	property bool preferredVisible: true
	width: parent ? parent.width : 0
	height: preferredVisible ? implicitHeight : 0
	visible: preferredVisible
	topPadding: Theme.geometry_listSectionHeader_topPadding
	bottomPadding: Theme.geometry_listSectionHeader_bottomPadding
	leftPadding: Theme.geometry_page_content_horizontalMargin + Theme.geometry_listItem_content_horizontalMargin
	rightPadding: Theme.geometry_page_content_horizontalMargin + Theme.geometry_listItem_content_horizontalMargin
	font.pixelSize: Theme.font_listItem_flat_primary_size_flat
	wrapMode: Text.Wrap
	color: Theme.color_font_secondary
}
