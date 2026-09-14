import Foundation
import Testing
@testable import MaryVNCKit

@Suite struct WireMessageTests {
    static let samples: [WireMessage] = [
        .hello(version: 1, name: "maryos", width: 1280, height: 800, fingerprint: Array(0..<32)),
        .hello(version: 1, name: "", width: 0, height: 0, fingerprint: [UInt8](repeating: 0xff, count: 32)),
        .frame(seq: 7, rects: [FrameRect(x: 16, y: 32, width: 64, height: 48, jpeg: [0xff, 0xd8, 0xff, 0xd9]),
                               FrameRect(x: 0, y: 0, width: 1, height: 1, jpeg: [])]),
        .frame(seq: 0, rects: []),
        .cursor(x: -3, y: 700, shown: true),
        .serverBye(reason: "replaced"),
        .pointer(x: 640, y: 12, buttons: [.left, .middle], scrollX: -120, scrollY: 15),
        .key(evdev: 125, pressed: true),
        .ack(seq: 0xdeadbeef),
        .quality(.fast),
        .refresh,
        .viewerBye,
    ]

    @Test(arguments: WireMessageTests.samples)
    func roundTrips(_ message: WireMessage) throws {
        #expect(try WireMessage.decode(message.encoded()) == message)
    }

    @Test(arguments: WireMessageTests.samples)
    func everyTruncationAndATrailingByteAreRefused(_ message: WireMessage) throws {
        let bytes = try message.encoded()
        for length in 0..<bytes.count {
            #expect(throws: MaryVNCError.self) { try WireMessage.decode(Array(bytes.prefix(length))) }
        }
        #expect(throws: MaryVNCError.self) { try WireMessage.decode(bytes + [0]) }
    }

    /// The layouts, byte for byte, as maryvnc/src/wire.c writes them.
    @Test func bytesMatchTheCLayouts() throws {
        #expect(try WireMessage.pointer(x: 0x0102, y: 3, buttons: [.left, .middle], scrollX: -1, scrollY: 2).encoded()
                == [0x10, 0x02, 0x01, 0x03, 0x00, 0x05, 0xff, 0xff, 0x02, 0x00])
        #expect(try WireMessage.key(evdev: 0x7d, pressed: true).encoded() == [0x11, 0x7d, 0x00, 0x01])
        #expect(try WireMessage.ack(seq: 0x01020304).encoded() == [0x12, 0x04, 0x03, 0x02, 0x01])
        #expect(try WireMessage.quality(.best).encoded() == [0x13, 0x00])
        #expect(try WireMessage.cursor(x: -2, y: 1, shown: false).encoded() == [0x03, 0xfe, 0xff, 0x01, 0x00, 0x00])
        let hello = try WireMessage.hello(version: 1, name: "pi", width: 1280, height: 800, fingerprint: [UInt8](repeating: 9, count: 32)).encoded()
        #expect(Array(hello.prefix(9)) == [0x01, 0x01, 0x00, 0x02, 0x00, 0x70, 0x69, 0x00, 0x05])
        #expect(hello.count == 1 + 2 + 2 + 2 + 2 + 2 + 32)
        let frame = try WireMessage.frame(seq: 2, rects: [FrameRect(x: 1, y: 2, width: 3, height: 4, jpeg: [0xaa])]).encoded()
        #expect(frame == [0x02, 0x02, 0, 0, 0, 0x01, 0x00, 0x01, 0, 0x02, 0, 0x03, 0, 0x04, 0, 0x01, 0x01, 0, 0, 0, 0xaa])
    }

    @Test func fieldsOutOfRangeAreRefused() throws {
        #expect(throws: MaryVNCError.self) { try WireMessage.serverBye(reason: String(repeating: "x", count: 33)).encoded() }
        #expect(throws: MaryVNCError.self) {
            try WireMessage.hello(version: 1, name: String(repeating: "é", count: 33), width: 1, height: 1, fingerprint: Array(0..<32)).encoded()
        }
        #expect(throws: MaryVNCError.self) { try WireMessage.serverBye(reason: "tab\there").encoded() }
        #expect(throws: MaryVNCError.self) { try WireMessage.serverBye(reason: "c1\u{85}").encoded() }
        #expect(throws: MaryVNCError.self) { try WireMessage.pointer(x: 0, y: 0, buttons: PointerButtons(rawValue: 8), scrollX: 0, scrollY: 0).encoded() }
        let nine = [FrameRect](repeating: FrameRect(x: 0, y: 0, width: 1, height: 1, jpeg: [1]), count: 9)
        #expect(throws: MaryVNCError.self) { try WireMessage.frame(seq: 1, rects: nine).encoded() }

        #expect(throws: MaryVNCError.self) { try WireMessage.decode([0x05]) }                       // an unknown type
        #expect(throws: MaryVNCError.self) { try WireMessage.decode([0x11, 0x01, 0x00, 0x02]) }     // pressed = 2
        #expect(throws: MaryVNCError.self) { try WireMessage.decode([0x13, 0x02]) }                 // quality 2
        #expect(throws: MaryVNCError.self) { try WireMessage.decode([0x03, 0, 0, 0, 0, 0x02]) }     // shown = 2
        #expect(throws: MaryVNCError.self) { try WireMessage.decode([0x04, 0x02, 0x00, 0xc0, 0xaf]) } // an overlong '/'
        #expect(throws: MaryVNCError.self) { try WireMessage.decode([0x02, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0x02, 0, 0, 0, 0]) } // codec 2
        #expect(try WireMessage.decode([0x04, 0x03, 0x00, 0x62, 0x79, 0x65]) == .serverBye(reason: "bye"))
    }

    @Test func handshakePayloadsCarryAVersionAndAName() throws {
        #expect(try HandshakePayload().encoded() == [0x01, 0x00, 0x00, 0x00])
        let named = HandshakePayload(name: "Rao's MacBook Pro")
        #expect(try HandshakePayload.decode(named.encoded()) == named)
        #expect(throws: MaryVNCError.self) { try HandshakePayload.decode([0x01, 0x00, 0x00, 0x00, 0x00]) }
        #expect(throws: MaryVNCError.self) { try HandshakePayload(name: String(repeating: "m", count: 65)).encoded() }
    }
}

@Suite struct RecordTests {
    @Test func recordsReassembleAcrossSplits() throws {
        let stream = try Record.framed([1, 2, 3]) + Record.framed([4]) + Record.framed(Array(repeating: 9, count: 300))
        for cut in [1, 3, 4, 5, 7, 8, 200] {
            var reader = RecordReader()
            var bodies: [[UInt8]] = []
            reader.append(stream.prefix(cut))
            while let body = try reader.next() { bodies.append(body) }
            reader.append(stream.dropFirst(cut))
            while let body = try reader.next() { bodies.append(body) }
            #expect(bodies == [[1, 2, 3], [4], Array(repeating: 9, count: 300)], "cut at \(cut)")
            #expect(reader.bufferedCount == 0)
        }
    }

    @Test func anEmptyOrOversizedRecordIsRefused() throws {
        var empty = RecordReader()
        empty.append([0, 0, 0, 0])
        #expect(throws: MaryVNCError.self) { try empty.next() }
        var small = RecordReader(maximum: 8)
        small.append([9, 0, 0, 0])
        #expect(throws: MaryVNCError.self) { try small.next() }
        #expect(throws: MaryVNCError.self) { try Record.framed([]) }
        #expect(throws: MaryVNCError.self) { try Record.framed([1, 2, 3], maximum: 2) }
    }

    @Test func theFirstRecordNamesItsPattern() throws {
        let noise: [UInt8] = Array(repeating: 7, count: 32)
        let record = try Record.first(pattern: .ik, noise: noise)
        #expect(Array(record.prefix(9)) == [37, 0, 0, 0, 0x4d, 0x56, 0x4e, 0x43, 0x02])
        var reader = RecordReader()
        reader.append(record)
        let parsed = try Record.parseFirst(try #require(try reader.next()))
        #expect(parsed.pattern == .ik && parsed.noise == noise)
        #expect(throws: MaryVNCError.self) { try Record.parseFirst(Array("MVNX".utf8) + [1]) }
        #expect(throws: MaryVNCError.self) { try Record.parseFirst(MaryVNC.magic + [3]) }
        #expect(throws: MaryVNCError.self) { try Record.first(pattern: .xx, noise: Array(repeating: 0, count: 1025)) }
    }
}
