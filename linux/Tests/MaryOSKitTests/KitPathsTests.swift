import Foundation
import Testing
@testable import MaryOSKit

@Suite struct KitPathsTests {
    func makeKit(checkout: Bool) throws -> URL {
        let root = FileManager.default.temporaryDirectory.appending(path: "maryos-kit-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root.appending(path: "distro"), withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: root.appending(path: "builder"), withIntermediateDirectories: true)
        try "DISTRO_ID=x\n".write(to: root.appending(path: "distro/distro.conf"), atomically: true, encoding: .utf8)
        try "#!/bin/sh\n".write(to: root.appending(path: "builder/build.sh"), atomically: true, encoding: .utf8)
        if checkout {
            try "// swift-tools-version: 6.0\n".write(to: root.appending(path: "Package.swift"), atomically: true, encoding: .utf8)
        }
        return root
    }

    @Test func findsTheKitByWalkingUp() throws {
        let root = try makeKit(checkout: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let deep = root.appending(path: "Sources/MaryOSKit/VM")
        try FileManager.default.createDirectory(at: deep, withIntermediateDirectories: true)
        let paths = try #require(KitPaths.locate(environment: [:], currentDirectory: deep, bundleResources: nil, home: URL(fileURLWithPath: "/tmp/home")))
        #expect(paths.root.standardizedFileURL.path == root.standardizedFileURL.path)
        #expect(paths.isCheckout)
        #expect(paths.outDirectory.lastPathComponent == "out")
        #expect(paths.stateRoot.lastPathComponent == "state")
        #expect(paths.state(for: .vm).directory.path.hasSuffix("state/vm/vm"))
    }

    @Test func findsLinuxSubdirectoryFromRepositoryRoot() throws {
        let repo = FileManager.default.temporaryDirectory.appending(path: "maryos-repo-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: repo) }
        let kit = repo.appending(path: "linux")
        try FileManager.default.createDirectory(at: kit.appending(path: "distro"), withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: kit.appending(path: "builder"), withIntermediateDirectories: true)
        try "DISTRO_ID=x\n".write(to: kit.appending(path: "distro/distro.conf"), atomically: true, encoding: .utf8)
        try "#!/bin/sh\n".write(to: kit.appending(path: "builder/build.sh"), atomically: true, encoding: .utf8)
        let paths = try #require(KitPaths.locate(environment: [:], currentDirectory: repo, bundleResources: nil))
        #expect(paths.root.standardizedFileURL.path == kit.standardizedFileURL.path)
    }

    @Test func environmentAndExplicitOverrides() throws {
        let root = try makeKit(checkout: false)
        defer { try? FileManager.default.removeItem(at: root) }
        let home = URL(fileURLWithPath: "/tmp/home")
        let viaEnv = try #require(KitPaths.locate(environment: [KitPaths.environmentKey: root.path, KitPaths.outEnvironmentKey: "/tmp/out"],
                                                  currentDirectory: URL(fileURLWithPath: "/"), bundleResources: nil, home: home))
        #expect(!viaEnv.isCheckout)
        #expect(viaEnv.outDirectory.path == "/tmp/out")
        #expect(viaEnv.stateRoot.path == "/tmp/home/Library/Caches/MaryOS/state")
        let explicit = try #require(KitPaths.locate(explicitRoot: root.path, environment: [:], currentDirectory: URL(fileURLWithPath: "/"), bundleResources: nil, home: home))
        #expect(explicit.root.standardizedFileURL.path == root.standardizedFileURL.path)
        #expect(explicit.outDirectory.path == "/tmp/home/Library/Caches/MaryOS/out")
    }

    @Test func fallsBackToThisCheckout() throws {
        let paths = try #require(KitPaths.locate(environment: [:], currentDirectory: URL(fileURLWithPath: "/"), bundleResources: nil))
        #expect(FileManager.default.fileExists(atPath: paths.distroConfig.path))
        #expect(FileManager.default.fileExists(atPath: paths.buildScript.path))
    }
}
