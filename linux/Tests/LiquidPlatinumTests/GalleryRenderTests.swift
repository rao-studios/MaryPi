import CoreGraphics
import Foundation
import ImageIO
import SwiftUI
import Testing
@testable import LiquidPlatinum

/// Renders the gallery to PNGs for a person to look at: `LP_GALLERY_OUT=/some/dir swift test --filter
/// GalleryRenderTests`. Skipped without it.
@Suite(.enabled(if: ProcessInfo.processInfo.environment["LP_GALLERY_OUT"] != nil))
struct GalleryRenderTests {
    @MainActor
    @Test func renderTheGallery() throws {
        let directory = URL(fileURLWithPath: ProcessInfo.processInfo.environment["LP_GALLERY_OUT"]!)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        _ = BrushTile.image
        for (name, view) in [("gallery", LiquidPlatinumGallery()), ("gallery-sheet", LiquidPlatinumGallery(showSheet: true)),
                             ("gallery-inactive", LiquidPlatinumGallery(active: false))] {
            let renderer = ImageRenderer(content: view.environment(\.lpGrainOrigin, CGPoint(x: 140, y: 90)).padding(20).background(Color(white: 0.55)))
            renderer.scale = 2
            let image = try #require(renderer.cgImage)
            let url = directory.appending(path: "\(name).png")
            let destination = try #require(CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil))
            CGImageDestinationAddImage(destination, image, nil)
            #expect(CGImageDestinationFinalize(destination))
        }
    }
}
