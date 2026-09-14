import SwiftUI

extension LPCubicBezier {
    /// The eased value at progress `x`, 0 to 1.
    public func value(at x: Double) -> Double {
        let x = min(max(x, 0), 1)
        func bezier(_ t: Double, _ p1: Double, _ p2: Double) -> Double {
            let u = 1 - t
            return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t
        }
        var lo = 0.0, hi = 1.0, t = x
        for _ in 0..<40 {
            let bx = bezier(t, x1, x2)
            if abs(bx - x) < 1e-6 { break }
            if bx < x { lo = t } else { hi = t }
            t = (lo + hi) / 2
        }
        return bezier(t, y1, y2)
    }
}

/// A liquid bead, as maryui's lp_bubble paints a traffic light: a pearly shell, two liquid blobs frozen at
/// `phase` seconds of their roll, a gloss, a rim and a drop shadow. Nothing here runs on a clock.
public struct LiquidBead: View {
    public struct Colors: Equatable, Sendable {
        public var base: LPColor
        public var deep: LPColor
        public var light: LPColor

        public init(base: LPColor, deep: LPColor, light: LPColor) {
            self.base = base
            self.deep = deep
            self.light = light
        }

        public static let close = Colors(base: LP.Traffic.Close.base, deep: LP.Traffic.Close.deep, light: LP.Traffic.Close.light)
        public static let minimize = Colors(base: LP.Traffic.Minimize.base, deep: LP.Traffic.Minimize.deep, light: LP.Traffic.Minimize.light)
        public static let zoom = Colors(base: LP.Traffic.Zoom.base, deep: LP.Traffic.Zoom.deep, light: LP.Traffic.Zoom.light)
        public static let inactive = Colors(base: LP.Traffic.inactive, deep: LP.Traffic.inactiveDeep, light: LP.Platinum._1)

        /// Pressed: every stop moves 35% toward the one below it.
        var pressed: Colors {
            Colors(base: base.mixed(with: deep, 0.35), deep: deep.mixed(with: .black, 0.35), light: light.mixed(with: base, 0.35))
        }
    }

    var colors: Colors
    var size: CGFloat
    var fill: Double
    var phase: Double
    var glyph: String?

    public init(colors: Colors, size: CGFloat = LP.Size.traffic, fill: Double = LP.Liquid.fillTraffic, phase: Double = 0, glyph: String? = nil) {
        self.colors = colors
        self.size = size
        self.fill = fill
        self.phase = phase
        self.glyph = glyph
    }

    /// Ping-pong progress of a roll `period` seconds long, eased like the web's keyframes.
    static func roll(_ time: Double, period: Double, reversed: Bool) -> Double {
        let f = (time.truncatingRemainder(dividingBy: 2 * period)) / period
        let p = f <= 1 ? f : 2 - f
        return LPCubicBezier(x1: 0.42, y1: 0, x2: 0.58, y2: 1).value(at: reversed ? 1 - p : p)
    }

    public var body: some View {
        let s = size
        ZStack {
            Circle().fill(RadialGradient(stops: [
                .init(color: colors.light.mixed(with: .white, 0.6).color, location: 0),
                .init(color: colors.base.mixed(with: LP.Platinum._2, 0.82).color, location: 0.88),
            ], center: UnitPoint(x: 0.5, y: 0.32), startRadius: 0, endRadius: s * hypot(0.5, 0.68)))
            blob(top: (1 - fill) * s - 0.05 * s, radii: (0.46, 0.41, 0.44, 0.40), period: LP.Liquid.wavePeriod * 1.55,
                 reversed: true, brighten: 0.15, opacity: LP.Liquid.opacityBack, surfaceLight: false)
            blob(top: (1 - fill) * s, radii: (0.44, 0.46, 0.40, 0.42), period: LP.Liquid.wavePeriod,
                 reversed: false, brighten: 0, opacity: LP.Liquid.opacityFront, surfaceLight: true)
            Ellipse()
                .fill(LinearGradient(colors: [LP.Traffic.gloss.color, LP.Traffic.gloss.opacity(0).color], startPoint: .top, endPoint: .bottom))
                .frame(width: 0.46 * s, height: 0.32 * s)
                .position(x: (0.18 + 0.23) * s, y: (0.08 + 0.16) * s)
            InsetShadow(Circle(), color: LP.Traffic.rim, blur: 2, y: 1)
            Circle().stroke(Color.black.opacity(0.25), lineWidth: 1).mask(Circle())
            if let glyph {
                Text(glyph)
                    .font(.system(size: 0.6 * s, weight: .bold))
                    .foregroundStyle(Color.black.opacity(0.5))
                    .lpEmbossed(LPColor.white.opacity(0.8))
            }
        }
        .frame(width: s, height: s)
        .clipShape(Circle())
        .background(Circle().fill(Color.black).shadow(color: LP.Traffic.beadShadow.color, radius: 1, x: 0, y: 1))
    }

    private func blob(top: Double, radii: (Double, Double, Double, Double), period: Double, reversed: Bool, brighten: Double,
                      opacity: Double, surfaceLight: Bool) -> some View {
        let s = Double(size), width = 2 * s
        let p = Self.roll(phase, period: period, reversed: reversed)
        let amplitude = LP.Liquid.waveAmplitude * s
        let stops = [colors.light, colors.base, colors.deep].map { $0.mixed(with: .white, brighten).color }
        return ZStack(alignment: .top) {
            UnevenRoundedRectangle(topLeadingRadius: radii.0 * width, bottomLeadingRadius: radii.3 * width,
                                   bottomTrailingRadius: radii.2 * width, topTrailingRadius: radii.1 * width)
                .fill(RadialGradient(stops: [.init(color: stops[0], location: 0), .init(color: stops[1], location: 0.42),
                                             .init(color: stops[2], location: 1)],
                                     center: UnitPoint(x: 0.4, y: 0.3), startRadius: 0, endRadius: 0.7 * width))
            if surfaceLight {
                LinearGradient(colors: [LP.Liquid.surfaceLight.color, LP.Liquid.surfaceLight.opacity(0).color], startPoint: .top, endPoint: .bottom)
                    .frame(height: 0.16 * width)
            }
        }
        .frame(width: width, height: width)
        .opacity(opacity)
        .rotationEffect(.degrees(-4 + 8 * p))
        .position(x: -0.5 * s + width / 2 + amplitude * (2 * p - 1), y: top + width / 2)
    }
}

/// Close, minimize and zoom: 18 pt beads 28 pt apart. The glyphs show while the pointer is over the group;
/// an inactive window's beads drain to platinum until then.
public struct TrafficLights: View {
    var onClose: () -> Void
    var onMinimize: () -> Void
    var onZoom: () -> Void
    var zoomed: Bool
    @Environment(\.lpWindowActive) private var active
    @State private var hovering = false

    public init(zoomed: Bool = false, onClose: @escaping () -> Void, onMinimize: @escaping () -> Void, onZoom: @escaping () -> Void) {
        self.zoomed = zoomed
        self.onClose = onClose
        self.onMinimize = onMinimize
        self.onZoom = onZoom
    }

    public var body: some View {
        HStack(spacing: LP.Size.trafficGap) {
            light(.close, glyph: "×", phase: 0, action: onClose)
            light(.minimize, glyph: "–", phase: 2.3, action: onMinimize)
            light(.zoom, glyph: zoomed ? "−" : "+", phase: 4.1, action: onZoom)
        }
        .onHover { inside in
            withAnimation(.easeOut(duration: LP.Motion.fast)) { hovering = inside }
        }
    }

    private func light(_ colors: LiquidBead.Colors, glyph: String, phase: Double, action: @escaping () -> Void) -> some View {
        Button(action: action) { EmptyView() }
            .buttonStyle(BeadStyle(colors: active || hovering ? colors : .inactive, glyph: hovering ? glyph : nil, phase: phase))
    }
}

private struct BeadStyle: ButtonStyle {
    var colors: LiquidBead.Colors
    var glyph: String?
    var phase: Double

    func makeBody(configuration: Configuration) -> some View {
        BeadButton(colors: colors, glyph: glyph, phase: phase, pressed: configuration.isPressed)
    }
}

private struct BeadButton: View {
    var colors: LiquidBead.Colors
    var glyph: String?
    var phase: Double
    var pressed: Bool
    @State private var hovering = false

    var body: some View {
        LiquidBead(colors: pressed ? colors.pressed : colors, fill: hovering ? 1.12 : LP.Liquid.fillTraffic, phase: phase, glyph: glyph)
            .animation(.easeInOut(duration: LP.Motion.slow), value: hovering)
            .animation(.easeOut(duration: pressed ? LP.Motion.fast : 0.2), value: pressed)
            .contentShape(Circle())
            .onHover { hovering = $0 }
    }
}
