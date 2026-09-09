import Foundation

/// Parser for `KEY=VALUE` files such as `distro/distro.conf`, which the
/// builder also sources as sh. Supports `#` comments, blank lines, and
/// single or double quoted values.
public enum ConfParser {
    public static func parse(_ text: String) throws -> [String: String] {
        var result: [String: String] = [:]
        for (index, rawLine) in text.split(omittingEmptySubsequences: false, whereSeparator: \.isNewline).enumerated() {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            guard let equals = line.firstIndex(of: "=") else {
                throw MaryOSError("line \(index + 1) is not KEY=VALUE: \(line)")
            }
            let key = line[..<equals].trimmingCharacters(in: .whitespaces)
            var value = line[line.index(after: equals)...].trimmingCharacters(in: .whitespaces)
            guard !key.isEmpty, key.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "_" }) else {
                throw MaryOSError("line \(index + 1) has an invalid key: \(line)")
            }
            if let first = value.first, first == "\"" || first == "'" {
                let body = value.dropFirst()
                if let close = body.firstIndex(of: first) {
                    value = String(body[..<close])
                } else {
                    throw MaryOSError("line \(index + 1) has an unterminated quote: \(line)")
                }
            } else if let comment = value.firstIndex(of: "#") {
                value = value[..<comment].trimmingCharacters(in: .whitespaces)
            }
            result[key] = value
        }
        return result
    }
}

/// The MaryOS definition from `distro/distro.conf`: identity, Ubuntu base,
/// first user, labels and sizes. One value drives every use of a name.
public struct DistroConfig: Sendable, Equatable, Codable {
    public static let requiredKeys = [
        "DISTRO_NAME", "DISTRO_ID", "DISTRO_VERSION", "DISTRO_CODENAME", "DISTRO_HOME_URL",
        "BASE_SUITE", "BASE_MIRROR", "BASE_COMPONENTS", "ARCH",
        "DEFAULT_USER", "DEFAULT_PASSWORD", "HOSTNAME", "LOCALE", "TIMEZONE",
        "BOOT_LABEL", "ROOT_LABEL", "BOOT_PARTITION_MIB", "ROOT_MIN_MIB", "VM_DISK_GIB",
    ]

    public var name: String
    public var id: String
    public var version: String
    public var codename: String
    /// How the codename is written for people (`DISTRO_CODENAME_PRETTY`, default: the codename).
    public var codenamePretty: String
    public var homeURL: String
    public var baseSuite: String
    public var baseMirror: String
    public var baseComponents: String
    public var arch: String
    public var defaultUser: String
    public var defaultPassword: String
    public var hostname: String
    public var locale: String
    public var timezone: String
    public var bootLabel: String
    public var rootLabel: String
    public var bootPartitionMiB: Int
    public var rootMinMiB: Int
    public var vmDiskGiB: Int

    public init(conf: [String: String]) throws {
        func required(_ key: String) throws -> String {
            guard let value = conf[key], !value.isEmpty else { throw MaryOSError("distro.conf does not set \(key)") }
            return value
        }
        func integer(_ key: String) throws -> Int {
            guard let value = Int(try required(key)), value > 0 else { throw MaryOSError("distro.conf: \(key) is not a positive number") }
            return value
        }
        name = try required("DISTRO_NAME")
        id = try required("DISTRO_ID")
        version = try required("DISTRO_VERSION")
        codename = try required("DISTRO_CODENAME")
        codenamePretty = conf["DISTRO_CODENAME_PRETTY"].flatMap { $0.isEmpty ? nil : $0 } ?? codename
        homeURL = try required("DISTRO_HOME_URL")
        baseSuite = try required("BASE_SUITE")
        baseMirror = try required("BASE_MIRROR")
        baseComponents = try required("BASE_COMPONENTS")
        arch = try required("ARCH")
        defaultUser = try required("DEFAULT_USER")
        defaultPassword = try required("DEFAULT_PASSWORD")
        hostname = try required("HOSTNAME")
        locale = try required("LOCALE")
        timezone = try required("TIMEZONE")
        bootLabel = try required("BOOT_LABEL")
        rootLabel = try required("ROOT_LABEL")
        bootPartitionMiB = try integer("BOOT_PARTITION_MIB")
        rootMinMiB = try integer("ROOT_MIN_MIB")
        vmDiskGiB = try integer("VM_DISK_GIB")
        guard id.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "-" }), id == id.lowercased() else {
            throw MaryOSError("distro.conf: DISTRO_ID must be lowercase letters, digits and dashes (got \(id))")
        }
    }

    public static func load(from url: URL) throws -> DistroConfig {
        let text: String
        do {
            text = try String(contentsOf: url, encoding: .utf8)
        } catch {
            throw MaryOSError("cannot read \(url.path): \(error.localizedDescription)")
        }
        return try DistroConfig(conf: ConfParser.parse(text))
    }

    /// `maryos-24.04-pi5.img`
    public func imageName(for target: ImageTarget) -> String {
        "\(id)-\(version)-\(target.rawValue).img"
    }

    /// `MaryOS 0.0`
    public var prettyName: String { "\(name) \(version)" }

    /// `MaryOS 0.0 (Liquid Platinum)`
    public var fullName: String { "\(name) \(version) (\(codenamePretty))" }

    /// The kernel command line the builder records for direct boot in the VM.
    public var vmCommandLine: String {
        "console=hvc0 root=LABEL=\(rootLabel) rootfstype=ext4 rw rootwait"
    }

    /// The same, with the boot mode's arguments (`systemd.unit=graphical.target maryos.ui=dev`).
    public func vmCommandLine(mode: VMBootMode) -> String {
        mode.commandLine(base: vmCommandLine)
    }

    public var vmDiskBytes: Int64 { Int64(vmDiskGiB) << 30 }
}
