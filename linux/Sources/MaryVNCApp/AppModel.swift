import AppKit
import CoreGraphics
import LiquidPlatinum
import MaryVNCKit
import Network
import Observation

/// The viewer: the Pis in view, the paired ones, and at most one session.
@MainActor @Observable
final class AppModel {
    enum Phase: Equatable {
        case idle
        case connecting(name: String)
        case pairing(name: String)
        case connected
        case waiting(name: String, seconds: Int)
    }

    enum Sheet: Equatable {
        case pair(DiscoveredPi)
        case connect
        case message(title: String, text: String)
        case forget(PairedPi)
    }

    /// Where a session goes, and with which key.
    struct Target: Equatable {
        var name: String
        var endpoint: NWEndpoint
        var piKey: [UInt8]?
        var isUSB: Bool
        var fingerprint: Fingerprint?
        /// Found by the Pi's own Bonjour record, so the key is surely that Pi's; an address could be any Pi.
        var fromDiscovery: Bool
    }

    /// The Pi's own address on the USB cable (MaryOS chapter 13).
    static let usbEndpoint = NWEndpoint.hostPort(host: "10.12.194.1", port: NWEndpoint.Port(rawValue: MaryVNC.port)!)

    var pis: [DiscoveredPi] = []
    var selectionID: String?
    var phase: Phase = .idle
    var sheet: Sheet?
    var desktopName = ""
    var desktopSize = CGSize.zero
    var frame: CGImage?
    var cursorShown = false
    var framesPerSecond = 0
    var bytesPerSecond = 0
    var connectedOverUSB = false
    var settings: ViewerSettings
    var pairing: PairingStore
    let identity: NoiseKeyPair

    @ObservationIgnored private let arguments: [String]
    /// `--test-profile DIR`: this Mac's key, the pairs and the settings in DIR rather than the Keychain and
    /// Application Support, and no browsing, so macOS asks for nothing.
    @ObservationIgnored private let testProfile: URL?
    @ObservationIgnored private let settingsURL: URL
    @ObservationIgnored private let browser = PiBrowser()
    @ObservationIgnored private let decoder = FrameDecoder()
    @ObservationIgnored private var session: PiSession?
    @ObservationIgnored private var target: Target?
    @ObservationIgnored private var backoff = Backoff()
    @ObservationIgnored private var reconnect: Task<Void, Never>?
    @ObservationIgnored private var stayDisconnected = false
    @ObservationIgnored private var statFrames = 0
    @ObservationIgnored private var statBytes = 0
    @ObservationIgnored private var statStart = Date()
    @ObservationIgnored private var startupMessage: Sheet?

    init(arguments: [String]) {
        self.arguments = arguments
        if let index = arguments.firstIndex(of: "--test-profile"), index + 1 < arguments.count {
            testProfile = URL(fileURLWithPath: arguments[index + 1], isDirectory: true)
        } else {
            testProfile = nil
        }
        settingsURL = testProfile?.appending(path: "settings.json") ?? ViewerSettings.defaultURL
        let pairsURL = testProfile?.appending(path: "pairs.json") ?? PairingStore.defaultURL
        settings = ViewerSettings.load(from: settingsURL)
        do {
            pairing = try PairingStore(url: pairsURL)
        } catch {
            pairing = PairingStore(emptyAt: pairsURL)
            startupMessage = .message(title: "The paired Pis could not be read",
                                      text: "\(pairsURL.path) is unreadable (\(error)). MaryVNC starts with no paired Pis and will write a new file when you pair.")
        }
        do {
            if let testProfile {
                identity = try Identity.loadOrCreate(from: FileIdentityStore(url: testProfile.appending(path: "identity.key")))
            } else {
                identity = try Identity.loadOrCreate(from: KeychainIdentityStore())
            }
        } catch {
            identity = NoiseKeyPair.generate()
            startupMessage = .message(title: "The Keychain would not keep this Mac’s key",
                                      text: "\(error). This session uses a key of its own; a Pi paired now will not know this Mac next time.")
        }
    }

    // MARK: What the window shows

    var fingerprint: Fingerprint { Fingerprint(publicKey: identity.publicKey) }
    var selectedPi: DiscoveredPi? { pis.first { $0.id == selectionID } }
    var selectedPaired: PairedPi? { selectedPi?.fingerprint.flatMap { pairing.find($0) } }
    var isSessionActive: Bool { phase != .idle }
    var isConnected: Bool { phase == .connected }
    var canPairSelected: Bool { selectedPi?.isUSB == true && selectedPaired == nil && !isSessionActive }

    var pairedOutOfView: [PairedPi] {
        pairing.pis.filter { paired in !pis.contains { $0.fingerprint == paired.fingerprint } }
    }

    func isPaired(_ pi: DiscoveredPi) -> Bool {
        pi.fingerprint.flatMap { pairing.find($0) } != nil
    }

    func isCurrent(_ pi: DiscoveredPi) -> Bool {
        guard let target, isSessionActive else { return false }
        return target.fingerprint != nil ? target.fingerprint == pi.fingerprint : target.name == pi.name
    }

    /// A paired Pi that this session is for, whether or not Bonjour sees it (a Pi reached by address).
    func isCurrent(_ paired: PairedPi) -> Bool {
        isSessionActive && target?.piKey == paired.publicKey
    }

    var windowTitle: String {
        isConnected && !desktopName.isEmpty ? "MaryVNC — \(desktopName)" : "MaryVNC"
    }

    var statusText: String {
        switch phase {
        case .idle:
            pis.isEmpty ? "Looking for Pis on the USB cable and the network" : "Not connected"
        case let .connecting(name):
            "Connecting to \(name)…"
        case let .pairing(name):
            "Pairing with \(name)…"
        case .connected:
            "\(desktopName) · \(connectedOverUSB ? "USB cable" : "network") · \(Int(desktopSize.width))×\(Int(desktopSize.height)) · \(framesPerSecond) fps · \(ByteCountFormatter.string(fromByteCount: Int64(bytesPerSecond), countStyle: .binary))/s"
        case let .waiting(name, seconds):
            "Lost \(name); trying again in \(seconds) s"
        }
    }

    var emptyState: (symbol: String, title: String, message: String) {
        switch phase {
        case .connecting, .connected: ("display", "Connecting…", "Waiting for the Pi’s desktop.")
        case .pairing: ("cable.connector", "Pairing…", "The Pi and this Mac are exchanging keys over the cable.")
        case .waiting: ("arrow.clockwise", "Reconnecting…", "MaryVNC will keep trying.")
        case .idle where pis.isEmpty && pairing.pis.isEmpty:
            ("cable.connector", "No Pi in view", "Plug a Pi in over USB or join its Wi-Fi.")
        case .idle:
            ("display", "Choose a Pi", "A paired Pi connects on its own. A new Pi pairs over the USB cable.")
        }
    }

    // MARK: Starting

    func start() async {
        if let startupMessage { sheet = startupMessage }
        handleArguments()
        guard testProfile == nil else { return }
        for await found in browser.results() {
            pis = found
            if selectionID == nil { selectionID = found.first?.id }
            autoConnect()
        }
    }

    /// `--connect HOST[:PORT]`, with `--pair` to pair instead of resuming with the most recent Pi's key.
    private func handleArguments() {
        guard let index = arguments.firstIndex(of: "--connect"), index + 1 < arguments.count else { return }
        let value = arguments[index + 1]
        var host = value, port = MaryVNC.port
        if let colon = value.lastIndex(of: ":"), !value.contains("]"), value.firstIndex(of: ":") == colon, let parsed = UInt16(value[value.index(after: colon)...]) {
            host = String(value[..<colon])
            port = parsed
        }
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!)
        if arguments.contains("--pair") {
            connect(Target(name: host, endpoint: endpoint, piKey: nil, isUSB: false, fingerprint: nil, fromDiscovery: false))
        } else if let recent = pairing.pis.max(by: { ($0.lastConnected ?? $0.paired) < ($1.lastConnected ?? $1.paired) }) {
            connect(Target(name: host, endpoint: endpoint, piKey: recent.publicKey, isUSB: false, fingerprint: recent.fingerprint, fromDiscovery: false))
        } else {
            sheet = .message(title: "No Pi is paired yet",
                             text: "Pair first: ./vnc.sh --connect \(value) --pair, over the USB cable or while `maryvncctl pair-window` is open on the Pi.")
        }
    }

    private func autoConnect() {
        guard phase == .idle, !stayDisconnected, sheet == nil else { return }
        let inView = Set(pis.compactMap(\.fingerprint))
        guard let paired = pairing.mostRecent(among: inView), let pi = pis.first(where: { $0.fingerprint == paired.fingerprint }) else { return }
        selectionID = pi.id
        connect(target(for: pi, key: paired.publicKey))
    }

    private func target(for pi: DiscoveredPi, key: [UInt8]?) -> Target {
        Target(name: pi.name, endpoint: pi.isUSB ? Self.usbEndpoint : pi.endpoint, piKey: key, isUSB: pi.isUSB, fingerprint: pi.fingerprint,
               fromDiscovery: true)
    }

    // MARK: Commands

    func select(_ pi: DiscoveredPi) {
        selectionID = pi.id
    }

    func connectSelected() {
        guard let pi = selectedPi else { return }
        if let paired = selectedPaired {
            connect(target(for: pi, key: paired.publicKey))
        } else if pi.isUSB {
            sheet = .pair(pi)
        } else {
            sheet = .message(title: "Pair over the USB cable first",
                             text: "\(pi.name) does not know this Mac yet. Connect the Pi to this Mac with the USB cable and choose Pair over USB; after that it connects over the network too.")
        }
    }

    func pairSelected() {
        if let pi = selectedPi, pi.isUSB { sheet = .pair(pi) }
    }

    func confirmPair(_ pi: DiscoveredPi) {
        sheet = nil
        connect(target(for: pi, key: nil))
    }

    func connectManually(host: String, port: UInt16, pair: Bool) {
        sheet = nil
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port) ?? NWEndpoint.Port(rawValue: MaryVNC.port)!)
        if pair {
            connect(Target(name: host, endpoint: endpoint, piKey: nil, isUSB: false, fingerprint: nil, fromDiscovery: false))
        } else if let recent = pairing.pis.max(by: { ($0.lastConnected ?? $0.paired) < ($1.lastConnected ?? $1.paired) }) {
            connect(Target(name: host, endpoint: endpoint, piKey: recent.publicKey, isUSB: false, fingerprint: recent.fingerprint, fromDiscovery: false))
        } else {
            sheet = .message(title: "No Pi is paired yet", text: "Tick Pair to pair with \(host); its pairing window must be open (`maryvncctl pair-window`).")
        }
    }

    func askToForgetSelected() {
        if let paired = selectedPaired { sheet = .forget(paired) }
    }

    func forget(_ paired: PairedPi) {
        sheet = nil
        if target?.piKey == paired.publicKey { disconnect() }
        try? pairing.forget(paired.fingerprint)
    }

    func disconnect() {
        stayDisconnected = true
        reconnect?.cancel()
        reconnect = nil
        session?.close()
        session = nil
        phase = .idle
        frame = nil
    }

    func refresh() {
        session?.send(.refresh)
    }

    func send(_ message: WireMessage) {
        guard isConnected else { return }
        session?.send(message)
    }

    func setQuality(_ quality: FrameQuality) {
        settings.quality = quality
        try? settings.save(to: settingsURL)
        session?.send(.quality(quality))
    }

    func setAccent(_ accent: ViewerSettings.Accent) {
        settings.accent = accent
        try? settings.save(to: settingsURL)
    }

    func setCommandKey(_ key: CommandKey) {
        settings.commandKey = key
        try? settings.save(to: settingsURL)
    }

    func showAbout() {
        let credits = NSAttributedString(string: "The viewer for MaryOS's remote desktop. Liquid Platinum tokens \(LP.sourceSHA). This Mac’s key: \(fingerprint.display).",
                                         attributes: [.font: NSFont.systemFont(ofSize: 11)])
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
    }

    // MARK: The session

    private func connect(_ target: Target) {
        reconnect?.cancel()
        reconnect = nil
        session?.close()
        stayDisconnected = false
        self.target = target
        let name = Identity.displayName
        let mode: SessionMode = target.piKey.map { .resume(piPublicKey: $0, displayName: name) } ?? .pair(displayName: name, expected: target.fingerprint)
        phase = target.piKey == nil ? .pairing(name: target.name) : .connecting(name: target.name)
        do {
            let session = try PiSession(endpoint: target.endpoint, identity: identity, mode: mode)
            self.session = session
            session.start()
            Task { await run(session) }
        } catch {
            phase = .idle
            sheet = .message(title: "MaryVNC could not start a session", text: "\(error)")
        }
    }

    private func run(_ session: PiSession) async {
        for await event in session.events {
            guard session === self.session else { continue }
            switch event {
            case .connecting, .handshaking:
                break
            case let .paired(piPublicKey):
                if let target {
                    try? pairing.upsert(publicKey: piPublicKey, name: target.name)
                    self.target?.piKey = piPublicKey
                    self.target?.fingerprint = Fingerprint(publicKey: piPublicKey)
                }
            case let .desktop(name, width, height, fingerprint):
                desktopName = name
                desktopSize = CGSize(width: width, height: height)
                await decoder.resize(width: width, height: height)
                phase = .connected
                connectedOverUSB = target?.isUSB ?? false
                backoff.reset()
                try? pairing.touch(fingerprint)
                session.send(.quality(settings.quality))
            case let .frame(seq, rects):
                if let image = await decoder.apply(rects) { frame = image.image }
                session.send(.ack(seq: seq))
                count(bytes: rects.reduce(0) { $0 + $1.jpeg.count })
            case let .cursor(_, _, shown):
                cursorShown = shown
            case let .ended(reason):
                ended(reason)
            }
        }
    }

    private func count(bytes: Int) {
        statFrames += 1
        statBytes += bytes
        let elapsed = Date().timeIntervalSince(statStart)
        if elapsed >= 1 {
            framesPerSecond = Int((Double(statFrames) / elapsed).rounded())
            bytesPerSecond = Int(Double(statBytes) / elapsed)
            statFrames = 0
            statBytes = 0
            statStart = Date()
        }
    }

    private func ended(_ reason: SessionEnd) {
        session = nil
        switch reason {
        case .byViewer:
            phase = .idle
            frame = nil
        case .bye("replaced"):
            settle(title: "Another Mac is watching \(target?.name ?? "the Pi")", text: "A Pi shows its desktop to one Mac at a time. Connect again to take it back.")
        case .bye("forgotten"):
            // The Pi said so after proving its key: drop the pairing here too.
            if let key = target?.piKey { try? pairing.forget(Fingerprint(publicKey: key)) }
            settle(title: "\(target?.name ?? "The Pi") no longer knows this Mac", text: "Pair again over the USB cable.")
        case .notPaired:
            // A silent refusal. Only a Pi that announced itself is surely the one this key was paired with; an
            // address may be another Pi, and the pairing with the right one must survive.
            if let target, target.fromDiscovery, let key = target.piKey { try? pairing.forget(Fingerprint(publicKey: key)) }
            settle(title: "\(target?.name ?? "The Pi") does not know this Mac",
                   text: target?.fromDiscovery == true ? "Pair again over the USB cable."
                       : "Pair with it over the USB cable, or by address with Pair ticked while its pairing window is open.")
        case .pairingRefused:
            settle(title: "\(target?.name ?? "The Pi") did not accept pairing",
                   text: "Pair over the USB cable, or open a pairing window on the Pi first: maryvncctl pair-window.")
        case .wrongPi:
            settle(title: "That Pi proved another key", text: "The key it proved is not the one it announced, so MaryVNC left before sending this Mac’s key.")
        case .bye, .failed:
            retry(reason)
        }
    }

    private func settle(title: String, text: String) {
        stayDisconnected = true
        phase = .idle
        frame = nil
        sheet = .message(title: title, text: text)
    }

    private func retry(_ reason: SessionEnd) {
        guard let target, target.piKey != nil, !stayDisconnected else {
            phase = .idle
            frame = nil
            if case let .failed(text) = reason { sheet = .message(title: "The session ended", text: text) }
            return
        }
        let delay = backoff.next()
        let seconds = max(1, Int((Double(delay.components.seconds) + Double(delay.components.attoseconds) / 1e18).rounded(.up)))
        phase = .waiting(name: target.name, seconds: seconds)
        reconnect = Task { [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled, let self, !self.stayDisconnected else { return }
            self.connect(target)
        }
    }
}

/// A CGImage handed from the decoder to the window.
struct DecodedFrame: @unchecked Sendable {
    let image: CGImage
}

/// Applies frames off the main thread; the framebuffer lives here and nowhere else.
actor FrameDecoder {
    private var framebuffer: Framebuffer?

    func resize(width: Int, height: Int) {
        if framebuffer?.width != width || framebuffer?.height != height {
            framebuffer = try? Framebuffer(width: width, height: height)
        }
    }

    func apply(_ rects: [FrameRect]) -> DecodedFrame? {
        guard let framebuffer else { return nil }
        for rect in rects {
            try? framebuffer.apply(rect)
        }
        return framebuffer.makeImage().map(DecodedFrame.init)
    }
}
