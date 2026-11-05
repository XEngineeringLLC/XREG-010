import QtQuick
import QtQuick.Shapes
import Victron.VenusOS

// A chart's larger view (in XregPop): the title, Autoscale and Lock axes, and the chart itself, which the page puts
// in as a child and names in `chart` (with editAxes on). Lock axes holds the ranges autoscale shows now; a tap on the
// top or bottom of an axis sets that limit, which turns autoscale off and holds the other ends where they are, as in
// the web app. Everything is set on the view (XregView), which the small chart reads too.
Item {
	id: p

	property QtObject view
	property Item chart
	property string title
	property string leftName              // the axes as the limit box names them, with their units
	property string leftUnit
	property string rightName: "Field"
	property string rightUnit: "%"
	property string hint: "Tap the top or bottom number of an axis to set that limit."
	default property alias content: holder.data

	function axisRange(ax) { return ax ? [ax.lo, ax.hi] : null }
	// the XregPop this view is in, which shows the limit box over it
	readonly property Item host: {
		for (let q = parent; q; q = q.parent) {
			if (q.openSheet !== undefined)
				return q
		}
		return null
	}

	Item {
		id: head
		width: parent.width
		height: 44
		Label {
			anchors { left: parent.left; verticalCenter: parent.verticalCenter }
			width: Math.min(implicitWidth, parent.width - ticks.width - 16)
			elide: Text.ElideRight
			text: p.title
			color: Theme.color_font_primary
			font.pixelSize: Theme.font_size_body2
		}
		Row {
			id: ticks
			anchors { right: parent.right; verticalCenter: parent.verticalCenter }
			spacing: 24
			Tick {
				text: "Autoscale"
				checked: !!p.view && p.view.auto
				onToggled: p.view.setAuto(!p.view.auto, p.axisRange(p.chart.lAx), p.axisRange(p.chart.rAx))
			}
			Tick {
				text: "Lock axes"
				checked: !!p.view && p.view.lock
				enabled: !!p.view && p.view.auto && !!p.chart && !!p.chart.lAx
				onToggled: p.view.setLock(!p.view.lock, p.axisRange(p.chart.lAx), p.axisRange(p.chart.rAx))
			}
		}
	}
	Label {
		id: hint
		anchors.top: head.bottom
		width: parent.width
		elide: Text.ElideRight
		text: p.hint
		color: Theme.color_font_secondary
		font.pixelSize: Theme.font_size_tiny
	}
	Item {
		id: holder
		anchors { top: hint.bottom; topMargin: 6; left: parent.left; right: parent.right; bottom: parent.bottom }
	}

	Connections {
		target: p.chart
		function onAxisTapped(right, isMax) { p.editEnd(right, isMax) }
	}
	// The limit being set: { right, isMax, value, other (the axis's other end), step, decimals, suffix, title }
	property var edit: null
	function editEnd(right, isMax) {
		const ax = right ? chart.rAx : chart.lAx
		if (!ax || !view || !host)
			return
		const dec = right ? 0 : view.decimals
		const fine = Math.pow(10, -dec)
		const unit = right ? rightUnit : leftUnit
		const end = isMax ? ax.hi : ax.lo
		edit = {
			right: right,
			isMax: isMax,
			value: right ? Math.round(end) : view.round(end, isMax),
			other: isMax ? ax.lo : ax.hi,
			// about a thirtieth of the axis a press, so a few presses or a short hold reach any useful limit
			step: Math.max(fine, Math.pow(10, Math.floor(Math.log10((ax.hi - ax.lo) / 30) + 1e-9))),
			decimals: dec,
			suffix: unit ? (unit === "%" || unit === "°" ? unit : " " + unit) : "",
			title: (right ? rightName : leftName) + " " + (isMax ? "maximum" : "minimum")
		}
		host.openSheet(limitSheet)
	}

	// The limit box: about half gui-v2's number dialog each way, so most of the chart stays in sight. A press on - or +
	// moves one step; held, it repeats, ten steps a press after 1 s and a hundred after 2.5 s. A tap on the number types
	// one (Enter or Set applies it). The value stays at least one step inside the axis's other end.
	Component {
		id: limitSheet
		Item {
			id: sh
			readonly property var e: p.edit
			readonly property string title: e ? e.title : ""
			readonly property string suffix: e ? e.suffix : ""
			property real value: e ? e.value : 0
			readonly property real fine: e ? Math.pow(10, -e.decimals) : 1
			property bool typing: false     // XregPop lifts the box above the on-screen keyboard while it shows
			implicitWidth: Math.round(Theme.geometry_modalDialog_width / 2)
			implicitHeight: col.implicitHeight

			function set(x) {
				x = Math.max(-100000, Math.min(100000, x))
				x = e.isMax ? Math.max(x, e.other + fine) : Math.min(x, e.other - fine)
				value = Number(x.toFixed(e.decimals))
			}
			function nudge(dir, mult) {
				const st = e.step * mult
				set(dir > 0 ? (Math.floor(value / st + 1e-6) + 1) * st : (Math.ceil(value / st - 1e-6) - 1) * st)
			}
			function startTyping() {
				field.text = value.toFixed(e.decimals)
				typing = true
				field.forceActiveFocus()
				field.selectAll()
			}
			function stopTyping() {
				typing = false
				field.focus = false
				Qt.inputMethod.hide()
			}
			// the typed number as the value, kept inside the axis; nothing usable typed leaves the value as it was
			function takeTyped() {
				if (!typing)
					return
				const x = parseFloat(field.text.replace(",", "."))
				stopTyping()
				if (isFinite(x))
					set(x)
			}
			function accept() {
				takeTyped()
				p.view.setEnd(e.right, e.isMax, value, p.axisRange(p.chart.lAx), p.axisRange(p.chart.rAx))
				p.host.closeSheet()
			}
			function reject() {
				stopTyping()
				p.host.closeSheet()
			}

			Column {
				id: col
				width: parent.width
				Label {
					width: parent.width
					height: 46
					leftPadding: 12
					rightPadding: 12
					horizontalAlignment: Text.AlignHCenter
					verticalAlignment: Text.AlignVCenter
					elide: Text.ElideRight
					text: sh.title
					color: Theme.color_font_primary
					font.pixelSize: Theme.font_size_body2
				}
				Row {
					anchors.horizontalCenter: parent.horizontalCenter
					spacing: 10
					Stepper { text: "\u2212"; onNudged: function(mult) { sh.takeTyped(); sh.nudge(-1, mult) } }
					Item {
						width: sh.width - 2 * 64 - 2 * 10 - 24
						height: 60
						Label {
							id: shown
							anchors.fill: parent
							visible: !sh.typing
							horizontalAlignment: Text.AlignHCenter
							verticalAlignment: Text.AlignVCenter
							font.features: { "tnum": 1 }
							text: sh.value.toFixed(sh.e ? sh.e.decimals : 0) + sh.suffix
							color: Theme.color_font_primary
							font.pixelSize: Theme.font_size_h1
							font.bold: true
							fontSizeMode: Text.HorizontalFit
							minimumPixelSize: Theme.font_size_body2
						}
						TextInput {
							id: field
							anchors.fill: parent
							visible: sh.typing
							clip: true
							horizontalAlignment: TextInput.AlignHCenter
							verticalAlignment: TextInput.AlignVCenter
							font.family: shown.font.family
							font.pixelSize: Theme.font_size_h1
							font.bold: true
							color: Theme.color_font_primary
							selectionColor: Theme.color_ok
							selectedTextColor: Theme.color_white
							maximumLength: 10
							inputMethodHints: Qt.ImhFormattedNumbersOnly | Qt.ImhNoPredictiveText
							validator: RegularExpressionValidator { regularExpression: /-?[0-9]{0,6}([.,][0-9]{0,2})?/ }
							onAccepted: sh.accept()
							Keys.onEscapePressed: sh.stopTyping()
						}
						// the number can be tapped to type one
						Rectangle {
							anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 6; bottomMargin: 4 }
							height: sh.typing ? 2 : 1
							color: sh.typing ? Theme.color_ok : Theme.color_font_secondary
						}
						MouseArea {
							anchors.fill: parent
							enabled: !sh.typing
							onClicked: sh.startTyping()
						}
					}
					Stepper { text: "+"; onNudged: function(mult) { sh.takeTyped(); sh.nudge(1, mult) } }
				}
				Item { width: 1; height: 12 }
				Rectangle { width: parent.width; height: 1; color: Theme.color_card_separator }
				Row {
					SheetButton { width: (sh.width - 1) / 2; text: "Cancel"; onClicked: sh.reject() }
					Rectangle { width: 1; height: 50; color: Theme.color_card_separator }
					SheetButton { width: (sh.width - 1) / 2; text: "Set"; strong: true; onClicked: sh.accept() }
				}
			}
		}
	}

	component SheetButton: Item {
		id: btn
		property string text
		property bool strong
		signal clicked()
		height: 50
		Label {
			anchors.centerIn: parent
			text: btn.text
			color: btn.strong ? Theme.color_ok : Theme.color_font_primary
			font.pixelSize: Theme.font_size_body2
			font.bold: btn.strong
		}
		MouseArea {
			anchors.fill: parent
			onClicked: btn.clicked()
		}
	}
	// - or +: a step on press, then repeats while held, 10 steps each after 1 s, 100 after 2.5 s
	component Stepper: Rectangle {
		id: stp
		property string text
		property int held
		signal nudged(int mult)
		width: 64
		height: 60
		radius: 6
		color: mouse.pressed ? Theme.color_ok : "transparent"
		border.width: 2
		border.color: Theme.color_ok
		Label {
			anchors.centerIn: parent
			text: stp.text
			color: mouse.pressed ? Theme.color_white : Theme.color_ok
			font.pixelSize: Theme.font_size_h1
		}
		MouseArea {
			id: mouse
			anchors.fill: parent
			onPressed: {
				stp.held = 0
				stp.nudged(1)
				repeat.interval = 400
				repeat.restart()
			}
			onReleased: repeat.stop()
			onCanceled: repeat.stop()
		}
		Timer {
			id: repeat
			repeat: true
			onTriggered: {
				interval = 80
				stp.held++
				stp.nudged(stp.held >= 26 ? 100 : stp.held >= 8 ? 10 : 1)
			}
		}
	}
	component Tick: Item {
		id: tick
		property string text
		property bool checked
		signal toggled()
		implicitWidth: tickRow.implicitWidth
		implicitHeight: 44
		opacity: enabled ? 1 : 0.4
		Row {
			id: tickRow
			anchors.verticalCenter: parent.verticalCenter
			spacing: 8
			Rectangle {
				anchors.verticalCenter: parent.verticalCenter
				width: 24
				height: 24
				radius: 4
				border.width: 2
				border.color: tick.checked ? Theme.color_ok : Theme.color_font_secondary
				color: tick.checked ? Theme.color_ok : "transparent"
				Shape {
					anchors.fill: parent
					visible: tick.checked
					preferredRendererType: Shape.GeometryRenderer
					ShapePath {
						strokeColor: Theme.color_white
						strokeWidth: 3
						fillColor: "transparent"
						capStyle: ShapePath.RoundCap
						joinStyle: ShapePath.RoundJoin
						PathPolyline { path: [Qt.point(6, 12.5), Qt.point(10.5, 17), Qt.point(18, 7.5)] }
					}
				}
			}
			Label {
				anchors.verticalCenter: parent.verticalCenter
				text: tick.text
				color: Theme.color_font_primary
				font.pixelSize: Theme.font_size_body1
			}
		}
		MouseArea {
			anchors.fill: parent
			anchors.margins: -8
			onClicked: tick.toggled()
		}
	}
}
