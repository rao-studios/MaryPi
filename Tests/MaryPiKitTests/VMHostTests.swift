import Foundation
import Testing
@testable import MaryPiKit

@Suite struct VMHostTests {
    static func probe(os: VMHostOS = .macOS, arch: VMArch = .arm64, env: [String: String] = ["PATH": "/opt/homebrew/bin:/usr/bin"],
                      executables: Set<String> = ["/opt/homebrew/bin/qemu-system-aarch64"],
                      files: [String: Int64] = ["/opt/homebrew/Cellar/qemu/11.1.1/share/qemu/edk2-aarch64-code.fd": 64 << 20,
                                                "/opt/homebrew/Cellar/qemu/11.1.1/share/qemu/edk2-arm-vars.fd": 64 << 20],
                      realpaths: [String: String] = ["/opt/homebrew/bin/qemu-system-aarch64": "/opt/homebrew/Cellar/qemu/11.1.1/bin/qemu-system-aarch64"],
                      writable: Set<String> = [], gicv2: Bool? = nil, hv: Bool = true, backends: [String] = ["cocoa", "none"],
                      terminal: Bool = false) -> HostProbe {
        HostProbe(os: os, arch: arch, environment: env,
                  isExecutable: { executables.contains($0) },
                  fileExists: { files[$0] != nil },
                  isWritable: { writable.contains($0) },
                  fileSize: { files[$0] },
                  realpath: { realpaths[$0] ?? $0 },
                  deviceTreeHasGICv2: { gicv2 },
                  hvSupport: { hv },
                  displayBackends: { _ in backends },
                  qemuVersion: { _ in "QEMU emulator version 11.1.1" },
                  isTerminal: terminal)
    }

    @Test func homebrewLayout() async throws {
        let host = await VMHost.detect(probe: Self.probe())
        #expect(host.qemu == "/opt/homebrew/bin/qemu-system-aarch64")
        #expect(host.qemuVersion == "QEMU emulator version 11.1.1")
        #expect(host.firmware?.code == "/opt/homebrew/Cellar/qemu/11.1.1/share/qemu/edk2-aarch64-code.fd")
        #expect(host.firmware?.varsTemplate == "/opt/homebrew/Cellar/qemu/11.1.1/share/qemu/edk2-arm-vars.fd")
        #expect(host.firmware?.needsPadding == false)
        #expect(host.hvfAvailable && !host.kvmAvailable)
        #expect(host.hostGICv2 == nil)
    }

    @Test func debianLayout() async throws {
        let probe = Self.probe(os: .linux, env: ["PATH": "/usr/bin", "DISPLAY": ":0"], executables: ["/usr/bin/qemu-system-aarch64"],
                               files: ["/usr/share/AAVMF/AAVMF_CODE.fd": 64 << 20, "/usr/share/AAVMF/AAVMF_VARS.fd": 64 << 20, "/dev/kvm": 0],
                               realpaths: [:], writable: ["/dev/kvm"], gicv2: false, hv: false, backends: ["gtk", "sdl", "none"])
        let host = await VMHost.detect(probe: probe)
        #expect(host.firmware?.origin == "AAVMF")
        #expect(host.kvmAvailable && !host.hvfAvailable)
        #expect(host.hostGICv2 == false)
        #expect(host.displayBackend(for: .window).backend == .gtk)
    }

    @Test func unpaddedFirmwareNeedsPadding() async throws {
        let probe = Self.probe(os: .linux, env: ["PATH": "/usr/bin"], executables: ["/usr/bin/qemu-system-aarch64"],
                               files: ["/usr/share/qemu-efi-aarch64/QEMU_EFI.fd": 2 << 20], realpaths: [:], hv: false, backends: [])
        let host = await VMHost.detect(probe: probe)
        #expect(host.firmware?.needsPadding == true)
        #expect(host.firmware?.varsTemplate == nil)
        #expect(host.displayBackend(for: .window).mode == .none)
    }

    @Test func environmentOverrides() async throws {
        let probe = Self.probe(env: ["PATH": "", VMHost.qemuEnvironmentKey: "/x/qemu", VMHost.firmwareCodEnvKeyForTests: "/x/code.fd"],
                               executables: [], files: ["/x/code.fd": 64 << 20], realpaths: [:])
        let host = await VMHost.detect(probe: probe)
        #expect(host.qemu == "/x/qemu")
        #expect(host.firmware?.code == "/x/code.fd")
        #expect(host.firmware?.origin == VMHost.firmwareCodeEnvironmentKey)
    }

    @Test func noQEMU() async throws {
        let host = await VMHost.detect(probe: Self.probe(executables: [], files: [:], realpaths: [:]))
        #expect(host.qemu == nil && host.firmware == nil)
    }

    @Test func accelRules() async throws {
        let v2 = VMProfile(name: "v2", gicVersion: 2)
        let v3 = VMProfile(name: "v3", gicVersion: 3)
        let mac = await VMHost.detect(probe: Self.probe())
        #expect(try mac.resolveAccel(requested: nil, profile: v2) == .tcg)
        #expect(try mac.resolveAccel(requested: nil, profile: v3) == .hvf)
        #expect(throws: MaryPiError.self) { _ = try mac.resolveAccel(requested: .hvf, profile: v2) }
        #expect(try mac.resolveAccel(requested: .hvf, profile: v3) == .hvf)
        #expect(mac.cpu(for: .tcg, profile: v2) == "cortex-a76")
        #expect(mac.cpu(for: .hvf, profile: v3) == "host")

        let pi5Host = await VMHost.detect(probe: Self.probe(os: .linux, env: ["PATH": "/usr/bin"], executables: ["/usr/bin/qemu-system-aarch64"],
                                                            files: ["/dev/kvm": 0], realpaths: [:], writable: ["/dev/kvm"], gicv2: true, hv: false))
        #expect(try pi5Host.resolveAccel(requested: nil, profile: v2) == .kvm)
        #expect(try pi5Host.resolveAccel(requested: nil, profile: v3) == .kvm)

        let gicv3Host = await VMHost.detect(probe: Self.probe(os: .linux, env: ["PATH": "/usr/bin"], executables: ["/usr/bin/qemu-system-aarch64"],
                                                              files: ["/dev/kvm": 0], realpaths: [:], writable: ["/dev/kvm"], gicv2: false, hv: false))
        #expect(try gicv3Host.resolveAccel(requested: nil, profile: v2) == .tcg)
        #expect(try gicv3Host.resolveAccel(requested: nil, profile: v3) == .kvm)
    }
}

extension VMHost {
    static var firmwareCodEnvKeyForTests: String { firmwareCodeEnvironmentKey }
}
