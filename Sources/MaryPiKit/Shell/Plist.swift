import Foundation

public enum Plist {
    public static func decode<T: Decodable>(_ type: T.Type, from data: Data) throws -> T {
        try PropertyListDecoder().decode(type, from: data)
    }

    public static func dictionary(from data: Data) throws -> [String: Any] {
        let object = try PropertyListSerialization.propertyList(from: data, options: [], format: nil)
        guard let dictionary = object as? [String: Any] else {
            throw MaryPiError("Property list is not a dictionary")
        }
        return dictionary
    }
}
