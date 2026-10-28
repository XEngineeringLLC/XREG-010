import QtQuick
import QtQuick.Shapes
import Victron.VenusOS

// Home-page tile: the web app's boat speed-performance view (Live Data > Boat > Speed). Percent of the learned
// best-ever speed, the sailing polar or motoring speed-vs-RPM curve evaluated at the present wind (or headwind) and
// sea state, and the live point: green while a steady run is being recorded, orange while settling. Math follows
// script.js (parseFrontCsv, frontEval, foldAwaJS, appToTrue, drawPerfPlot, drawMotorPlot, renderPerf); the bridge
// passes PerfLive, MotorLive and /perfcurve.csv through unchanged as /Perf/*.
Rectangle {
	id: perf

	property string svc
	property bool linkUp
	property bool active
	readonly property color brand: "#00a19a"    // web app --accent, the app's speed number

	radius: Theme.geometry_overviewPage_widget_radius
	color: Theme.color_background_secondary

	VeQuickItem { id: want; uid: perf.svc + "/Ui/Want/Perf" }
	Timer {
		interval: 15000
		repeat: true
		triggeredOnStart: true
		running: perf.active
		onTriggered: want.setValue(Math.floor(Date.now() / 1000))
	}

	VeQuickItem { id: sValid; uid: perf.svc + "/Perf/Live/valid" }
	VeQuickItem { id: sWs; uid: perf.svc + "/Perf/Live/ws" }
	VeQuickItem { id: sWa; uid: perf.svc + "/Perf/Live/wa" }
	VeQuickItem { id: sSpd; uid: perf.svc + "/Perf/Live/spd" }
	VeQuickItem { id: sPct; uid: perf.svc + "/Perf/Live/pct" }
	VeQuickItem { id: sSea; uid: perf.svc + "/Perf/Live/pitchStd" }
	VeQuickItem { id: sSrc; uid: perf.svc + "/Perf/Live/src" }
	VeQuickItem { id: sPts; uid: perf.svc + "/Perf/Live/ptCount" }
	VeQuickItem { id: sSteady; uid: perf.svc + "/Perf/Live/steady" }
	VeQuickItem { id: sSettle; uid: perf.svc + "/Perf/Live/settleSec" }
	VeQuickItem { id: sState; uid: perf.svc + "/Perf/Live/state" }
	VeQuickItem { id: sHours; uid: perf.svc + "/Perf/Live/sailHours" }
	VeQuickItem { id: mValid; uid: perf.svc + "/Perf/Motor/valid" }
	VeQuickItem { id: mRpm; uid: perf.svc + "/Perf/Motor/rpm" }
	VeQuickItem { id: mHw; uid: perf.svc + "/Perf/Motor/headwind" }
	VeQuickItem { id: mSpd; uid: perf.svc + "/Perf/Motor/spd" }
	VeQuickItem { id: mPct; uid: perf.svc + "/Perf/Motor/pct" }
	VeQuickItem { id: mSea; uid: perf.svc + "/Perf/Motor/pitchStd" }
	VeQuickItem { id: mSrc; uid: perf.svc + "/Perf/Motor/src" }
	VeQuickItem { id: mPts; uid: perf.svc + "/Perf/Motor/ptCount" }
	VeQuickItem { id: mSteady; uid: perf.svc + "/Perf/Motor/steady" }
	VeQuickItem { id: mSettle; uid: perf.svc + "/Perf/Motor/settleSec" }
	VeQuickItem { id: mState; uid: perf.svc + "/Perf/Motor/state" }
	VeQuickItem { id: mHours; uid: perf.svc + "/Perf/Motor/motorHours" }
	VeQuickItem {
		id: curveCsv
		uid: perf.svc + "/Perf/CurveCsv"
		onValueChanged: perf.parseCurve()
	}
	Component.onCompleted: parseCurve()

	function num(it) { return perf.linkUp && it.valid && it.value !== null && it.value !== "" ? Number(it.value) : NaN }

	// Sailing or motoring: follows whichever the regulator grades now; the tile toggle pins one for this visit.
	// A boat with no sailing polar and no sailing hours is a motorboat (the firmware never learns sailing for one).
	readonly property bool sailBoat: num(sPts) > 0 || num(sHours) > 0 || num(sValid) >= 0.5
	property int pinned: -1
	property int autoView: num(sHours) > num(mHours) ? 0 : 1
	readonly property int liveView: num(mValid) >= 0.5 ? 1 : num(sValid) >= 0.5 ? 0 : -1
	onLiveViewChanged: if (liveView >= 0) autoView = liveView
	readonly property int view: !sailBoat ? 1 : pinned >= 0 ? pinned : autoView
	property bool trueFrame: false

	readonly property bool lValid: num(view ? mValid : sValid) >= 0.5
	readonly property int lState: isFinite(num(view ? mState : sState)) ? num(view ? mState : sState) | 0 : 3
	readonly property real lPct: num(view ? mPct : sPct)
	readonly property bool graded: lValid && (lState === 0 || lState === 1) && lPct > 0
	readonly property real lSpd: num(view ? mSpd : sSpd)
	readonly property real lSea: num(view ? mSea : sSea)
	readonly property bool lSteady: num(view ? mSteady : sSteady) >= 0.5
	readonly property real lSettle: num(view ? mSettle : sSettle)
	readonly property real lHours: num(view ? mHours : sHours)
	property real settlePeak: 0
	onLSettleChanged: {
		if (!(lSettle > 0) || lSteady)
			settlePeak = 0
		else if (lSettle > settlePeak)
			settlePeak = lSettle
	}

	function stateColor(st) { return st === 0 ? Theme.color_green : st === 1 ? Theme.color_blue : Theme.color_font_secondary }
	readonly property var stateLabels: ["MEASURED", "ESTIMATED", "Learning this operating region", "No reference here yet"]
	function seaWord(s) { return s < 0.5 ? "calm" : s < 1.5 ? "slight" : s < 3 ? "choppy" : s < 5 ? "rough" : "very rough" }

	// ---- best-ever fronts (BEFRONT1 sail + motor blocks), evaluated as script.js frontEval ----
	readonly property var sailScale: [2.0, 12.0, 1.0]     // AWS, AWA, sea: firmware axisScale[]
	readonly property var motorScale: [100.0, 2.0, 1.0]   // RPM, headwind, sea
	property var sailFront: []
	property var motorFront: []
	property bool folded: true
	property int curveRev: 0
	function parseCurve() {
		let sail = [], motor = [], cur = null, naxis = 3
		// read value, not valid: inside a value-change handler valid may not have caught up yet
		const txt = curveCsv.value ? String(curveCsv.value) : ""
		for (const line of txt.split("\n")) {
			const t = line.trim()
			if (!t.length)
				continue
			const c = t.split(",")
			if (c[0] === "BEFRONT1") {
				cur = c[1] === "MOTOR" ? motor : sail
				naxis = (+c[2]) || 3
				continue
			}
			if (!cur || c.length < naxis + 1)
				continue
			let x = []
			for (let a = 0; a < naxis; ++a)
				x.push(+c[a])
			const y = +c[naxis]
			if (x.some(isNaN) || isNaN(y))
				continue
			cur.push({ x: x, y: y })
		}
		sailFront = sail
		motorFront = motor
		// the firmware stores the sailing front folded to 0..180 only when the polar is symmetric
		folded = !sail.some(p => p.x[1] < 0 || p.x[1] > 180)
		curveRev++
		refresh()
	}
	function frontEval(pts, q, sc) {
		let wsum = 0, acc = 0
		for (const pt of pts) {
			let d2 = 0
			for (let a = 0; a < q.length; ++a) {
				const dx = (q[a] - pt.x[a]) / (sc[a] > 1e-9 ? sc[a] : 1)
				d2 += dx * dx
			}
			if (d2 < 1e-12)
				return pt.y
			const w = 1 / (d2 + 1e-9)
			wsum += w
			acc += w * pt.y
		}
		return wsum > 0 ? acc / wsum : 0
	}
	function foldAwa(a) {
		if (!folded)
			return a
		let pa = a
		while (pa < 0) pa += 360
		while (pa >= 360) pa -= 360
		return pa > 180 ? 360 - pa : pa
	}
	function appToTrue(awa, bs, aws) {
		const r = Math.PI / 180, ca = Math.cos(Math.abs(awa) * r)
		const tws = Math.sqrt(Math.max(0, aws * aws + bs * bs - 2 * aws * bs * ca))
		let twa = 0
		if (tws > 1e-6)
			twa = Math.acos(Math.max(-1, Math.min(1, (aws * ca - bs) / tws))) / r
		return awa < 0 ? -twa : twa
	}

	// The curve sweep costs (sweep steps x front points): redo it only when its inputs move noticeably, checked every
	// 2 s and at once on a view, frame or curve change
	property var curve: ({ pts: [], maxS: 4, maxR: 3600 })
	property string curveKey
	function refresh() {
		const sea = Math.round((lSea || 0) * 10) / 10
		const key = view + "|" + curveRev + "|" + trueFrame + "|" + sea + "|"
			+ (view ? Math.round(num(mHw) * 2 || 0) : Math.round(num(sWs) * 2 || 0))
		if (key === curveKey)
			return
		curveKey = key
		let pts = [], maxS = 4, maxR = 3600
		if (view === 0 && sailFront.length >= 2) {
			const aws = num(sWs) || 0
			let lo = 999, hi = -999
			for (const p of sailFront) {
				lo = Math.min(lo, p.x[1])
				hi = Math.max(hi, p.x[1])
			}
			const st = Math.max(1, (hi - lo) / 90)
			for (let a = lo; a <= hi + 1e-6; a += st) {
				const s = frontEval(sailFront, [aws, foldAwa(a), sea], sailScale)
				if (s <= 0)
					continue
				pts.push([trueFrame ? appToTrue(a, s, aws) : a, s])
				maxS = Math.max(maxS, s)
			}
		} else if (view === 1 && motorFront.length >= 2) {
			const hw = num(mHw) || 0
			let lo = 1e9, hi = -1e9
			for (const p of motorFront) {
				lo = Math.min(lo, p.x[0])
				hi = Math.max(hi, p.x[0])
			}
			const st = Math.max(20, (hi - lo) / 120)
			for (let r = lo; r <= hi + 1e-6; r += st) {
				const v = frontEval(motorFront, [r, hw, sea], motorScale)
				if (v <= 0)
					continue
				pts.push([r, v])
				maxS = Math.max(maxS, v)
				maxR = Math.max(maxR, r)
			}
		}
		curve = { pts: pts, maxS: maxS, maxR: maxR }
	}
	onViewChanged: refresh()
	onTrueFrameChanged: refresh()
	Timer {
		interval: 2000
		repeat: true
		triggeredOnStart: true
		running: perf.visible
		onTriggered: perf.refresh()
	}
	readonly property bool haveCurve: curve.pts.length > 1
	readonly property real maxS: Math.ceil(Math.max(curve.maxS, lValid && lSpd > 0 ? lSpd : 0))
	readonly property real maxR: Math.ceil(Math.max(curve.maxR, view === 1 && lValid ? num(mRpm) || 0 : 0) / 500) * 500

	// ---- layout: header, then the app's four corners around the plot ----
	readonly property real k: Math.max(0.7, Math.min(1, width / 430, height / 348))
	function fs(px, floor) { return Math.max(floor, Math.round(px * k)) }
	readonly property real headH: 40
	readonly property real side: Math.round(84 * k)
	readonly property real toggleH: 28
	readonly property real plotS: Math.max(60, Math.min(height - headH - toggleH - 10, width - 20 - 2 * side))

	Label {
		x: 12
		y: 10
		text: "Speed"
		color: Theme.color_font_secondary
		font.pixelSize: Theme.font_size_caption
	}
	// Settle status: Settled, or Settling with the seconds left before recording starts
	Row {
		x: 72
		y: 8
		height: 22
		spacing: 6
		visible: isFinite(perf.lSettle) && perf.lSettle >= 0
		Item {
			width: 18
			height: 18
			anchors.verticalCenter: parent.verticalCenter
			Rectangle {
				anchors.centerIn: parent
				visible: perf.lSteady || !(perf.lSettle > 0)
				width: 10; height: 10; radius: 5
				color: perf.lSteady ? Theme.color_green : Theme.color_orange
			}
			Shape {
				anchors.fill: parent
				visible: !perf.lSteady && perf.lSettle > 0
				preferredRendererType: Shape.CurveRenderer
				ShapePath {
					strokeColor: Qt.rgba(Theme.color_orange.r, Theme.color_orange.g, Theme.color_orange.b, 0.3)
					strokeWidth: 2.5
					fillColor: "transparent"
					PathAngleArc { centerX: 9; centerY: 9; radiusX: 7; radiusY: 7; startAngle: 0; sweepAngle: 360 }
				}
				ShapePath {
					strokeColor: Theme.color_orange
					strokeWidth: 2.5
					fillColor: "transparent"
					capStyle: ShapePath.RoundCap
					PathAngleArc {
						centerX: 9; centerY: 9; radiusX: 7; radiusY: 7; startAngle: -90
						sweepAngle: perf.settlePeak > 0 ? 360 * perf.lSettle / perf.settlePeak : 0
					}
				}
			}
			Label {
				anchors.centerIn: parent
				visible: !perf.lSteady && perf.lSettle > 0
				text: Math.ceil(perf.lSettle)
				font.pixelSize: 9
				font.bold: true
			}
		}
		Label {
			anchors.verticalCenter: parent.verticalCenter
			text: perf.lSteady ? "Settled" : "Settling"
			color: perf.lSteady ? Theme.color_green : Theme.color_orange
			font.pixelSize: Theme.font_size_caption
			font.bold: true
		}
	}
	SegmentedButtonRow {
		anchors { right: parent.right; rightMargin: 8; top: parent.top; topMargin: 6 }
		width: Math.round(190 * perf.k)
		height: 30
		visible: perf.sailBoat
		fontPixelSize: Theme.font_size_tiny
		model: [{ value: "Sailing" }, { value: "Motoring" }]
		currentIndex: perf.view
		onButtonClicked: function(buttonIndex) { perf.pinned = buttonIndex }
	}

	Item {
		id: body
		x: 10
		y: perf.headH
		width: perf.width - 20
		height: perf.height - perf.headH - 8

		// Top left: conditions
		Column {
			width: perf.side
			Label {
				width: parent.width
				wrapMode: Text.WordWrap
				text: perf.view ? "Engine RPM" : "Apparent wind"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
			Label {
				text: !perf.lValid ? "--" : perf.view ? Math.round(perf.num(mRpm)) : Math.round(perf.num(sWs) || 0) + " kn"
				font.pixelSize: perf.fs(26, 18)
				font.bold: true
			}
			Label {
				width: parent.width
				wrapMode: Text.WordWrap
				text: {
					if (!perf.lValid)
						return ""
					if (perf.view) {
						const hw = perf.num(mHw) || 0
						return Math.round(Math.abs(hw)) + (hw < 0 ? " kn tailwind" : " kn headwind")
					}
					return Math.round(perf.num(sWa) || 0) + "°"
				}
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
		}
		// Top right: sea state
		Column {
			anchors.right: parent.right
			width: perf.side
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: "Sea state"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: isFinite(perf.lSea) ? perf.lSea.toFixed(1) + "°" : "--"
				font.pixelSize: perf.fs(26, 18)
				font.bold: true
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: isFinite(perf.lSea) ? perf.seaWord(perf.lSea) : ""
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
		}
		// Bottom left: % of best and what it is based on
		Column {
			anchors.bottom: parent.bottom
			width: perf.side
			Label {
				text: perf.graded ? Math.round(perf.lPct) + "%" : "--"
				color: perf.graded ? perf.stateColor(perf.lState) : Theme.color_font_primary
				font.pixelSize: perf.fs(34, 24)
				font.bold: true
			}
			Label {
				text: "of best"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(14, 12)
			}
			Label {
				width: parent.width
				wrapMode: Text.WordWrap
				text: perf.lValid ? perf.stateLabels[perf.lState] || "" : ""
				color: perf.stateColor(perf.lState)
				font.pixelSize: perf.fs(12, 11)
			}
		}
		// Bottom right: data maturity
		Column {
			anchors { right: parent.right; bottom: parent.bottom }
			width: perf.side
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: isFinite(perf.num(perf.view ? mPts : sPts)) ? perf.num(perf.view ? mPts : sPts) | 0 : "--"
				font.pixelSize: perf.fs(20, 16)
				font.bold: true
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				wrapMode: Text.WordWrap
				text: "best surface points"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(12, 11)
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: !isFinite(perf.lHours) ? "--" : perf.lHours >= 10 ? Math.round(perf.lHours) : perf.lHours.toFixed(1)
				font.pixelSize: perf.fs(20, 16)
				font.bold: true
			}
			Label {
				width: parent.width
				horizontalAlignment: Text.AlignRight
				text: perf.view ? "motoring hours" : "sailing hours"
				color: Theme.color_font_secondary
				font.pixelSize: perf.fs(12, 11)
			}
		}

		// Plot box, centred between the corner columns
		Item {
			id: box
			x: (body.width - width) / 2
			y: 0
			width: perf.plotS
			height: perf.plotS

			// Sailing polar: rings, spokes every 45 degrees, best-ever line (mirrored when symmetric), live dot
			Item {
				id: polar
				anchors.fill: parent
				visible: perf.view === 0
				readonly property real cx: width / 2
				readonly property real cy: height * 0.54
				readonly property real rr: Math.min(width, height) * 0.42
				function pt(ang, s) {
					const r = s / perf.maxS * rr, a = ang * Math.PI / 180
					return Qt.point(cx + r * Math.sin(a), cy - r * Math.cos(a))
				}
				Repeater {
					model: 4
					delegate: Item {
						required property int index
						readonly property real r: polar.rr * (index + 1) / 4
						Rectangle {
							x: polar.cx - parent.r
							y: polar.cy - parent.r
							width: 2 * parent.r
							height: width
							radius: parent.r
							color: "transparent"
							border.width: 1
							border.color: Theme.color_card_separator
						}
						Label {
							x: polar.cx + 3
							y: polar.cy - parent.r + 1
							text: Math.round(perf.maxS * (index + 1) / 4) + " kn"
							color: Theme.color_font_disabled
							font.pixelSize: 10
						}
					}
				}
				Shape {
					anchors.fill: parent
					preferredRendererType: Shape.CurveRenderer
					ShapePath {
						strokeColor: Theme.color_card_separator
						strokeWidth: 1
						fillColor: "transparent"
						PathMultiline {
							paths: {
								let p = []
								for (const dg of [0, 45, 90, 135, 180, -45, -90, -135])
									p.push([Qt.point(polar.cx, polar.cy), polar.pt(dg, perf.maxS)])
								return p
							}
						}
					}
					ShapePath {
						strokeColor: Theme.color_blue
						strokeWidth: 2.2
						fillColor: "transparent"
						joinStyle: ShapePath.RoundJoin
						capStyle: ShapePath.RoundCap
						PathMultiline {
							paths: {
								if (perf.view !== 0 || !perf.haveCurve)
									return []
								const one = perf.curve.pts.map(q => polar.pt(q[0], q[1]))
								return perf.folded ? [one, perf.curve.pts.map(q => polar.pt(-q[0], q[1]))] : [one]
							}
						}
					}
				}
				Label {
					anchors.horizontalCenter: parent.horizontalCenter
					y: polar.cy - polar.rr - height - 1
					text: "0° into wind"
					color: Theme.color_font_secondary
					font.pixelSize: 10
				}
				Label {
					anchors.horizontalCenter: parent.horizontalCenter
					y: polar.cy + polar.rr + 1
					text: "180°"
					color: Theme.color_font_secondary
					font.pixelSize: 10
				}
				Label {
					anchors.centerIn: parent
					anchors.verticalCenterOffset: polar.rr * 0.5
					visible: !perf.haveCurve
					text: "No sailing data yet"
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_caption
				}
				Rectangle {
					readonly property point at: polar.pt(perf.trueFrame ? perf.appToTrue(perf.num(sWa) || 0, perf.lSpd || 0, perf.num(sWs) || 0)
						: perf.num(sWa) || 0, perf.lSpd || 0)
					visible: perf.view === 0 && perf.lValid
					x: at.x - width / 2
					y: at.y - height / 2
					width: 13; height: 13; radius: 6.5
					color: perf.lSteady ? Theme.color_green : Theme.color_orange
					border.width: 2
					border.color: Theme.color_white
				}
				Column {
					x: polar.cx - width / 2
					y: polar.cy - height / 2
					Label {
						anchors.horizontalCenter: parent.horizontalCenter
						text: perf.lValid && isFinite(perf.lSpd) ? perf.lSpd.toFixed(1) : "--"
						color: perf.brand
						font.pixelSize: perf.fs(38, 26)
						font.bold: true
					}
					Label {
						anchors.horizontalCenter: parent.horizontalCenter
						text: perf.num(sSrc) >= 1.5 ? "KN · SOG" : "KN · STW"
						color: Theme.color_font_secondary
						font.pixelSize: 10
						font.bold: true
					}
				}
			}

			// Motoring map: best-ever boat speed against RPM at the present headwind and sea state, live dot
			Item {
				id: motor
				anchors.fill: parent
				visible: perf.view === 1
				readonly property real padL: 38
				readonly property real padB: 16
				readonly property real cw: width - padL - 4
				readonly property real ch: height - padB - 4
				readonly property real rStep: Math.max(500, Math.ceil(perf.maxR / 6 / 500) * 500)
				readonly property real sStep: Math.max(1, Math.ceil(perf.maxS / 5))
				function mx(r) { return padL + r / perf.maxR * cw }
				function my(s) { return 4 + ch - s / perf.maxS * ch }
				Repeater {
					model: Math.floor(perf.maxR / motor.rStep) + 1
					delegate: Item {
						required property int index
						Rectangle {
							x: motor.mx(index * motor.rStep)
							y: 4
							width: 1
							height: motor.ch
							color: Theme.color_card_separator
						}
						Label {
							x: motor.mx(index * motor.rStep) - width / 2
							y: motor.height - motor.padB + 2
							text: index * motor.rStep
							color: Theme.color_font_disabled
							font.pixelSize: 10
						}
					}
				}
				Repeater {
					model: Math.floor(perf.maxS / motor.sStep) + 1
					delegate: Item {
						required property int index
						Rectangle {
							x: motor.padL
							y: motor.my(index * motor.sStep)
							width: motor.cw
							height: 1
							color: Theme.color_card_separator
						}
						Label {
							x: motor.padL - width - 3
							y: motor.my(index * motor.sStep) - height / 2
							text: index * motor.sStep + (index === Math.floor(perf.maxS / motor.sStep) ? " kn" : "")
							color: Theme.color_font_disabled
							font.pixelSize: 10
						}
					}
				}
				Shape {
					anchors.fill: parent
					preferredRendererType: Shape.CurveRenderer
					ShapePath {
						strokeColor: Theme.color_blue
						strokeWidth: 2.2
						fillColor: "transparent"
						joinStyle: ShapePath.RoundJoin
						capStyle: ShapePath.RoundCap
						PathPolyline {
							path: perf.view === 1 && perf.haveCurve ? perf.curve.pts.map(q => Qt.point(motor.mx(q[0]), motor.my(q[1]))) : []
						}
					}
				}
				Label {
					anchors.centerIn: parent
					visible: !perf.haveCurve
					text: "No motoring data yet"
					color: Theme.color_font_secondary
					font.pixelSize: Theme.font_size_caption
				}
				Rectangle {
					visible: perf.view === 1 && perf.lValid
					x: motor.mx(perf.num(mRpm) || 0) - width / 2
					y: motor.my(perf.lSpd || 0) - height / 2
					width: 13; height: 13; radius: 6.5
					color: perf.lSteady ? Theme.color_green : Theme.color_orange
					border.width: 2
					border.color: Theme.color_white
				}
				Column {
					x: motor.padL + 8
					y: 8
					Label {
						text: perf.lValid && isFinite(perf.lSpd) ? perf.lSpd.toFixed(1) : "--"
						color: perf.brand
						font.pixelSize: perf.fs(34, 24)
						font.bold: true
					}
					Label {
						text: perf.num(mSrc) >= 1.5 ? "KN · SOG" : "KN · STW"
						color: Theme.color_font_secondary
						font.pixelSize: 10
						font.bold: true
					}
				}
			}
		}

		// Under the plot: the polar's wind frame (Apparent is the front's own frame), or the motoring x axis name
		Row {
			anchors.horizontalCenter: box.horizontalCenter
			y: box.height + 4
			spacing: 0
			visible: perf.view === 0
			Repeater {
				model: ["Apparent", "True"]
				delegate: Rectangle {
					required property int index
					required property string modelData
					readonly property bool isOn: (index === 1) === perf.trueFrame
					width: frameLabel.implicitWidth + 18
					height: perf.toggleH - 6
					radius: 4
					color: isOn ? Theme.color_ok : "transparent"
					border.width: 1
					border.color: Theme.color_ok
					Label {
						id: frameLabel
						anchors.centerIn: parent
						text: parent.modelData
						color: parent.isOn ? Theme.color_button_down_text : Theme.color_font_secondary
						font.pixelSize: 12
					}
					MouseArea {
						anchors.fill: parent
						anchors.margins: -4
						onClicked: perf.trueFrame = parent.index === 1
					}
				}
			}
		}
		Label {
			anchors.horizontalCenter: box.horizontalCenter
			y: box.height + 4
			visible: perf.view === 1
			text: "RPM"
			color: Theme.color_font_secondary
			font.pixelSize: 12
		}
	}
}
