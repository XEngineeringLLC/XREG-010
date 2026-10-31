import QtQuick
Label {
	height: text.length > 0 ? implicitHeight : 0
	font.pixelSize: Theme.font_listItem_caption_size
	color: Theme.color_font_secondary
	wrapMode: Text.Wrap
}
