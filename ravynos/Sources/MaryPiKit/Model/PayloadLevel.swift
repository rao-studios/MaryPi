import Foundation

/// What MaryPi can actually put on a card right now.
///
/// The level is recomputed from the ravynOS build tree on every scan and is
/// shown verbatim in the UI so nobody is surprised by what the Pi does.
public enum PayloadLevel: Int, Comparable, Sendable, Codable, CaseIterable {
    /// Raspberry Pi firmware + UEFI only. No ravynOS code on the card.
    case bootstrapOnly = 0
    /// The ravynOS arm64 booter and kernel are present. Serial output only.
    case kernelBringUp = 1
    /// Kernel, kernelcache with kexts, and an arm64 root filesystem.
    case fullSystem = 2

    public static func < (lhs: PayloadLevel, rhs: PayloadLevel) -> Bool {
        lhs.rawValue < rhs.rawValue
    }

    public var title: String {
        switch self {
        case .bootstrapOnly: return "Bootstrap only"
        case .kernelBringUp: return "Kernel bring-up"
        case .fullSystem: return "Full system (experimental)"
        }
    }

    public var shortName: String {
        switch self {
        case .bootstrapOnly: return "Level 0"
        case .kernelBringUp: return "Level 1"
        case .fullSystem: return "Level 2"
        }
    }
}

/// Something the payload scan noticed. Blocking findings prevent a flash.
public struct Finding: Sendable, Codable, Hashable, Identifiable {
    public enum Severity: String, Sendable, Codable, Hashable {
        case info
        case warning
        case blocking
    }

    public var id: String { "\(severity.rawValue):\(message)" }
    public let severity: Severity
    public let message: String

    public init(_ severity: Severity, _ message: String) {
        self.severity = severity
        self.message = message
    }
}

/// The result of scanning the ravynOS build tree.
public struct PayloadReport: Sendable, Codable {
    public var level: PayloadLevel
    public var firmwareVersion: String
    public var ravynosRoot: String?
    public var buildDir: String?
    public var booter: String?
    public var kernel: String?
    public var kernelArch: String?
    public var kernelcache: String?
    public var sysroot: String?
    /// Directory of arm64 kexts the booter hands to the kernel (System.kext + drivers).
    public var extensions: String?
    /// Names of the kexts in `extensions` (without .kext), for reports.
    public var extensionNames: [String]
    /// Freestanding process 1 (ravyninit) staged as /sbin/launchd while no
    /// real launchd exists for arm64.
    public var initProgram: String?
    public var findings: [Finding]

    public init(
        level: PayloadLevel,
        firmwareVersion: String,
        ravynosRoot: String? = nil,
        buildDir: String? = nil,
        booter: String? = nil,
        kernel: String? = nil,
        kernelArch: String? = nil,
        kernelcache: String? = nil,
        sysroot: String? = nil,
        extensions: String? = nil,
        extensionNames: [String] = [],
        initProgram: String? = nil,
        findings: [Finding] = []
    ) {
        self.level = level
        self.firmwareVersion = firmwareVersion
        self.ravynosRoot = ravynosRoot
        self.buildDir = buildDir
        self.booter = booter
        self.kernel = kernel
        self.kernelArch = kernelArch
        self.kernelcache = kernelcache
        self.sysroot = sysroot
        self.extensions = extensions
        self.extensionNames = extensionNames
        self.initProgram = initProgram
        self.findings = findings
    }

    /// A level-1 image can carry a minimal root (ravyninit as process 1) once
    /// the kernel can reach the card's second partition.
    public var hasMinimalRoot: Bool { hasStorageStack && initProgram != nil }

    /// True when the staged kexts include a block-storage stack, so the kernel
    /// can be told to mount a root filesystem.
    public var hasStorageStack: Bool {
        extensionNames.contains("IOStorageFamily") && extensionNames.contains { $0.hasPrefix("RavynVirtIOBlock") || $0.hasPrefix("RavynSDHCI") }
    }

    public var hasBlockingFindings: Bool {
        findings.contains { $0.severity == .blocking }
    }

    /// One paragraph that tells the truth about what the card will do.
    public var honestSummary: String {
        switch level {
        case .bootstrapOnly:
            return "Bootstrap only. This card boots the Raspberry Pi 5 into UEFI firmware (rpi5-uefi \(firmwareVersion)). ravynOS arm64 is not bootable yet, so no operating system will start. Useful to verify the board, its EEPROM and the serial console."
        case .kernelBringUp:
            if extensions != nil {
                let root: String
                if hasMinimalRoot {
                    root = "mounts the card's HFS+ partition as root and starts ravyninit, a freestanding process 1 with a small console shell (no libSystem, no launchd yet)"
                } else if hasStorageStack {
                    root = "mounts the card's HFS+ partition as root and stops when it finds no /sbin/launchd"
                } else {
                    root = "starts IOKit and stops where the drivers end"
                }
                return "Kernel bring-up with drivers. This card boots the ravynOS arm64 kernel through the ravynOS booter, which also loads \(extensionNames.count) kext(s) (\(extensionNames.joined(separator: ", "))). The kernel links them at boot, \(root). Output appears on the debug UART and on the screen."
            }
            return "Kernel bring-up. This card boots the ravynOS arm64 kernel through the ravynOS booter. Output appears on the 3-pin debug UART only. There are no kexts, no root filesystem and no userland: the kernel runs its bootstrap, starts IOKit and then panics because no platform driver matches the board."
        case .fullSystem:
            return "Full system image (experimental). This card carries the ravynOS arm64 kernelcache and an arm64 root filesystem. Expect rough edges; watch the serial console."
        }
    }

    /// Bullet points for the UI's "what the Pi will do" list.
    public var whatThePiWillDo: [String] {
        var lines = [
            "Raspberry Pi firmware reads config.txt and loads RPI_EFI.fd (UEFI)",
        ]
        switch level {
        case .bootstrapOnly:
            lines.append("UEFI finds no EFI/BOOT/BOOTAA64.EFI and stops at its setup screen or shell")
            lines.append("Nothing from ravynOS runs")
        case .kernelBringUp:
            lines.append("UEFI runs EFI/BOOT/BOOTAA64.EFI, the ravynOS booter")
            lines.append("The booter loads ravynos/kernel, builds the device tree and jumps into XNU")
            lines.append("XNU logs its bootstrap on the serial console, brings up the GIC and timer, starts IOKit")
            lines.append("IOKit finds no platform driver (no kexts yet) and the kernel panics; that is the expected end")
        case .fullSystem:
            lines.append("UEFI runs EFI/BOOT/BOOTAA64.EFI, the ravynOS booter")
            lines.append("The booter loads ravynos/kernelcache and jumps into XNU")
            lines.append("XNU mounts the HFS+ root partition and starts launchd")
        }
        return lines
    }
}
