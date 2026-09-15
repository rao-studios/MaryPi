import Foundation
import Testing
@testable import MaryVNCKit

@Suite struct FingerprintTests {
    @Test func theFingerprintIsTheKeysSHA256InGroupsOfFour() throws {
        let zero = Fingerprint(publicKey: [UInt8](repeating: 0, count: 32))
        #expect(zero.hex == "66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925")
        #expect(zero.display == "6668 7aad f862 bd77 6c8f c18b 8e9f 8e20")
        #expect(zero.short == "6668 7aad")
        #expect(Fingerprint(hex: zero.hex.uppercased()) == zero)
        #expect(Fingerprint(hex: "66687aad") == nil)
        #expect(Fingerprint(hex: String(repeating: "g", count: 64)) == nil)
    }
}

@Suite struct IdentityTests {
    @Test func theIdentityIsMadeOnceAndKept() throws {
        let store = InMemoryIdentityStore()
        let first = try Identity.loadOrCreate(from: store)
        let second = try Identity.loadOrCreate(from: store)
        #expect(first.publicKey == second.publicKey)
        try store.delete()
        #expect(try Identity.loadOrCreate(from: store).publicKey != first.publicKey)
    }

    @Test func aFileStoreKeepsOneKeyReadableOnlyByItsOwner() throws {
        let directory = FileManager.default.temporaryDirectory.appending(path: "maryvnc-identity-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = FileIdentityStore(url: directory.appending(path: "identity.key"))
        let first = try Identity.loadOrCreate(from: store)
        #expect(try Identity.loadOrCreate(from: FileIdentityStore(url: store.url)).publicKey == first.publicKey)
        let mode = try FileManager.default.attributesOfItem(atPath: store.url.path)[.posixPermissions] as? Int
        #expect(mode == 0o600)
        try Data([1, 2, 3]).write(to: store.url)
        #expect(throws: MaryVNCError.self) { try store.load() }
    }

    @Test func theWireNameDropsControlsAndFitsSixtyFourBytes() throws {
        #expect(Identity.wireName("Rao's MacBook Pro") == "Rao's MacBook Pro")
        #expect(Identity.wireName("tab\there\u{85}") == "tabhere")
        #expect(Identity.wireName("") == "Mac")
        let long = Identity.wireName(String(repeating: "é", count: 40))   // two bytes each
        #expect(long.utf8.count == 64)
        #expect(try HandshakePayload(name: Identity.wireName(String(repeating: "🍎", count: 20))).encoded().count <= 2 + 2 + 64)
    }
}

@Suite struct PairingStoreTests {
    private func temporaryURL() -> URL {
        FileManager.default.temporaryDirectory.appending(path: "maryvnc-tests-\(UUID().uuidString)/pairs.json")
    }

    @Test func pisAreAddedFoundTouchedAndForgotten() throws {
        let url = temporaryURL()
        defer { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
        var store = try PairingStore(url: url)
        let a = NoiseKeyPair.generate().publicKey, b = NoiseKeyPair.generate().publicKey
        let t0 = Date(timeIntervalSince1970: 1_000_000)
        try store.upsert(publicKey: a, name: "maryos", now: t0)
        try store.upsert(publicKey: b, name: "kitchen", now: t0.addingTimeInterval(10))
        try store.upsert(publicKey: a, name: "maryos-pi5", now: t0.addingTimeInterval(20))
        #expect(store.pis.count == 2)
        #expect(store.find(Fingerprint(publicKey: a))?.name == "maryos-pi5")
        #expect(store.find(Fingerprint(publicKey: a))?.paired == t0)

        let both: Set = [Fingerprint(publicKey: a), Fingerprint(publicKey: b)]
        #expect(store.mostRecent(among: both)?.publicKey == b)             // paired later, neither connected
        try store.touch(Fingerprint(publicKey: a), now: t0.addingTimeInterval(30))
        #expect(store.mostRecent(among: both)?.publicKey == a)
        #expect(store.mostRecent(among: [Fingerprint(publicKey: b)])?.publicKey == b)
        #expect(store.mostRecent(among: []) == nil)

        let reread = try PairingStore(url: url)
        #expect(reread.pis == store.pis)
        let mode = try FileManager.default.attributesOfItem(atPath: url.path)[.posixPermissions] as? Int
        #expect(mode == 0o600)

        try store.forget(Fingerprint(publicKey: a))
        #expect(try PairingStore(url: url).pis.map(\.publicKey) == [b])
    }

    @Test func aLaterVersionOrABadKeyIsRefused() throws {
        let url = temporaryURL()
        defer { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(#"{"version": 2, "pis": []}"#.utf8).write(to: url)
        #expect(throws: MaryVNCError.self) { try PairingStore(url: url) }
        try Data(#"{"version": 1, "pis": [{"publicKey": "abc", "name": "x", "paired": "2026-09-14T00:00:00Z"}]}"#.utf8).write(to: url)
        #expect(throws: (any Error).self) { try PairingStore(url: url) }
    }
}

@Suite struct PairedPiAddressTests {
    @Test func theLastAddressIsKeptAndAFileWithoutOneStillReads() throws {
        let url = FileManager.default.temporaryDirectory.appending(path: "maryvnc-tests-\(UUID().uuidString)/pairs.json")
        defer { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
        let key = NoiseKeyPair.generate().publicKey
        let hex = key.map { String(format: "%02x", $0) }.joined()
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(#"{"version": 1, "pis": [{"publicKey": "\#(hex)", "name": "maryos", "paired": "2026-09-14T20:00:00Z"}]}"#.utf8).write(to: url)
        var store = try PairingStore(url: url)
        #expect(store.pis.first?.lastAddress == nil)
        try store.remember(address: "10.0.0.73", for: Fingerprint(publicKey: key))
        #expect(try PairingStore(url: url).pis.first?.lastAddress == "10.0.0.73")
        try store.touch(Fingerprint(publicKey: key), address: "fe80::8aa2:9eff:fede:9d3d%en0")
        let reread = try PairingStore(url: url)
        #expect(reread.pis.first?.lastAddress == "fe80::8aa2:9eff:fede:9d3d%en0")
        #expect(reread.pis.first?.lastConnected != nil)
    }
}
