import CoreGraphics
import Foundation
import ImageIO

/// The Pi's desktop as the viewer holds it: an sRGB bitmap (B, G, R, X in memory) that each `frame`
/// rectangle is decoded into, and a CGImage of the whole for the view. Not thread-safe: one owner applies
/// frames and makes images.
public final class Framebuffer {
    public let width: Int
    public let height: Int
    private let context: CGContext
    private let space: CGColorSpace

    public init(width: Int, height: Int) throws {
        guard (1...8192).contains(width), (1...8192).contains(height) else { throw MaryVNCError("a desktop of \(width)×\(height)") }
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4, space: space,
                                      bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { throw MaryVNCError("no bitmap for a \(width)×\(height) desktop") }
        context.interpolationQuality = .none
        context.setBlendMode(.copy)
        context.setFillColor(gray: 0, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        self.width = width
        self.height = height
        self.context = context
        self.space = space
    }

    /// Decodes one rectangle's JPEG into place.
    public func apply(_ rect: FrameRect) throws {
        let x = Int(rect.x), y = Int(rect.y), w = Int(rect.width), h = Int(rect.height)
        guard w > 0, h > 0, x + w <= width, y + h <= height else { throw MaryVNCError("a rectangle outside the desktop") }
        guard let source = CGImageSourceCreateWithData(Data(rect.jpeg) as CFData, nil),
              let decoded = CGImageSourceCreateImageAtIndex(source, 0, nil)
        else { throw MaryVNCError("a rectangle that is not a JPEG") }
        guard decoded.width == w, decoded.height == h else {
            throw MaryVNCError("a \(decoded.width)×\(decoded.height) JPEG for a \(w)×\(h) rectangle")
        }
        // TurboJPEG's JPEGs carry no profile; take them as sRGB so drawing copies instead of converting.
        let image = decoded.colorSpace?.name == CGColorSpace.sRGB ? decoded : (decoded.copy(colorSpace: space) ?? decoded)
        // Bitmap memory runs top down; drawing coordinates run bottom up.
        context.draw(image, in: CGRect(x: x, y: height - y - h, width: w, height: h))
    }

    public func makeImage() -> CGImage? {
        context.makeImage()
    }

    /// Red, green and blue at a pixel.
    public func pixel(x: Int, y: Int) -> (red: UInt8, green: UInt8, blue: UInt8)? {
        guard let data = context.data, (0..<width).contains(x), (0..<height).contains(y) else { return nil }
        let p = data.assumingMemoryBound(to: UInt8.self).advanced(by: (y * width + x) * 4)
        return (p[2], p[1], p[0])
    }
}
