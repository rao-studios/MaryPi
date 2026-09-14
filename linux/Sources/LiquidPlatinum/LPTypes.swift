import CoreGraphics
import SwiftUI

/// A token color: sRGB components and alpha, 0 to 1.
public struct LPColor: Equatable, Sendable {
    public var red: Double
    public var green: Double
    public var blue: Double
    public var alpha: Double

    public init(red: Double, green: Double, blue: Double, alpha: Double) {
        self.red = red
        self.green = green
        self.blue = blue
        self.alpha = alpha
    }

    public var color: Color { Color(.sRGB, red: red, green: green, blue: blue, opacity: alpha) }
    public var cgColor: CGColor { CGColor(srgbRed: red, green: green, blue: blue, alpha: alpha) }

    public func opacity(_ alpha: Double) -> LPColor {
        LPColor(red: red, green: green, blue: blue, alpha: alpha)
    }
}

/// One layer of a CSS box-shadow token.
public struct LPShadowLayer: Equatable, Sendable {
    public var inset: Bool
    public var x: CGFloat
    public var y: CGFloat
    public var blur: CGFloat
    public var spread: CGFloat
    public var color: LPColor

    public init(inset: Bool, x: CGFloat, y: CGFloat, blur: CGFloat, spread: CGFloat, color: LPColor) {
        self.inset = inset
        self.x = x
        self.y = y
        self.blur = blur
        self.spread = spread
        self.color = color
    }
}

/// A CSS cubic-bezier easing.
public struct LPCubicBezier: Equatable, Sendable {
    public var x1: Double
    public var y1: Double
    public var x2: Double
    public var y2: Double

    public init(x1: Double, y1: Double, x2: Double, y2: Double) {
        self.x1 = x1
        self.y1 = y1
        self.x2 = x2
        self.y2 = y2
    }

    public func animation(duration: TimeInterval) -> Animation {
        .timingCurve(x1, y1, x2, y2, duration: duration)
    }
}

/// A token as `LP_TOKENS[]` lists it.
public struct LPTokenInfo: Equatable, Sendable {
    public let name: String
    public let type: String
    public let value: String

    public init(_ name: String, _ type: String, _ value: String) {
        self.name = name
        self.type = type
        self.value = value
    }
}
