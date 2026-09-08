import Foundation
import Testing
@testable import MaryPiKit

@Suite struct PayloadResolverTests {
    private func tree(in dir: URL) -> BuildTree {
        let root = dir.appending(path: "ravynos")
        try? FileManager.default.createDirectory(at: root.appending(path: "Kernel/xnu"), withIntermediateDirectories: true)
        return BuildTree(source: .flag, ravynosRoot: root, buildDir: dir.appending(path: "build"))
    }

    @Test func noCheckoutIsLevel0() {
        let report = PayloadResolver().evaluate(BuildTree(source: .none, ravynosRoot: nil, buildDir: nil), firmwareVersion: "v0.3")
        #expect(report.level == .bootstrapOnly)
        #expect(report.findings.contains { $0.message.contains("No ravynOS checkout") })
    }

    @Test func noBuildDirIsLevel0() throws {
        let dir = try Fixtures.temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let report = PayloadResolver().evaluate(tree(in: dir), firmwareVersion: "v0.3")
        #expect(report.level == .bootstrapOnly)
        #expect(report.findings.contains { $0.message.contains("No build directory") })
    }

    @Test func x86KernelStaysLevel0() throws {
        let dir = try Fixtures.temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let tree = tree(in: dir)
        try Fixtures.write(Fixtures.machO64(cputype: Fixtures.x86_64), to: dir.appending(path: "build/sysroot/System/Library/Kernels/kernel"))
        try Fixtures.write(Data("efi".utf8), to: dir.appending(path: "build/booter/bootaa64.efi"))
        let report = PayloadResolver().evaluate(tree, firmwareVersion: "v0.3")
        #expect(report.level == .bootstrapOnly)
        #expect(report.findings.contains { $0.message.contains("x86_64") })
        #expect(report.booter != nil)
    }

    @Test func arm64KernelWithoutBooterStaysLevel0() throws {
        let dir = try Fixtures.temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let tree = tree(in: dir)
        try Fixtures.write(Fixtures.machO64(cputype: Fixtures.arm64), to: dir.appending(path: "build/sysroot-arm64/System/Library/Kernels/kernel.development"))
        let report = PayloadResolver().evaluate(tree, firmwareVersion: "v0.3")
        #expect(report.level == .bootstrapOnly)
        #expect(report.kernel?.hasSuffix("kernel.development") == true)
        #expect(report.findings.contains { $0.message.contains("no booter") })
    }

    @Test func kernelAndBooterIsLevel1() throws {
        let dir = try Fixtures.temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let tree = tree(in: dir)
        try Fixtures.write(Fixtures.machO64(cputype: Fixtures.arm64), to: dir.appending(path: "build/sysroot-arm64/System/Library/Kernels/kernel"))
        try Fixtures.write(Data("efi".utf8), to: dir.appending(path: "build/booter/bootaa64.efi"))
        let report = PayloadResolver().evaluate(tree, firmwareVersion: "v0.3")
        #expect(report.level == .kernelBringUp)
        #expect(report.findings.contains { $0.message.contains("Not a full system yet") })
    }

    @Test func fullSystemIsLevel2() throws {
        let dir = try Fixtures.temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let tree = tree(in: dir)
        let arm64 = Fixtures.machO64(cputype: Fixtures.arm64)
        try Fixtures.write(arm64, to: dir.appending(path: "build/sysroot-arm64/System/Library/Kernels/kernel"))
        try Fixtures.write(arm64, to: dir.appending(path: "build/kernelcache-arm64"))
        try Fixtures.write(arm64, to: dir.appending(path: "build/sysroot-arm64/sbin/launchd"))
        try Fixtures.write(arm64, to: dir.appending(path: "build/sysroot-arm64/usr/lib/libSystem.B.dylib"))
        try Fixtures.write(Data(), to: dir.appending(path: "build/sysroot-arm64/System/Library/Extensions/IOStorageFamily.kext/Info.plist"))
        try Fixtures.write(Data("efi".utf8), to: dir.appending(path: "build/sysroot-arm64/System/Library/CoreServices/bootaa64.efi"))
        let report = PayloadResolver().evaluate(tree, firmwareVersion: "v0.3")
        #expect(report.level == .fullSystem)
        #expect(report.kernelcache?.hasSuffix("kernelcache-arm64") == true)
        #expect(report.sysroot?.hasSuffix("sysroot-arm64") == true)
    }

    @Test func buildTreeLocatePrecedence() throws {
        let dir = try Fixtures.temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let home = dir.appending(path: "home")
        let flagged = BuildTree.locate(explicitRoot: "/tmp/a", environment: [BuildTree.environmentRootKey: "/tmp/b"], settings: Settings(ravynosRoot: "/tmp/c"), home: home)
        #expect(flagged.source == .flag)
        #expect(flagged.ravynosRoot?.path == "/tmp/a")
        #expect(flagged.buildDir?.path == "/tmp/build")
        let fromEnv = BuildTree.locate(explicitRoot: nil, environment: [BuildTree.environmentRootKey: "/tmp/b", BuildTree.environmentBuildKey: "/tmp/out"], settings: Settings(ravynosRoot: "/tmp/c"), home: home)
        #expect(fromEnv.source == .environment)
        #expect(fromEnv.buildDir?.path == "/tmp/out")
        let fromSettings = BuildTree.locate(explicitRoot: nil, environment: [:], settings: Settings(ravynosRoot: "/tmp/c"), home: home)
        #expect(fromSettings.source == .settings)
        let none = BuildTree.locate(explicitRoot: nil, environment: [:], settings: Settings(), home: home)
        #expect(none.source == .none)
        #expect(none.ravynosRoot == nil)
        try FileManager.default.createDirectory(at: home.appending(path: BuildTree.defaultRelativeRoot + "/Kernel/xnu"), withIntermediateDirectories: true)
        let byDefault = BuildTree.locate(explicitRoot: nil, environment: [:], settings: Settings(), home: home)
        #expect(byDefault.source == .defaultLocation)
    }

    @Test func settingsRoundTrip() throws {
        let dir = try Fixtures.temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        let url = dir.appending(path: "config.json")
        try Settings(ravynosRoot: "/x", buildDir: "/y").save(to: url)
        #expect(Settings.load(from: url) == Settings(ravynosRoot: "/x", buildDir: "/y"))
        #expect(Settings.load(from: dir.appending(path: "missing.json")) == Settings())
    }

    @Test func imageSizeRules() throws {
        let dir = try Fixtures.temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: dir) }
        for name in [FirmwareBundle.efiFileName, FirmwareBundle.configFileName, FirmwareBundle.dtbFileName] {
            try Fixtures.write(Data("x".utf8), to: dir.appending(path: name))
        }
        let source = FirmwareSource(id: "rpi5-uefi", version: "v0.3", url: "https://example.com/a.zip", sha256: String(repeating: "a", count: 64), sizeBytes: 1, files: [FirmwareBundle.efiFileName])
        let bundle = try FirmwareBundle(source: source, directory: dir)
        let level0 = ImageSpec(payload: PayloadReport(level: .bootstrapOnly, firmwareVersion: "v0.3"), firmware: bundle, outputURL: dir.appending(path: "a.img"))
        #expect(level0.imageSizeBytes(sysrootBytes: 0) == 512 << 20)
        let level2 = ImageSpec(payload: PayloadReport(level: .fullSystem, firmwareVersion: "v0.3"), firmware: bundle, outputURL: dir.appending(path: "b.img"))
        #expect(level2.imageSizeBytes(sysrootBytes: 100 << 20) == (256 + 1024) << 20)
        let big = level2.imageSizeBytes(sysrootBytes: 2000 << 20)
        #expect(big % (64 << 20) == 0)
        #expect(big >= (256 << 20) + Int64(Double(2000 << 20) * 1.25))
        let qemu = ImageSpec(payload: PayloadReport(level: .fullSystem, firmwareVersion: "v0.3"), firmware: bundle, outputURL: dir.appending(path: "c.img"), qemuVirt: true)
        #expect(!qemu.includesRoot)
        #expect(qemu.targetName == "qemu-virt")
    }
}
