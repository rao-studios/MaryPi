import Foundation
import Testing
@testable import MaryOSKit

@Suite struct BuildLogTests {
    @Test func recognisesBuilderStages() {
        #expect(BuildLog.stage(in: "==> stage: rootfs_build") == .rootfs)
        #expect(BuildLog.stage(in: "==> stage: ui_build") == .ui)
        #expect(BuildLog.stage(in: "==> stage: mary_build") == .mary)
        #expect(BuildLog.stage(in: "==> stage: target_build vm") == .target(.vm))
        #expect(BuildLog.stage(in: "\u{1B}[1;36m==> stage: image_build pi5\u{1B}[0m") == .image(.pi5))
        #expect(BuildLog.stage(in: "==> stage: target_build moon") == nil)
        #expect(BuildLog.stage(in: "==> apt-get install foo") == nil)
        #expect(BuildLog.stage(in: "Get:1 http://ports.ubuntu.com noble InRelease") == nil)
    }

    @Test func artifactsForEachTarget() throws {
        let config = try DistroConfig(conf: ConfParser.parse(DistroConfigTests.sample))
        let paths = KitPaths(root: URL(fileURLWithPath: "/kit"), isCheckout: true, outDirectory: URL(fileURLWithPath: "/kit/out"), stateRoot: URL(fileURLWithPath: "/kit/state"))
        let pi5 = BuildArtifacts.locate(target: .pi5, config: config, paths: paths)
        #expect(pi5.image.path == "/kit/out/maryos-24.04-pi5.img")
        #expect(pi5.kernel == nil)
        #expect(pi5.requiredFiles.count == 1)
        let vm = BuildArtifacts.locate(target: .vm, config: config, paths: paths)
        #expect(vm.kernel?.path == "/kit/out/vm/Image")
        #expect(vm.initrd?.path == "/kit/out/vm/initrd.img")
        #expect(vm.bootInfo?.path == "/kit/out/vm/boot.json")
        #expect(vm.requiredFiles.count == 4)
        #expect(!vm.isComplete)
    }

    @Test func desktopArtifacts() {
        let paths = KitPaths(root: URL(fileURLWithPath: "/kit"), isCheckout: true, outDirectory: URL(fileURLWithPath: "/kit/out"), stateRoot: URL(fileURLWithPath: "/kit/state"))
        let ui = UIArtifacts.locate(paths: paths)
        #expect(ui.binary.path == "/kit/out/ui/usr/bin/maryui-desktop")
        #expect(ui.versionFile.path == "/kit/out/ui/usr/share/maryui/maryui.env")
        #expect(!ui.isBuilt)
        #expect(ui.summary.hasPrefix("not built yet"))
        #expect(BuildRunner.targetlessStages.contains("ui") && !BuildRunner.targetlessStages.contains("image"))
    }

    @Test func maryArtifacts() {
        let paths = KitPaths(root: URL(fileURLWithPath: "/kit"), isCheckout: true, outDirectory: URL(fileURLWithPath: "/kit/out"), stateRoot: URL(fileURLWithPath: "/kit/state"))
        let mary = MaryArtifacts.locate(paths: paths)
        #expect(paths.marySource.path == "/kit/mary")
        #expect(mary.binaries.path == "/kit/out/mary/usr/bin")
        #expect(mary.versionFile.path == "/kit/out/mary/usr/share/doc/mary/mary.env")
        #expect(!mary.isBuilt)
        #expect(mary.summary.hasPrefix("not built yet"))
        #expect(BuildRunner.targetlessStages.contains("mary"))
    }

    @Test func bootInfoDecodes() throws {
        let json = #"{"kernelVersion":"6.8.0-79-generic","kernel":"Image","initrd":"initrd.img","cmdline":"console=hvc0 root=LABEL=maryos-root rootfstype=ext4 rw rootwait","built":"2026-09-07T00:00:00Z"}"#
        let url = FileManager.default.temporaryDirectory.appending(path: "boot-\(UUID().uuidString).json")
        try json.write(to: url, atomically: true, encoding: .utf8)
        defer { try? FileManager.default.removeItem(at: url) }
        let info = try VMBootInfo.load(from: url)
        #expect(info.kernelVersion == "6.8.0-79-generic")
        #expect(info.cmdline.hasPrefix("console=hvc0"))
    }
}

@Suite struct StepPlanTests {
    @Test func buildAndFlash() {
        let plan = StepPlan.standard(build: true, hasTarget: true)
        #expect(plan.steps.map(\.kind) == [.doctor, .buildRootfs, .buildUI, .buildMary, .buildTarget, .buildImage, .verifyTarget, .unmount, .write, .eject])
        #expect(StepPlan.standard(build: false, hasTarget: true).steps.map(\.kind) == [.doctor, .verifyTarget, .unmount, .write, .eject])
        #expect(StepPlan.standard(build: true, hasTarget: false).steps.map(\.kind) == [.doctor, .buildRootfs, .buildUI, .buildMary, .buildTarget, .buildImage])
    }

    @Test func setUpdatesStatusAndProgress() {
        var plan = StepPlan.standard(build: true, hasTarget: false)
        plan.set(.buildImage, .running, progress: 0.5)
        #expect(plan[.buildImage]?.progress == 0.5)
        plan.set(.buildImage, .done, detail: "maryos-24.04-vm.img")
        #expect(plan[.buildImage]?.progress == 1)
        #expect(plan[.buildImage]?.detail == "maryos-24.04-vm.img")
        #expect(!plan.isFinished)
        plan.set(.doctor, .done); plan.set(.buildRootfs, .done); plan.set(.buildUI, .done); plan.set(.buildMary, .done); plan.set(.buildTarget, .failed("x"))
        #expect(plan.isFinished)
        #expect(plan.failedStep?.kind == .buildTarget)
        #expect(StepKind.write.isPrivileged && !StepKind.buildImage.isPrivileged)
    }
}

@Suite struct SharedDirectoryTests {
    @Test func parsesForms() throws {
        let dir = FileManager.default.temporaryDirectory
        let plain = try SharedDirectory.parse(dir.path)
        #expect(plain.tag == dir.lastPathComponent)
        #expect(!plain.readOnly)
        let tagged = try SharedDirectory.parse("stuff=\(dir.path):ro")
        #expect(tagged.tag == "stuff")
        #expect(tagged.readOnly)
        #expect(tagged.url.path == dir.standardizedFileURL.path)
        #expect(throws: MaryOSError.self) { _ = try SharedDirectory.parse("nope=/definitely/not/here") }
    }
}
