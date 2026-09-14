import SwiftUI

/// Liquid Platinum's accent appearance: the five roles that follow the Blue or Graphite setting.
public struct LPAccent: Equatable, Sendable {
    public var base: LPColor
    public var deep: LPColor
    public var light: LPColor
    public var soft: LPColor
    public var focusRing: LPColor

    public static let blue = LPAccent(base: LP.Accent.Blue.base, deep: LP.Accent.Blue.deep, light: LP.Accent.Blue.light,
                                      soft: LP.Accent.Blue.soft, focusRing: LP.Accent.Blue.focusRing)
    public static let graphite = LPAccent(base: LP.Accent.Graphite.base, deep: LP.Accent.Graphite.deep, light: LP.Accent.Graphite.light,
                                          soft: LP.Accent.Graphite.soft, focusRing: LP.Accent.Graphite.focusRing)
}

/// The name of the coordinate space a window's root view declares, so a surface knows where it sits.
public let lpWindowSpace = "lp.window"

extension EnvironmentValues {
    /// The window's top-left corner on the screen, in points with y running down. The grain is sampled at
    /// screen coordinates, so it holds still on the desktop while windows move over it: one sheet of metal.
    @Entry public var lpGrainOrigin: CGPoint = .zero
    @Entry public var lpAccent: LPAccent = .blue
    /// Whether the window is key; inactive chrome drains.
    @Entry public var lpWindowActive: Bool = true
}

extension LPColor {
    /// `a` moved `t` of the way toward `b`.
    public func mixed(with other: LPColor, _ t: Double) -> LPColor {
        LPColor(red: red + (other.red - red) * t, green: green + (other.green - green) * t,
                blue: blue + (other.blue - blue) * t, alpha: alpha + (other.alpha - alpha) * t)
    }

    public static let white = LPColor(red: 1, green: 1, blue: 1, alpha: 1)
    public static let black = LPColor(red: 0, green: 0, blue: 0, alpha: 1)
}

/// The grain: the brush tile laid with the overlay blend at `opacity`, sampled at screen coordinates.
public struct BrushGrain: View {
    public var opacity: Double
    @Environment(\.lpGrainOrigin) private var origin

    public init(opacity: Double = LP.Brush.opacity) {
        self.opacity = opacity
    }

    public var body: some View {
        GeometryReader { geometry in
            if let tile = BrushTile.image {
                let frame = geometry.frame(in: .named(lpWindowSpace))
                let size = BrushTile.pointSize
                let ox = Self.wrap((origin.x + frame.minX).rounded(), size)
                let oy = Self.wrap((origin.y + frame.minY).rounded(), size)
                Image(decorative: tile, scale: CGFloat(BrushTile.imageScale))
                    .resizable(resizingMode: .tile)
                    .frame(width: geometry.size.width + size, height: geometry.size.height + size)
                    .offset(x: -ox, y: -oy)
                    .frame(width: geometry.size.width, height: geometry.size.height, alignment: .topLeading)
                    .clipped()
            }
        }
        .blendMode(.overlay)
        .opacity(opacity)
        .allowsHitTesting(false)
    }

    static func wrap(_ value: CGFloat, _ size: CGFloat) -> CGFloat {
        let r = fmod(value, size)
        return r < 0 ? r + size : r
    }
}

/// A CSS inset box-shadow inside a shape: a ring outside the shape casts the shadow in, and the shape masks
/// the ring away.
public struct InsetShadow: View {
    var shape: AnyShape
    var color: LPColor
    var blur: CGFloat
    var x: CGFloat
    var y: CGFloat

    public init(_ shape: some Shape, color: LPColor, blur: CGFloat = 0, x: CGFloat = 0, y: CGFloat) {
        self.shape = AnyShape(shape)
        self.color = color
        self.blur = blur
        self.x = x
        self.y = y
    }

    public var body: some View {
        GeometryReader { geometry in
            let rect = CGRect(origin: .zero, size: geometry.size)
            let margin = blur * 2 + abs(x) + abs(y) + 4
            Path { path in
                path.addRect(rect.insetBy(dx: -margin, dy: -margin))
                path.addPath(shape.path(in: rect))
            }
            .fill(Color.black, style: FillStyle(eoFill: true))
            .shadow(color: color.color, radius: blur / 2, x: x, y: y)
        }
        .mask(shape)
        .allowsHitTesting(false)
    }
}

/// `emboss-raised`: a white line inside the top edge and a dark one inside the bottom.
public struct RaisedEmboss: View {
    var shape: AnyShape

    public init(_ shape: some Shape) {
        self.shape = AnyShape(shape)
    }

    public var body: some View {
        ZStack {
            InsetShadow(shape, color: LP.Edge.light, y: 1)
            InsetShadow(shape, color: LP.Edge.dark, y: -1)
        }
    }
}

/// `emboss-well`: a soft shadow under the top edge and a faint ring.
public struct WellEmboss: View {
    var shape: AnyShape

    public init(_ shape: some Shape) {
        self.shape = AnyShape(shape)
    }

    public var body: some View {
        ZStack {
            InsetShadow(shape, color: LPColor.black.opacity(0.28), blur: 3, y: 1)
            shape.stroke(Color.black.opacity(0.12), lineWidth: 2).mask(shape)
        }
        .allowsHitTesting(false)
    }
}

/// The surfaces of MaryUI's Surface component.
public enum LPSurface: Sendable, Equatable {
    /// Window chrome and toolbars.
    case flat
    case titlebar(active: Bool)
    case raised
    case bar
    case well
    case body

    var top: LPColor {
        switch self {
        case .flat: LP.Surface.windowTop
        case let .titlebar(active): active ? LP.Surface.titlebarTop : LP.Surface.titlebarInactiveTop
        case .raised: LP.Surface.raisedTop
        case .bar: LP.Surface.menubarTop
        case .well: LP.Surface.well
        case .body: LP.Surface.body
        }
    }

    var bottom: LPColor {
        switch self {
        case .flat: LP.Surface.windowBottom
        case let .titlebar(active): active ? LP.Surface.titlebarBottom : LP.Surface.titlebarInactiveBottom
        case .raised: LP.Surface.raisedBottom
        case .bar: LP.Surface.menubarBottom
        case .well: LP.Surface.well
        case .body: LP.Surface.body
        }
    }

    var grain: Double {
        switch self {
        case .well: LP.Brush.opacity * 0.35
        case .body: LP.Brush.opacity * 0.25
        default: LP.Brush.opacity
        }
    }
}

/// The moving light: a band along `sheen.angle` across a surface, lit by the screen blend. `position` is
/// where the room's light falls across the surface, 0 at its left edge and 1 at its right.
public struct SheenBand: View {
    var position: CGFloat
    var active: Bool

    public init(position: CGFloat, active: Bool = true) {
        self.position = position
        self.active = active
    }

    public var body: some View {
        GeometryReader { geometry in
            let w = geometry.size.width, h = geometry.size.height
            let bandW = 2 * w, bandH = 1.6 * h
            let radians = Double(LP.Sheen.angle) * .pi / 180
            let dx = sin(radians), dy = -cos(radians)
            let length = abs(Double(bandW) * dx) + abs(Double(bandH) * dy)
            let half = length / 2
            let start = UnitPoint(x: 0.5 - dx * half / Double(bandW), y: 0.5 - dy * half / Double(bandH))
            let end = UnitPoint(x: 0.5 + dx * half / Double(bandW), y: 0.5 + dy * half / Double(bandH))
            let white = LP.Sheen.color
            Rectangle()
                .fill(LinearGradient(stops: [
                    .init(color: white.opacity(0).color, location: 0.36),
                    .init(color: white.color, location: 0.5),
                    .init(color: white.opacity(0).color, location: 0.64),
                ], startPoint: start, endPoint: end))
                .frame(width: bandW, height: bandH)
                .position(x: w / 2 + (position - 0.5) * w, y: h / 2)
        }
        .blendMode(.screen)
        .opacity(active ? LP.Sheen.alpha : LP.Sheen.alphaInactive)
        .allowsHitTesting(false)
    }

    /// Where the room's light (`sheen.light-x` of the screen's width) falls across a window.
    public static func position(windowX: CGFloat, windowWidth: CGFloat, screenWidth: CGFloat) -> CGFloat {
        guard windowWidth > 0 else { return 0.5 }
        return min(max((CGFloat(LP.Sheen.lightX) * screenWidth - windowX) / windowWidth, -0.3), 1.3)
    }
}

/// A brushed surface in a shape: the gradient, the grain, an optional sheen, and the surface's emboss.
public struct BrushedSurface: View {
    var surface: LPSurface
    var shape: AnyShape
    var sheen: CGFloat?

    public init(_ surface: LPSurface, in shape: some Shape = Rectangle(), sheen: CGFloat? = nil) {
        self.surface = surface
        self.shape = AnyShape(shape)
        self.sheen = sheen
    }

    public var body: some View {
        ZStack {
            shape.fill(LinearGradient(colors: [surface.top.color, surface.bottom.color], startPoint: .top, endPoint: .bottom))
            BrushGrain(opacity: surface.grain).clipShape(shape)
            if let sheen {
                SheenBand(position: sheen, active: surface != .titlebar(active: false)).clipShape(shape)
            }
            switch surface {
            case .well: WellEmboss(shape)
            case .body: EmptyView()
            default: RaisedEmboss(shape)
            }
        }
        .compositingGroup()
    }
}

extension View {
    /// Text on metal: the ink over a white impression one point below.
    public func lpEmbossed(_ emboss: LPColor = LP.Ink.emboss) -> some View {
        shadow(color: emboss.color, radius: 0, x: 0, y: 1)
    }

    /// Text on the accent: a dark impression one point below.
    public func lpOnAccent() -> some View {
        shadow(color: Color.black.opacity(0.22), radius: 0, x: 0, y: 1)
    }
}
