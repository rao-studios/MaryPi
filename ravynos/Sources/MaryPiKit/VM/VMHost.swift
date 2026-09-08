import Foundation

public enum VMHostOS: String, Codable, Sendable { case macOS, linux, other }
public enum VMArch: String, Codable, Sendable { case arm64, x86_64, other }
public enum VMAccel: String, Codable, Sendable, CaseIterable { case hvf, kvm, tcg }
public enum VMDisplayMode: String, Codable, Sendable, CaseIterable { case window, serial, none }
public enum VMDisplayBackend: String, Codable, Sendable { case cocoa, gtk, sdl, none }

/// An EDK2 AArch64 firmware pair. pflash images must be exactly 64 MiB;
/// distros that ship the raw 2 MiB `QEMU_EFI.fd` need padding into the state dir.
public struct VMFirmware: Codable, Sendable, Equatable {
    public static let pflashSize: Int64 = 64 << 20

    public let code: String
    public let varsTemplate: String?
    public let needsPadding: Bool
    public let origin: String

    public init(code: String, varsTemplate: String?, needsPadding: Bool, origin: String) {
        self.code = code
        self.varsTemplate = varsTemplate
        self.needsPadding = needsPadding
        self.origin = origin
    }
}

/// Everything `VMHost.detect` touches, so tests can describe fake hosts.
public struct HostProbe: Sendable {
    public var os: VMHostOS
    public var arch: VMArch
    public var environment: [String: String]
    public var isExecutable: @Sendable (String) -> Bool
    public var fileExists: @Sendable (String) -> Bool
    public var isWritable: @Sendable (String) -> Bool
    public var fileSize: @Sendable (String) -> Int64?
    public var realpath: @Sendable (String) -> String
    /// Whether the host device tree advertises a GICv2 (nil when unknown, e.g. ACPI hosts or macOS).
    public var deviceTreeHasGICv2: @Sendable () -> Bool?
    public var hvSupport: @Sendable () -> Bool
    /// Output of `qemu -display help`, one backend per element.
    public var displayBackends: @Sendable (String) async -> [String]
    public var qemuVersion: @Sendable (String) async -> String?
    public var isTerminal: Bool

    public init(os: VMHostOS, arch: VMArch, environment: [String: String],
                isExecutable: @escaping @Sendable (String) -> Bool,
                fileExists: @escaping @Sendable (String) -> Bool,
                isWritable: @escaping @Sendable (String) -> Bool,
                fileSize: @escaping @Sendable (String) -> Int64?,
                realpath: @escaping @Sendable (String) -> String,
                deviceTreeHasGICv2: @escaping @Sendable () -> Bool?,
                hvSupport: @escaping @Sendable () -> Bool,
                displayBackends: @escaping @Sendable (String) async -> [String],
                qemuVersion: @escaping @Sendable (String) async -> String?,
                isTerminal: Bool) {
        self.os = os
        self.arch = arch
        self.environment = environment
        self.isExecutable = isExecutable
        self.fileExists = fileExists
        self.isWritable = isWritable
        self.fileSize = fileSize
        self.realpath = realpath
        self.deviceTreeHasGICv2 = deviceTreeHasGICv2
        self.hvSupport = hvSupport
        self.displayBackends = displayBackends
        self.qemuVersion = qemuVersion
        self.isTerminal = isTerminal
    }

    public static var currentOS: VMHostOS {
        #if os(macOS)
        return .macOS
        #elseif os(Linux)
        return .linux
        #else
        return .other
        #endif
    }

    public static var currentArch: VMArch {
        #if arch(arm64)
        return .arm64
        #elseif arch(x86_64)
        return .x86_64
        #else
        return .other
        #endif
    }

    /// The real machine.
    public static func live(runner: CommandRunner = CommandRunner()) -> HostProbe {
        HostProbe(
            os: currentOS,
            arch: currentArch,
            environment: ProcessInfo.processInfo.environment,
            isExecutable: { FileManager.default.isExecutableFile(atPath: $0) },
            fileExists: { FileManager.default.fileExists(atPath: $0) },
            isWritable: { FileManager.default.isWritableFile(atPath: $0) },
            fileSize: { (try? FileManager.default.attributesOfItem(atPath: $0)[.size] as? NSNumber)?.int64Value },
            realpath: { URL(fileURLWithPath: $0).resolvingSymlinksInPath().path },
            deviceTreeHasGICv2: {
                let fm = FileManager.default
                let base = "/sys/firmware/devicetree/base"
                guard fm.fileExists(atPath: base), let enumerator = fm.enumerator(atPath: base) else { return nil }
                for case let path as String in enumerator where path.hasSuffix("compatible") {
                    if let data = fm.contents(atPath: base + "/" + path) {
                        let text = String(decoding: data, as: UTF8.self)
                        if text.contains("arm,gic-400") || text.contains("arm,cortex-a15-gic") || text.contains("arm,gic-v2") {
                            return true
                        }
                    }
                }
                return false
            },
            hvSupport: {
                #if os(macOS)
                var value: Int32 = 0
                var size = MemoryLayout<Int32>.size
                return sysctlbyname("kern.hv_support", &value, &size, nil, 0) == 0 && value == 1
                #else
                return false
                #endif
            },
            displayBackends: { qemu in
                guard let result = try? await runner.run(qemu, ["-display", "help"]) else { return [] }
                return result.stdoutText.split(whereSeparator: \.isNewline)
                    .map { $0.trimmingCharacters(in: .whitespaces) }
                    .filter { !$0.isEmpty && !$0.lowercased().hasPrefix("available") }
            },
            qemuVersion: { qemu in
                guard let result = try? await runner.run(qemu, ["--version"]), result.succeeded else { return nil }
                return result.stdoutText.split(whereSeparator: \.isNewline).first.map(String.init)
            },
            isTerminal: isatty(0) != 0
        )
    }
}

/// What this host can do: QEMU, firmware, accelerators, displays.
public struct VMHost: Sendable, Equatable {
    public static let qemuEnvironmentKey = "MARYPI_VM_QEMU"
    public static let firmwareCodeEnvironmentKey = "MARYPI_VM_EFI_CODE"
    public static let firmwareVarsEnvironmentKey = "MARYPI_VM_EFI_VARS"
    public static let qemuSearchPath = ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"]
    public static let qemuBinaryName = "qemu-system-aarch64"

    public let os: VMHostOS
    public let arch: VMArch
    public let qemu: String?
    public let qemuVersion: String?
    public let firmware: VMFirmware?
    public let hvfAvailable: Bool
    public let kvmAvailable: Bool
    public let hostGICv2: Bool?
    public let displayBackends: [String]
    public let hasGUISession: Bool
    public let isTerminal: Bool

    public init(os: VMHostOS, arch: VMArch, qemu: String?, qemuVersion: String?, firmware: VMFirmware?, hvfAvailable: Bool,
                kvmAvailable: Bool, hostGICv2: Bool?, displayBackends: [String], hasGUISession: Bool, isTerminal: Bool) {
        self.os = os
        self.arch = arch
        self.qemu = qemu
        self.qemuVersion = qemuVersion
        self.firmware = firmware
        self.hvfAvailable = hvfAvailable
        self.kvmAvailable = kvmAvailable
        self.hostGICv2 = hostGICv2
        self.displayBackends = displayBackends
        self.hasGUISession = hasGUISession
        self.isTerminal = isTerminal
    }

    /// `$MARYPI_VM_QEMU`, then `$PATH`, then the well-known directories.
    public static func findQEMU(probe: HostProbe) -> String? {
        if let override = probe.environment[qemuEnvironmentKey], !override.isEmpty {
            return override
        }
        let pathDirs = (probe.environment["PATH"] ?? "").split(separator: ":").map(String.init)
        for dir in pathDirs + qemuSearchPath where !dir.isEmpty {
            let candidate = "\(dir)/\(qemuBinaryName)"
            if probe.isExecutable(candidate) { return candidate }
        }
        return nil
    }

    /// The same search order as `vm/run.sh find_firmware`.
    public static func findFirmware(qemu: String?, probe: HostProbe) -> VMFirmware? {
        func make(_ code: String, _ vars: String?, origin: String) -> VMFirmware {
            let size = probe.fileSize(code)
            let varsPath = vars.flatMap { probe.fileExists($0) ? $0 : nil }
            return VMFirmware(code: code, varsTemplate: varsPath, needsPadding: size != VMFirmware.pflashSize, origin: origin)
        }
        if let code = probe.environment[firmwareCodeEnvironmentKey], !code.isEmpty {
            let vars = probe.environment[firmwareVarsEnvironmentKey].flatMap { $0.isEmpty ? nil : $0 }
            return make(code, vars, origin: firmwareCodeEnvironmentKey)
        }
        var shareDirs: [String] = []
        if let qemu {
            let real = probe.realpath(qemu)
            shareDirs.append(URL(fileURLWithPath: real).deletingLastPathComponent().deletingLastPathComponent().appending(path: "share/qemu").path)
        }
        shareDirs += ["/opt/homebrew/share/qemu", "/usr/local/share/qemu", "/usr/share/qemu"]
        for dir in shareDirs {
            let code = "\(dir)/edk2-aarch64-code.fd"
            if probe.fileExists(code) {
                return make(code, "\(dir)/edk2-arm-vars.fd", origin: "QEMU share directory")
            }
        }
        if probe.fileExists("/usr/share/AAVMF/AAVMF_CODE.fd") {
            return make("/usr/share/AAVMF/AAVMF_CODE.fd", "/usr/share/AAVMF/AAVMF_VARS.fd", origin: "AAVMF")
        }
        if probe.fileExists("/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw") {
            return make("/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw", "/usr/share/edk2/aarch64/vars-template-pflash.raw", origin: "edk2-aarch64")
        }
        for code in ["/usr/share/qemu-efi-aarch64/QEMU_EFI.fd", "/usr/share/edk2/aarch64/QEMU_EFI.fd"] where probe.fileExists(code) {
            return make(code, nil, origin: "QEMU_EFI.fd (padded)")
        }
        return nil
    }

    public static func detect(probe: HostProbe) async -> VMHost {
        let qemu = findQEMU(probe: probe)
        let version = qemu == nil ? nil : await probe.qemuVersion(qemu!)
        let backends = qemu == nil ? [] : await probe.displayBackends(qemu!)
        let hasGUI: Bool
        switch probe.os {
        case .macOS: hasGUI = true
        default: hasGUI = !((probe.environment["DISPLAY"] ?? "") + (probe.environment["WAYLAND_DISPLAY"] ?? "")).isEmpty
        }
        return VMHost(
            os: probe.os,
            arch: probe.arch,
            qemu: qemu,
            qemuVersion: version,
            firmware: findFirmware(qemu: qemu, probe: probe),
            hvfAvailable: probe.os == .macOS && probe.arch == .arm64 && probe.hvSupport(),
            kvmAvailable: probe.os == .linux && probe.arch == .arm64 && probe.isWritable("/dev/kvm"),
            hostGICv2: probe.os == .linux ? probe.deviceTreeHasGICv2() : nil,
            displayBackends: backends,
            hasGUISession: hasGUI,
            isTerminal: probe.isTerminal
        )
    }

    /// Same table as `vm/run.sh choose_accel`.
    public func resolveAccel(requested: VMAccel?, profile: VMProfile) throws -> VMAccel {
        switch requested {
        case .hvf:
            guard profile.gicVersion == 3 else {
                throw MaryPiError("QEMU cannot use hvf with a GICv2 guest (HVF does not support GICv2 emulation). Use --accel tcg or --profile qemu-virt-gicv3.")
            }
            return .hvf
        case .kvm:
            return .kvm
        case .tcg:
            return .tcg
        case nil:
            if profile.gicVersion == 3 {
                if hvfAvailable { return .hvf }
                if kvmAvailable { return .kvm }
                return .tcg
            }
            if kvmAvailable && hostGICv2 == true { return .kvm }
            return .tcg
        }
    }

    public func cpu(for accel: VMAccel, profile: VMProfile) -> String {
        accel == .tcg ? profile.cpuEmulated : profile.cpuAccelerated
    }

    /// Same table as `vm/run.sh choose_display`; window may degrade to none.
    public func displayBackend(for mode: VMDisplayMode) -> (mode: VMDisplayMode, backend: VMDisplayBackend) {
        switch mode {
        case .window:
            if os == .macOS { return (.window, .cocoa) }
            guard hasGUISession else { return (.none, .none) }
            if displayBackends.contains("gtk") { return (.window, .gtk) }
            if displayBackends.contains("sdl") { return (.window, .sdl) }
            return (.none, .none)
        case .serial, .none:
            return (mode, .none)
        }
    }
}
