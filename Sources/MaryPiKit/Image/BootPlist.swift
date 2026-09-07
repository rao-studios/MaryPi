import Foundation

/// `ravynos/com.ravynos.boot.plist` on the FAT partition. The schema mirrors
/// Apple's com.apple.Boot.plist and is read by the ravynOS AArch64 booter.
public struct BootPlist: Sendable, Equatable, Hashable {
    public static let fileName = "com.ravynos.boot.plist"
    public static let kernelKey = "Kernel"
    public static let kernelFlagsKey = "Kernel Flags"
    public static let defaultKernelPath = "\\ravynos\\kernel"
    public static let defaultKernelcachePath = "\\ravynos\\kernelcache"

    /// Backslash-separated path on the boot volume, e.g. `\ravynos\kernel`.
    public var kernel: String
    public var kernelFlags: String

    public init(kernel: String, kernelFlags: String) {
        self.kernel = kernel
        self.kernelFlags = kernelFlags
    }

    public init(plistData: Data) throws {
        let dictionary = try Plist.dictionary(from: plistData)
        guard let kernel = dictionary[Self.kernelKey] as? String else {
            throw MaryPiError("\(Self.fileName) has no \(Self.kernelKey) entry")
        }
        self.kernel = kernel
        self.kernelFlags = dictionary[Self.kernelFlagsKey] as? String ?? ""
    }

    public func plistData() throws -> Data {
        let dictionary: [String: Any] = [Self.kernelKey: kernel, Self.kernelFlagsKey: kernelFlags]
        return try PropertyListSerialization.data(fromPropertyList: dictionary, format: .xml, options: 0)
    }
}
