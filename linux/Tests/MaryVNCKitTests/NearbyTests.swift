import Foundation
import Testing
@testable import MaryVNCKit

/// One vector from `Fixtures/nearby-vectors.json`, byte-identical to MaryOS's `maryvnc/tests/fixtures/nearby-vectors.json`
/// (which `maryvnc/tools/maryvnc-nearby-vectors` prints).
struct NearbyVector: Decodable {
    let description: String
    let piStatic, piPublic, macStatic, macPublic, nearbyKey, challenge, call, answer: String
    let callTag, answerTag, name: String?
    let pairable: Bool
    let port: UInt16

    enum CodingKeys: String, CodingKey {
        case description, challenge, call, answer, name, pairable, port
        case piStatic = "pi_static", piPublic = "pi_public", macStatic = "mac_static", macPublic = "mac_public"
        case nearbyKey = "nearby_key", callTag = "call_tag", answerTag = "answer_tag"
    }

    static func all() throws -> [NearbyVector] {
        struct File: Decodable {
            let label: String
            let vectors: [NearbyVector]
        }
        guard let url = Bundle.module.url(forResource: "nearby-vectors", withExtension: "json", subdirectory: "Fixtures") else {
            throw MaryVNCError("Fixtures/nearby-vectors.json is missing from the test bundle")
        }
        let file = try JSONDecoder().decode(File.self, from: Data(contentsOf: url))
        guard file.label == "MaryVNC/1 nearby" else { throw MaryVNCError("the vectors are for \(file.label)") }
        return file.vectors
    }
}

private func unhex(_ text: String) throws -> [UInt8] {
    guard let bytes = PairedPi.bytes(hex: text) else { throw MaryVNCError("not hex: \(text)") }
    return bytes
}

@Suite struct NearbyTests {
    @Test func theSharedVectorsMatchByteForByte() throws {
        let vectors = try NearbyVector.all()
        #expect(vectors.count >= 3)
        for v in vectors {
            let pi = try NoiseKeyPair(privateKey: unhex(v.piStatic)), mac = try NoiseKeyPair(privateKey: unhex(v.macStatic))
            #expect(try pi.publicKey == unhex(v.piPublic), "\(v.description)")
            #expect(try mac.publicKey == unhex(v.macPublic), "\(v.description)")
            let key = try NearbyKey(identity: mac, piPublicKey: pi.publicKey)
            #expect(try key.key == unhex(v.nearbyKey), "the Mac's side: \(v.description)")
            #expect(try Nearby.key(identity: pi, peer: mac.publicKey) == unhex(v.nearbyKey), "the Pi's side: \(v.description)")

            let call = try NearbyCall(decoding: unhex(v.call))
            #expect(try call.challenge == unhex(v.challenge))
            #expect(try call.encoded() == unhex(v.call))
            #expect(call.isTagged(for: key.key) == (v.callTag != nil), "\(v.description)")
            if let tag = v.callTag {
                #expect(try Nearby.tag(key: key.key, over: call.prefix) == unhex(tag))
            }

            let answer = try NearbyAnswer(decoding: unhex(v.answer))
            #expect(try answer.encoded() == unhex(v.answer))
            #expect(answer.challenge == call.challenge)
            #expect(answer.port == v.port)
            #expect((answer.offer != nil) == v.pairable)
            if v.pairable {
                #expect(answer.offer?.publicKey == pi.publicKey)
                #expect(answer.offer?.name == v.name)
            }
            #expect(answer.isTagged(for: key) == (v.answerTag != nil), "\(v.description)")
            if let tag = v.answerTag {
                #expect(try answer.tags == [unhex(tag)])
            }
        }
    }

    @Test func aCallHidesHowManyPisAndWhichTagIsReal() throws {
        let mac = NoiseKeyPair.generate()
        let one = try NearbyKey(identity: mac, piPublicKey: NoiseKeyPair.generate().publicKey)
        let call = try NearbyCall(keys: [one])
        #expect(call.tags.count == 4)
        #expect(call.isTagged(for: one.key))
        let five = try (0..<5).map { _ in try NearbyKey(identity: mac, piPublicKey: NoiseKeyPair.generate().publicKey) }
        let many = try NearbyCall(keys: five)
        #expect(many.tags.count == 8)
        #expect(five.allSatisfy { many.isTagged(for: $0.key) })
        #expect(try NearbyCall(keys: []).tags.isEmpty)
        #expect(throws: MaryVNCError.self) { try NearbyCall(keys: Array(repeating: one, count: 33)) }
        #expect(try NearbyCall(keys: [one]).challenge != call.challenge)
    }

    @Test func aTagIsForItsPairAndItsChallengeAlone() throws {
        let pi = NoiseKeyPair.generate(), mac = NoiseKeyPair.generate(), stranger = NoiseKeyPair.generate()
        let key = try NearbyKey(identity: mac, piPublicKey: pi.publicKey)
        let piSide = try Nearby.key(identity: pi, peer: mac.publicKey)
        var call = try NearbyCall(keys: [key])
        #expect(call.isTagged(for: piSide))
        #expect(!call.isTagged(for: try Nearby.key(identity: pi, peer: stranger.publicKey)))
        call.challenge[0] ^= 1
        #expect(!call.isTagged(for: piSide))

        var answer = NearbyAnswer(challenge: call.challenge, port: 5901, offer: nil, tags: [])
        answer.tags = [Nearby.tag(key: piSide, over: try answer.prefix())]
        #expect(answer.isTagged(for: key))
        #expect(!answer.isTagged(for: try NearbyKey(identity: stranger, piPublicKey: pi.publicKey)))
        answer.port = 5902
        #expect(!answer.isTagged(for: key))
        #expect(throws: MaryVNCError.self) { try NearbyKey(identity: mac, piPublicKey: [UInt8](repeating: 0, count: 32)) }
    }

    @Test func everyMalformedDatagramIsRefused() throws {
        let call = try NearbyCall(keys: [try NearbyKey(identity: .generate(), piPublicKey: NoiseKeyPair.generate().publicKey)])
        let bytes = try call.encoded()
        #expect(bytes.count == 22 + 4 * 8)
        #expect(try NearbyCall(decoding: bytes) == call)
        for cut in 0..<bytes.count {
            #expect(throws: MaryVNCError.self) { try NearbyCall(decoding: Array(bytes.prefix(cut))) }
        }
        #expect(throws: MaryVNCError.self) { try NearbyCall(decoding: bytes + [0]) }
        let future = bytes.enumerated().map { $0.offset == 4 ? 2 : $0.element }
        #expect(throws: MaryVNCError.self) { try NearbyCall(decoding: future) }
        let three = Array(bytes.prefix(21)) + [3] + Array(bytes[22..<46])
        #expect(throws: MaryVNCError.self) { try NearbyCall(decoding: three) }

        let answer = NearbyAnswer(challenge: call.challenge, port: 5901,
                                  offer: .init(publicKey: NoiseKeyPair.generate().publicKey, name: "maryos"), tags: [[UInt8](repeating: 0x5a, count: 8)])
        let encoded = try answer.encoded()
        #expect(encoded.count == 24 + 32 + 2 + 6 + 1 + 8)
        #expect(try NearbyAnswer(decoding: encoded) == answer)
        for cut in 0..<encoded.count {
            #expect(throws: MaryVNCError.self) { try NearbyAnswer(decoding: Array(encoded.prefix(cut))) }
        }
        #expect(throws: MaryVNCError.self) { try NearbyAnswer(decoding: encoded + [0]) }
        for (offset, value) in [(23, UInt8(3)), (58, UInt8(0x07)), (64, UInt8(33))] {   // a flag, a control character, 33 tags
            let bad = encoded.enumerated().map { $0.offset == offset ? value : $0.element }
            #expect(throws: MaryVNCError.self) { try NearbyAnswer(decoding: bad) }
        }
        #expect(throws: MaryVNCError.self) { try NearbyAnswer(decoding: Array("MVNA".utf8) + [UInt8](repeating: 0, count: 600)) }
        #expect(throws: MaryVNCError.self) {
            try NearbyAnswer(challenge: call.challenge, port: 1, offer: .init(publicKey: [1, 2], name: "x"), tags: []).encoded()
        }
        #expect(throws: MaryVNCError.self) {
            try NearbyAnswer(challenge: call.challenge, port: 1, offer: .init(publicKey: NoiseKeyPair.generate().publicKey, name: "bad\nname"), tags: []).encoded()
        }
    }
}
