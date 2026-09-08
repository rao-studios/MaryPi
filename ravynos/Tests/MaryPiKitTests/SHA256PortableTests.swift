import Foundation
import Testing
@testable import MaryPiKit

@Suite struct SHA256PortableTests {
    @Test func fipsVectors() {
        #expect(SHA256Portable.hex(of: Data()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        #expect(SHA256Portable.hex(of: Data("abc".utf8)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        #expect(SHA256Portable.hex(of: Data("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".utf8))
            == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
    }

    @Test func streamingMatchesOneShot() {
        let data = Data((0..<(1 << 20) + 123).map { UInt8($0 & 0xff) })
        var hasher = SHA256Portable()
        var offset = 0
        for size in [1, 63, 64, 65, 1000, 4096, 1 << 19] {
            let end = min(data.count, offset + size)
            hasher.update(data[offset..<end])
            offset = end
        }
        hasher.update(data[offset...])
        #expect(SHA256Portable.hex(hasher.finalize()) == SHA256Portable.hex(of: data))
    }

    @Test func matchesFileHash() throws {
        let url = FileManager.default.temporaryDirectory.appending(path: "marypi-sha-\(UUID().uuidString).bin")
        let data = Data("MaryPi portable hash".utf8)
        try data.write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try FileHash.sha256Hex(of: url) == SHA256Portable.hex(of: data))
    }
}
