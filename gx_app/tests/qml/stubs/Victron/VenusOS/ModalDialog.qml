import QtQuick
import QtQuick.Templates as T
T.Dialog {
	id: root
	property string secondaryTitle
	property int titleTextFormat
	property int dialogDoneOptions: 4
	property bool canAccept: true
	property bool headerShowsTitles: true
	property var tryAccept
	property var tryReject
	component FocusableContentItem : FocusScope {
		focus: true
		implicitWidth: 600
		implicitHeight: 200
	}
	width: 600; height: 400
}
