import Foundation

/// A host directory exported to the guest over virtiofs.
public struct SharedDirectory: Sendable, Equatable, Hashable {
    public var tag: String
    public var url: URL
    public var readOnly: Bool

    public init(tag: String, url: URL, readOnly: Bool = false) {
        self.tag = tag
        self.url = url
        self.readOnly = readOnly
    }

    /// `tag=/path` or `/path` (the tag is then the last path component); a
    /// trailing `:ro` makes it read-only.
    public static func parse(_ text: String) throws -> SharedDirectory {
        var spec = text
        var readOnly = false
        if spec.hasSuffix(":ro") {
            readOnly = true
            spec = String(spec.dropLast(3))
        }
        let tag: String
        let path: String
        if let equals = spec.firstIndex(of: "="), !spec.hasPrefix("/") {
            tag = String(spec[..<equals])
            path = String(spec[spec.index(after: equals)...])
        } else {
            path = spec
            tag = URL(fileURLWithPath: spec).lastPathComponent
        }
        let url = URL(fileURLWithPath: (path as NSString).expandingTildeInPath).standardizedFileURL
        guard !tag.isEmpty, !path.isEmpty else { throw MaryOSError("share \(text) must be tag=/path or /path") }
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            throw MaryOSError("share \(text): \(url.path) is not a directory")
        }
        return SharedDirectory(tag: tag, url: url, readOnly: readOnly)
    }
}

/// How the guest boots. The image's default target is multi-user (a login
/// prompt on tty1 and hvc0); `desktop` appends `systemd.unit=graphical.target`
/// so maryos-desktop.service starts the Liquid Platinum desktop, and `dev`
/// adds `maryos.ui=dev` so the desktop runs from `out/ui` over virtiofs and
/// restarts whenever `make ui` replaces it.
public enum VMBootMode: Sendable, Equatable, Hashable {
    case console
    case desktop(dev: Bool)

    public var kernelArguments: [String] {
        switch self {
        case .console: return []
        case .desktop(let dev): return ["systemd.unit=graphical.target"] + (dev ? ["maryos.ui=dev"] : [])
        }
    }

    /// The builder's command line with this mode's arguments appended.
    public func commandLine(base: String) -> String {
        ([base] + kernelArguments).joined(separator: " ")
    }

    public var title: String {
        switch self {
        case .console: return "console"
        case .desktop(false): return "desktop"
        case .desktop(true): return "desktop (dev: out/ui over virtiofs)"
        }
    }
}

/// Everything needed to boot one MaryOS VM: kernel files, disk, resources,
/// shares and display. A pure value, so it can be printed, compared and tested.
public struct VMSpec: Sendable, Equatable {
    public static let defaultMemoryMiB = 4096
    public static let defaultShareTag = "maryos-out"
    public static let defaultDisplayWidth = 1280
    public static let defaultDisplayHeight = 800

    /// Half the host's cores, between 1 and 4.
    public static var defaultCPUs: Int {
        max(1, min(4, ProcessInfo.processInfo.activeProcessorCount / 2))
    }

    public var name: String
    public var cpus: Int
    public var memoryMiB: Int
    public var disk: URL
    public var kernel: URL
    public var initrd: URL?
    public var commandLine: String
    public var macAddress: String?
    public var sharedDirectories: [SharedDirectory]
    public var displayWidth: Int
    public var displayHeight: Int
    public var headless: Bool
    /// Informational: `commandLine` already carries the mode's arguments.
    public var bootMode: VMBootMode

    public init(name: String, cpus: Int, memoryMiB: Int, disk: URL, kernel: URL, initrd: URL?, commandLine: String,
                macAddress: String? = nil, sharedDirectories: [SharedDirectory] = [],
                displayWidth: Int = VMSpec.defaultDisplayWidth, displayHeight: Int = VMSpec.defaultDisplayHeight, headless: Bool = false,
                bootMode: VMBootMode = .console) {
        self.name = name
        self.cpus = cpus
        self.memoryMiB = memoryMiB
        self.disk = disk
        self.kernel = kernel
        self.initrd = initrd
        self.commandLine = commandLine
        self.macAddress = macAddress
        self.sharedDirectories = sharedDirectories
        self.displayWidth = displayWidth
        self.displayHeight = displayHeight
        self.headless = headless
        self.bootMode = bootMode
    }

    public var summary: String {
        var parts = ["\(cpus) CPU\(cpus == 1 ? "" : "s")", "\(memoryMiB) MiB", "disk \(disk.lastPathComponent)", "kernel \(kernel.lastPathComponent)"]
        parts.append(headless ? "headless" : "\(displayWidth)x\(displayHeight) window")
        if !sharedDirectories.isEmpty { parts.append("shares " + sharedDirectories.map(\.tag).joined(separator: ",")) }
        if bootMode != .console { parts.append("boots to the \(bootMode.title)") }
        return parts.joined(separator: ", ")
    }
}
