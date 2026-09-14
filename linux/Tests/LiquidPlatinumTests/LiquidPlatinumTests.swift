import CryptoKit
import Foundation
import Testing
@testable import LiquidPlatinum

@Suite struct TokenTests {
    /// The submodule's header, which the generator reads.
    static let header = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        .appending(path: "../../maryos/maryui/include/maryui/lp_tokens.h").standardized

    @Test func everyTokenInTheHeaderIsGenerated() throws {
        let text = try String(contentsOf: Self.header, encoding: .utf8)
        let rows = text.split(separator: "\n").filter { $0.hasPrefix("    { \"") }.count
        #expect(rows > 300)
        #expect(LP.count == rows && LP.all.count == rows)
        #expect(text.contains("#define LP_TOKENS_SOURCE_SHA \"\(LP.sourceSHA)\""))
    }

    @Test func tokensKeepTheirValuesAndTypes() {
        #expect(LP.Size.titlebarHeight == 42)
        #expect(LP.Radius.window == 12)
        #expect(LP.Brush.freqX == 0.02)
        #expect(LP.Traffic.Close.base == LPColor(red: 0.9333, green: 0.4196, blue: 0.3725, alpha: 1))
        #expect(LP.Shadow.window.count == 2 && LP.Shadow.window[1].spread == 1)
        #expect(LP.all.first == LPTokenInfo("platinum.0", "color", "#f7f7f9"))
    }
}

@Suite struct BrushTileTests {
    /// `lp_brush_tile_render` from maryos/maryui (lp_texture.c and lp_noise.c against cairo, -std=c11), run on
    /// this Mac on 2026-09-14: the gray channel of the 512 px tile, rows top down.
    static let cTileSHA256 = "722fa7e9b7b3b65aabfa55e9c23164b3c50e67e40821b520e39a207179c259fb"

    @Test func theTileIsTheCDesktopsByteForByte() throws {
        let bytes = BrushTile.grayBytes()
        let n = BrushTile.tokens.size
        #expect(n == 512 && bytes.count == n * n)
        #expect(bytes[0] == 103 && bytes[1] == 111 && bytes[n] == 121)
        #expect(bytes[200 * n + 100] == 130 && bytes[511 * n + 511] == 149)
        #expect(SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined() == Self.cTileSHA256)
        let image = try #require(BrushTile.makeImage(bytes))
        #expect(image.width == 512 && image.height == 512)
    }

    @Test func theTokensAreWidenedFromFloatAsTheCDesktopReadsThem() {
        #expect(BrushTile.tokens.frequencyX == Double(Float(0.02)))
        #expect(BrushTile.tokens.frequencyX != 0.02)
        #expect(BrushTile.tokens.rise == 1 && BrushTile.tokens.run == 2 && BrushTile.tokens.octaves == 2 && BrushTile.tokens.seed == 7)
    }
}
