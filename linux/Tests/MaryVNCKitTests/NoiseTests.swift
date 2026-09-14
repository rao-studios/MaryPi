import Foundation
import Testing
@testable import MaryVNCKit

/// One vector from cacophony, as `Fixtures/noise-vectors.json` holds it (byte-identical to MaryOS's
/// `maryvnc/tests/fixtures/noise-vectors.json`).
struct NoiseVector: Decodable, Sendable, CustomTestStringConvertible {
    struct Message: Decodable, Sendable {
        let payload: String
        let ciphertext: String
    }

    let protocolName: String
    let initPrologue: String
    let initStatic: String
    let initEphemeral: String
    let initRemoteStatic: String?
    let respPrologue: String
    let respStatic: String
    let respEphemeral: String
    let handshakeHash: String
    let messages: [Message]

    enum CodingKeys: String, CodingKey {
        case protocolName = "protocol_name", initPrologue = "init_prologue", initStatic = "init_static"
        case initEphemeral = "init_ephemeral", initRemoteStatic = "init_remote_static", respPrologue = "resp_prologue"
        case respStatic = "resp_static", respEphemeral = "resp_ephemeral", handshakeHash = "handshake_hash", messages
    }

    var testDescription: String { protocolName }

    static func load() throws -> [NoiseVector] {
        guard let url = Bundle.module.url(forResource: "noise-vectors", withExtension: "json", subdirectory: "Fixtures") else {
            throw MaryVNCError("Fixtures/noise-vectors.json is missing from the test bundle")
        }
        struct File: Decodable { let vectors: [NoiseVector] }
        return try JSONDecoder().decode(File.self, from: Data(contentsOf: url)).vectors
    }
}

func hex(_ string: String) -> [UInt8] {
    var bytes: [UInt8] = []
    var index = string.startIndex
    while index < string.endIndex, let next = string.index(index, offsetBy: 2, limitedBy: string.endIndex) {
        bytes.append(UInt8(string[index..<next], radix: 16) ?? 0)
        index = next
    }
    return bytes
}

@Suite struct NoiseVectorTests {
    static let vectors = (try? NoiseVector.load()) ?? []

    @Test func bothPatternsHaveVectors() {
        #expect(Set(Self.vectors.map(\.protocolName)) == ["Noise_XX_25519_ChaChaPoly_SHA256", "Noise_IK_25519_ChaChaPoly_SHA256"])
    }

    @Test(arguments: NoiseVectorTests.vectors)
    func handshakeAndSessionMatchTheVector(_ vector: NoiseVector) throws {
        let pattern: NoisePattern = vector.protocolName.hasPrefix("Noise_XX") ? .xx : .ik
        var initiator = try NoiseHandshake(
            pattern: pattern, initiator: true, prologue: hex(vector.initPrologue),
            staticKey: NoiseKeyPair(privateKey: hex(vector.initStatic)),
            remoteStatic: vector.initRemoteStatic.map(hex), ephemeral: NoiseKeyPair(privateKey: hex(vector.initEphemeral))
        )
        var responder = try NoiseHandshake(
            pattern: pattern, initiator: false, prologue: hex(vector.respPrologue),
            staticKey: NoiseKeyPair(privateKey: hex(vector.respStatic)), ephemeral: NoiseKeyPair(privateKey: hex(vector.respEphemeral))
        )
        let rounds = pattern.messages.count
        for (i, message) in vector.messages.prefix(rounds).enumerated() {
            if i % 2 == 0 {
                let sent = try initiator.writeMessage(payload: hex(message.payload))
                #expect(sent == hex(message.ciphertext), "message \(i)")
                #expect(try responder.readMessage(sent) == hex(message.payload))
            } else {
                let sent = try responder.writeMessage(payload: hex(message.payload))
                #expect(sent == hex(message.ciphertext), "message \(i)")
                #expect(try initiator.readMessage(sent) == hex(message.payload))
            }
        }
        #expect(initiator.isComplete && responder.isComplete)
        #expect(initiator.handshakeHash == hex(vector.handshakeHash))
        #expect(responder.handshakeHash == hex(vector.handshakeHash))
        #expect(responder.remoteStatic == (try NoiseKeyPair(privateKey: hex(vector.initStatic))).publicKey)

        var (initiatorSend, initiatorReceive) = try initiator.transport()
        var (responderSend, responderReceive) = try responder.transport()
        for (i, message) in vector.messages.enumerated().dropFirst(rounds) {
            if i % 2 == 0 {
                let sent = try initiatorSend.seal(hex(message.payload))
                #expect(sent == hex(message.ciphertext), "message \(i)")
                #expect(try responderReceive.open(sent) == hex(message.payload))
            } else {
                let sent = try responderSend.seal(hex(message.payload))
                #expect(sent == hex(message.ciphertext), "message \(i)")
                #expect(try initiatorReceive.open(sent) == hex(message.payload))
            }
        }
    }
}

@Suite struct NoiseBehaviourTests {
    /// XX, as a Mac pairs, then IK with the key it learned, as it resumes; both with MaryVNC's prologue.
    @Test func pairingThenResumingWithTheLearnedKey() throws {
        let mac = NoiseKeyPair.generate(), pi = NoiseKeyPair.generate()
        var viewer = try NoiseHandshake(pattern: .xx, initiator: true, staticKey: mac)
        var server = try NoiseHandshake(pattern: .xx, initiator: false, staticKey: pi)
        _ = try server.readMessage(viewer.writeMessage())
        _ = try viewer.readMessage(server.writeMessage())
        #expect(try server.readMessage(viewer.writeMessage(payload: Array("mac".utf8))) == Array("mac".utf8))
        #expect(viewer.remoteStatic == pi.publicKey)
        #expect(server.remoteStatic == mac.publicKey)

        var resume = try NoiseHandshake(pattern: .ik, initiator: true, staticKey: mac, remoteStatic: viewer.remoteStatic)
        var answer = try NoiseHandshake(pattern: .ik, initiator: false, staticKey: pi)
        _ = try answer.readMessage(resume.writeMessage())
        #expect(answer.remoteStatic == mac.publicKey)
        _ = try resume.readMessage(answer.writeMessage())
        var (send, _) = try resume.transport()
        var (_, receive) = try answer.transport()
        #expect(try receive.open(send.seal([1, 2, 3])) == [1, 2, 3])
    }

    /// A Mac that has the wrong key for the Pi fails at the Pi's first read, before the Pi writes anything.
    @Test func ikWithTheWrongPiKeyFailsAtTheResponder() throws {
        let mac = NoiseKeyPair.generate(), pi = NoiseKeyPair.generate(), impostor = NoiseKeyPair.generate()
        var viewer = try NoiseHandshake(pattern: .ik, initiator: true, staticKey: mac, remoteStatic: impostor.publicKey)
        var server = try NoiseHandshake(pattern: .ik, initiator: false, staticKey: pi)
        let first = try viewer.writeMessage()
        #expect(throws: MaryVNCError.self) { try server.readMessage(first) }
        #expect(!server.isMyTurn)
        #expect(throws: MaryVNCError.self) { try server.writeMessage() }
    }

    @Test func aFlippedByteFailsAndLeavesTheCounterWhereItWas() throws {
        let mac = NoiseKeyPair.generate(), pi = NoiseKeyPair.generate()
        var viewer = try NoiseHandshake(pattern: .xx, initiator: true, staticKey: mac)
        var server = try NoiseHandshake(pattern: .xx, initiator: false, staticKey: pi)
        _ = try server.readMessage(viewer.writeMessage())
        _ = try viewer.readMessage(server.writeMessage())
        _ = try server.readMessage(viewer.writeMessage())
        var (send, _) = try viewer.transport()
        var (_, receive) = try server.transport()
        let first = try send.seal(Array("first".utf8)), second = try send.seal(Array("second".utf8))
        var flipped = first
        flipped[3] ^= 0x40
        #expect(throws: MaryVNCError.self) { try receive.open(flipped) }
        #expect(throws: MaryVNCError.self) { try receive.open(second) }      // out of order: the counter did not move past it
        #expect(try receive.open(first) == Array("first".utf8))
        #expect(try receive.open(second) == Array("second".utf8))
        #expect(throws: MaryVNCError.self) { try receive.open(second) }      // a replay
    }

    @Test func theNonceIsFourZeroBytesAndALittleEndianCounter() {
        #expect(NoiseCipher.nonceBytes(0) == [UInt8](repeating: 0, count: 12))
        #expect(NoiseCipher.nonceBytes(0x0102) == [0, 0, 0, 0, 0x02, 0x01, 0, 0, 0, 0, 0, 0])
    }

    @Test func aLowOrderPointIsRefused() throws {
        let key = NoiseKeyPair.generate()
        #expect(throws: MaryVNCError.self) { try key.agree(with: [UInt8](repeating: 0, count: 32)) }
    }
}
