import Darwin
import Foundation
import Network
import Testing
@testable import MaryVNCKit

@Suite struct BackoffTests {
    @Test func delaysDoubleToTheCapAndReset() {
        var backoff = Backoff()
        let delays = (0..<7).map { _ in backoff.next(jitter: 1) }
        #expect(delays == [.milliseconds(500), .seconds(1), .seconds(2), .seconds(4), .seconds(8), .seconds(8), .seconds(8)])
        backoff.reset()
        #expect(backoff.next(jitter: 1.2) == .milliseconds(600))
    }
}

/// Against the real server: set MARYVNCD to a maryvncd built on this Mac (make -C maryvnc all in MaryOS),
/// e.g. `MARYVNCD=../../MaryOS/maryvnc/build/bin/maryvncd swift test --filter MaryvncdInteropTests`.
/// maryvncctl is taken from the same directory. Skipped without it.
@Suite(.enabled(if: ProcessInfo.processInfo.environment["MARYVNCD"] != nil), .serialized)
struct MaryvncdInteropTests {
    struct Daemon {
        let process: Process
        let directory: URL
        let port: UInt16
        let nearbyPort: UInt16
        let control: String

        static func start(binary: String) throws -> Daemon {
            let directory = FileManager.default.temporaryDirectory.appending(path: "maryvncd-\(UUID().uuidString.prefix(8))")
            let state = directory.appending(path: "state"), runtime = directory.appending(path: "run")
            try FileManager.default.createDirectory(at: runtime, withIntermediateDirectories: true)
            let initialise = Process()
            initialise.executableURL = URL(fileURLWithPath: binary)
            initialise.arguments = ["init", "--state", state.path]
            initialise.standardError = FileHandle.nullDevice
            try initialise.run()
            initialise.waitUntilExit()
            let port = UInt16.random(in: 30000...45000), nearbyPort = UInt16.random(in: 45001...60000)
            let process = Process()
            process.executableURL = URL(fileURLWithPath: binary)
            process.arguments = ["--state", state.path, "--runtime", runtime.path, "--port", "\(port)", "--bind", "127.0.0.1",
                                 "--nearby-port", "\(nearbyPort)", "--pair-window", "120", "--test-pattern", "320x200"]
            let log = directory.appending(path: "maryvncd.log")
            FileManager.default.createFile(atPath: log.path, contents: nil)
            process.standardError = try FileHandle(forWritingTo: log)
            try process.run()
            return Daemon(process: process, directory: directory, port: port, nearbyPort: nearbyPort,
                          control: runtime.appending(path: "control.sock").path)
        }

        func waitUntilListening() async throws {
            for _ in 0..<50 {
                let fd = socket(AF_INET, SOCK_STREAM, 0)
                var address = sockaddr_in(sin_len: UInt8(MemoryLayout<sockaddr_in>.size), sin_family: sa_family_t(AF_INET),
                                          sin_port: port.bigEndian, sin_addr: in_addr(s_addr: inet_addr("127.0.0.1")), sin_zero: (0, 0, 0, 0, 0, 0, 0, 0))
                let connected = withUnsafePointer(to: &address) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
                }
                close(fd)
                if connected == 0 { return }
                try await Task.sleep(for: .milliseconds(100))
            }
            throw MaryVNCError("maryvncd did not listen on \(port)")
        }

        func stop() {
            if let log = try? String(contentsOf: directory.appending(path: "maryvncd.log"), encoding: .utf8) {
                print("maryvncd log:\n\(log)")
            }
            process.terminate()
            process.waitUntilExit()
            try? FileManager.default.removeItem(at: directory)
        }

        var endpoint: NWEndpoint { .hostPort(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!) }

        /// maryvncctl from beside maryvncd, against this daemon's control socket; its exit status.
        func control(_ arguments: [String], binary: String) throws -> Int32 {
            let ctl = Process()
            ctl.executableURL = URL(fileURLWithPath: binary).deletingLastPathComponent().appending(path: "maryvncctl")
            ctl.arguments = ["--socket", control] + arguments
            ctl.standardOutput = FileHandle.nullDevice
            try ctl.run()
            ctl.waitUntilExit()
            return ctl.terminationStatus
        }
    }

    struct Outcome {
        var piKey: [UInt8]?
        var desktop: (width: Int, height: Int, fingerprint: Fingerprint)?
        var frames = 0
        var end: SessionEnd?
    }

    /// Runs a session until it ends, acknowledging and decoding frames; input after the first, close after `frames`.
    func run(_ session: PiSession, frames wanted: Int) async throws -> Outcome {
        try await withThrowingTaskGroup(of: Outcome.self) { group in
            group.addTask {
                var outcome = Outcome()
                var framebuffer: Framebuffer?
                session.start()
                for await event in session.events {
                    switch event {
                    case let .paired(key):
                        outcome.piKey = key
                    case let .desktop(_, width, height, fingerprint):
                        outcome.desktop = (width, height, fingerprint)
                        framebuffer = try Framebuffer(width: width, height: height)
                    case let .frame(seq, rects):
                        for rect in rects { try framebuffer?.apply(rect) }
                        session.send(.ack(seq: seq))
                        outcome.frames += 1
                        if outcome.frames == 1 {
                            session.send(.pointer(x: 100, y: 50, buttons: [.left], scrollX: 0, scrollY: 0))
                            session.send(.key(evdev: 30, pressed: true))
                        }
                        if outcome.frames == wanted { session.close() }
                    case let .ended(reason):
                        outcome.end = reason
                    default:
                        break
                    }
                }
                return outcome
            }
            group.addTask {
                try await Task.sleep(for: .seconds(15))
                throw MaryVNCError("the session did not end within 15 s")
            }
            let outcome = try await group.next()!
            group.cancelAll()
            return outcome
        }
    }

    @Test func pairResumeAndBeRefusedByTheRealServer() async throws {
        let binary = ProcessInfo.processInfo.environment["MARYVNCD"]!
        let daemon = try Daemon.start(binary: binary)
        defer { daemon.stop() }
        try await daemon.waitUntilListening()
        let mac = NoiseKeyPair.generate()

        let paired = try await run(PiSession(endpoint: daemon.endpoint, identity: mac, mode: .pair(displayName: "swift test", expected: nil)), frames: 3)
        let piKey = try #require(paired.piKey)
        #expect(paired.desktop?.width == 320 && paired.desktop?.height == 200)
        #expect(paired.desktop?.fingerprint == Fingerprint(publicKey: piKey))
        // A frame already on its way when the viewer closes still arrives, so at least, not exactly.
        #expect(paired.frames >= 3, "\(String(describing: paired.end))")
        #expect(paired.end == .byViewer)

        let resumed = try await run(PiSession(endpoint: daemon.endpoint, identity: mac, mode: .resume(piPublicKey: piKey, displayName: "swift test")), frames: 2)
        #expect(resumed.frames >= 2, "ended \(String(describing: resumed.end)) after \(resumed.frames) frames")
        #expect(resumed.end == .byViewer)

        // Pairing closed the window; open it again. A pairing that expects another fingerprint is left before this
        // Mac's key is sent.
        #expect(try daemon.control(["pair-window", "120"], binary: binary) == 0)
        let impostor = try await run(PiSession(endpoint: daemon.endpoint, identity: NoiseKeyPair.generate(),
                                               mode: .pair(displayName: "x", expected: Fingerprint(publicKey: NoiseKeyPair.generate().publicKey))), frames: 1)
        #expect(impostor.end == .wrongPi)

        #expect(try daemon.control(["pair-window", "0"], binary: binary) == 0)

        let stranger = try await run(PiSession(endpoint: daemon.endpoint, identity: NoiseKeyPair.generate(), mode: .pair(displayName: "x", expected: nil)), frames: 1)
        #expect(stranger.end == .pairingRefused)
        let unknown = try await run(PiSession(endpoint: daemon.endpoint, identity: NoiseKeyPair.generate(), mode: .resume(piPublicKey: piKey, displayName: "x")), frames: 1)
        #expect(unknown.end == .notPaired)
    }

    /// The next answer the scanner hears, or nil after `limit`.
    func next(from scanner: NearbyScanner, within limit: Duration = .seconds(3)) async throws -> NearbyHeard? {
        try await withThrowingTaskGroup(of: NearbyHeard?.self) { group in
            group.addTask {
                for await heard in scanner.heard { return heard }
                return nil
            }
            group.addTask {
                try await Task.sleep(for: limit)
                return nil
            }
            let first = try await group.next() ?? nil
            group.cancelAll()
            return first
        }
    }

    @Test func nearbyAnswersItsPairsAndAnOpenWindowAndNobodyElse() async throws {
        let binary = ProcessInfo.processInfo.environment["MARYVNCD"]!
        let daemon = try Daemon.start(binary: binary)
        defer { daemon.stop() }
        try await daemon.waitUntilListening()
        let piKey = try [UInt8](Data(contentsOf: daemon.directory.appending(path: "state/identity.pub")))
        let mac = NoiseKeyPair.generate()
        let scanner = NearbyScanner(port: daemon.nearbyPort)
        try scanner.start()
        defer { scanner.stop() }

        // The window is open: a Mac paired with nothing hears the Pi's key and name, and where maryvncd listens.
        scanner.call(keys: [], addresses: ["127.0.0.1"], multicast: false)
        let offer = try await next(from: scanner)
        #expect(offer?.offer?.publicKey == piKey)
        #expect(offer?.piPublicKey == nil)
        #expect(offer?.port == daemon.port)
        #expect(offer?.host == "127.0.0.1")

        // It pairs with the key the offer carried, and that closes the window.
        let paired = try await run(PiSession(endpoint: daemon.endpoint, identity: mac,
                                             mode: .pair(displayName: "swift test", expected: Fingerprint(publicKey: piKey))), frames: 1)
        #expect(paired.piKey == piKey)

        // The paired Mac's call comes back with its tag and no offer (the Pi answers one address twice a second at most).
        try await Task.sleep(for: .milliseconds(1100))
        scanner.call(keys: [try NearbyKey(identity: mac, piPublicKey: piKey)], addresses: ["127.0.0.1"], multicast: false)
        let tagged = try await next(from: scanner)
        #expect(tagged?.piPublicKey == piKey)
        #expect(tagged?.offer == nil)

        // A Mac the Pi does not know gets nothing, even calling with the Pi's key.
        scanner.call(keys: [try NearbyKey(identity: NoiseKeyPair.generate(), piPublicKey: piKey)], addresses: ["127.0.0.1"], multicast: false)
        #expect(try await next(from: scanner, within: .milliseconds(800)) == nil)
    }
}
