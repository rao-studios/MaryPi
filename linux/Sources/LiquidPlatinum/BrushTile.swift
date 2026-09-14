import CoreGraphics
import Foundation

/// The brushed grain: a square gray tile of stitched turbulence, rotated along the brush angle and pressed
/// toward mid-gray, drawn exactly as maryui's `lp_brush_tile_render` draws it (and as the web's
/// feTurbulence filter does). Surfaces lay it over their gradient with the overlay blend at `brush.opacity`.
public enum BrushTile {
    public struct Parameters: Sendable, Equatable {
        public var frequencyX: Double
        public var frequencyY: Double
        public var octaves: Int
        public var seed: Int
        public var size: Int
        public var rise: Int
        public var run: Int
        public var contrast: Double
    }

    /// The tokens as the C desktop reads them. Its token macros are float literals (`0.02f`) widened into
    /// doubles, so the same widening here keeps the two tiles byte for byte the same.
    public static let tokens = Parameters(
        frequencyX: Double(Float(LP.Brush.freqX)), frequencyY: Double(Float(LP.Brush.freqY)), octaves: Int(LP.Brush.octaves),
        seed: Int(LP.Brush.seed), size: Int(LP.Brush.tile), rise: Int(LP.Brush.Angle.rise), run: Int(LP.Brush.Angle.run),
        contrast: Double(Float(LP.Brush.contrast))
    )

    /// One gray byte per pixel, rows top down. `scale` 2 samples the same pattern at half-pixel steps, for
    /// Retina; scale 1 is the C desktop's tile exactly.
    public static func grayBytes(_ p: Parameters = tokens, scale: Int = 1) -> [UInt8] {
        let n = p.size * scale
        var out = [UInt8](repeating: 0, count: n * n)
        // The pattern space: the noise repeats every `period` in x and y, and the whole pattern is rotated
        // by -angle. The rotated lattice contains (n, 0) and (0, n), so the tile repeats seamlessly.
        let period = Double(p.size) / Double(p.rise * p.rise + p.run * p.run).squareRoot()
        let degrees = atan2(Double(p.rise), Double(p.run)) * 180.0 / Double.pi
        let angle = -degrees * Double.pi / 180.0
        let c = cos(-angle), s = sin(-angle)
        let tile = Turbulence.Tile(x: 0, y: 0, width: period, height: period)
        let slope = p.contrast, intercept = (1 - slope) / 2
        let turbulence = Turbulence(seed: p.seed)
        func channel(_ k: Int, _ u: Double, _ v: Double) -> Double {
            Turbulence.fractalValue(turbulence.sum(channel: k, x: u, y: v, baseFrequencyX: p.frequencyX, baseFrequencyY: p.frequencyY,
                                                   octaves: p.octaves, fractalSum: true, tile: tile))
        }
        for y in 0..<n {
            for x in 0..<n {
                let px = (Double(x) + 0.5) / Double(scale), py = (Double(y) + 0.5) / Double(scale)
                var u = c * px - s * py, v = s * px + c * py
                // Stitched noise is seamless across one period but not periodic as a function: sample inside it.
                u = fmod(u, period)
                if u < 0 { u += period }
                v = fmod(v, period)
                if v < 0 { v += period }
                // feTurbulence's RGB channels, then saturate(0), then the linear transfer.
                let luminance = 0.213 * channel(0, u, v) + 0.715 * channel(1, u, v) + 0.072 * channel(2, u, v)
                let value = min(max(slope * luminance + intercept, 0), 1)
                out[y * n + x] = UInt8(value * 255.0 + 0.5)
            }
        }
        return out
    }

    /// The tile's edge in points, and the scale its image is made at.
    public static let pointSize = CGFloat(tokens.size)
    public static let imageScale = 2

    /// The tile as a gray image at `imageScale`, made once, on first use (about a second and a half in a
    /// debug build; touch it early off the main thread).
    public static let image: CGImage? = makeImage(grayBytes(scale: imageScale))

    static func makeImage(_ bytes: [UInt8]) -> CGImage? {
        let n = Int(Double(bytes.count).squareRoot())
        guard n * n == bytes.count, let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
        return CGImage(width: n, height: n, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: n, space: CGColorSpaceCreateDeviceGray(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue), provider: provider, decode: nil,
                       shouldInterpolate: false, intent: .defaultIntent)
    }
}
