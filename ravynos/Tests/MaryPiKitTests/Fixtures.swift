import Foundation
import Testing
@testable import MaryPiKit

enum Fixtures {
    static func url(_ name: String, _ ext: String) throws -> URL {
        guard let url = Bundle.module.url(forResource: name, withExtension: ext, subdirectory: "Fixtures") else {
            throw MaryPiError("Missing fixture \(name).\(ext)")
        }
        return url
    }

    static func data(_ name: String, _ ext: String) throws -> Data {
        try Data(contentsOf: url(name, ext))
    }

    static func text(_ name: String, _ ext: String) throws -> String {
        try String(contentsOf: url(name, ext), encoding: .utf8)
    }

    /// A synthetic 64-bit Mach-O header for the given cputype/subtype.
    static func machO64(cputype: UInt32, cpusubtype: UInt32 = 0) -> Data {
        var data = Data()
        for value in [UInt32(0xFEED_FACF), cputype, cpusubtype, 2, 0, 0, 0, 0] {
            var little = value.littleEndian
            data.append(Data(bytes: &little, count: 4))
        }
        return data
    }

    static func fat(_ archs: [(UInt32, UInt32)]) -> Data {
        var data = Data()
        func appendBig(_ value: UInt32) {
            var big = value.bigEndian
            data.append(Data(bytes: &big, count: 4))
        }
        appendBig(0xCAFE_BABE)
        appendBig(UInt32(archs.count))
        for (type, subtype) in archs {
            appendBig(type)
            appendBig(subtype)
            appendBig(0)
            appendBig(0)
            appendBig(0)
        }
        return data
    }

    static let arm64: UInt32 = 0x0100_000C
    static let x86_64: UInt32 = 0x0100_0007

    static func temporaryDirectory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appending(path: "MaryPiKitTests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    static func write(_ data: Data, to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: url)
    }
}
