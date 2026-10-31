import QtQuick
import QtQuick.Templates as T
T.Button {
	id: root
	property color color: enabled ? (down ? Theme.color_button_down_text : Theme.color_font_primary) : Theme.color_font_disabled
	property color backgroundColor: enabled ? (down ? Theme.color_ok : Theme.color_darkOk) : Theme.color_background_disabled
	property color borderColor: enabled ? Theme.color_ok : Theme.color_font_disabled
	property real borderWidth: Theme.geometry_button_border_width
	property real radius: Theme.geometry_button_radius
	down: pressed || checked
	topPadding: topInset + Theme.geometry_button_padding
	bottomPadding: bottomInset + Theme.geometry_button_padding
	leftPadding: leftInset + Theme.geometry_button_padding
	rightPadding: rightInset + Theme.geometry_button_padding
	implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
	implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
	font.family: Global.fontFamily
	font.pixelSize: Theme.font_button_size
	flat: true
	background: Rectangle {
		implicitHeight: Theme.geometry_button_height
		color: root.backgroundColor
		border.width: root.borderWidth
		border.color: root.borderColor
		visible: !root.flat
		radius: root.radius
	}
	contentItem: Label {
		text: root.text
		color: root.color
		font: root.font
		horizontalAlignment: Text.AlignHCenter
		verticalAlignment: Text.AlignVCenter
		elide: Text.ElideRight
	}
}
