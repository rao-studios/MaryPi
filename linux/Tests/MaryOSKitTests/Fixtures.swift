import Foundation
import Testing
@testable import MaryOSKit

enum Fixtures {
    static func url(_ name: String, _ ext: String) throws -> URL {
        guard let url = Bundle.module.url(forResource: name, withExtension: ext, subdirectory: "Fixtures") else {
            throw MaryOSError("Missing fixture \(name).\(ext)")
        }
        return url
    }

    static func data(_ name: String, _ ext: String) throws -> Data {
        try Data(contentsOf: url(name, ext))
    }

    static func text(_ name: String, _ ext: String) throws -> String {
        try String(contentsOf: url(name, ext), encoding: .utf8)
    }
}
