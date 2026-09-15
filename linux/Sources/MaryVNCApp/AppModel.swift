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
        case pair(NearbyPi)
        case connect
        case message(title: String, text: String)
        case forget(PairedPi)
    }

    /// Where a session goes, and with which key.
    struct Target: Equatable {
        var name: String
        var endpoint: NWEndpoint
        /// The address to remember for the next Nearby call.
        var host: String?
        var piKey: [UInt8]?
        var fingerprint: Fingerprint?
    }

    var pis: [NearbyPi] = []
    var selectionID: String?
    var phase: Phase = .idle
    var sheet: Sheet?
    var desktopName = ""
    var desktopSize = CGSize.zero
    var frame: CGImage?
    var cursorShown = false
    var framesPerSecond = 0
    var bytesPerSecond = 0
    var settings: ViewerSettings
    var pairing: PairingStore
    let identity: NoiseKeyPair

    @ObservationIgnored private let arguments: [String]
    /// `--test-profile DIR`: this Mac's key, the pairs and the settings in DIR rather than the Keychain and
    /// Application Support, and no Nearby calls unless `--nearby` is given too, so macOS asks for nothing.
    @ObservationIgnored private let testProfile: URL?
    @ObservationIgnored private let settingsURL: URL
    @ObservationIgnored private let scanner = NearbyScanner()
    @ObservationIgnored private let pathMonitor = NWPathMonitor()
    @ObservationIgnored private let decoder = FrameDecoder()
    @ObservationIgnored private var session: PiSession?
    @ObservationIgnored private var target: Target?
    @ObservationIgnored private var backoff = Backoff()
    @ObservationIgnored private var reconnect: Task<Void, Never>?
    @ObservationIgnored private var calling: Task<Void, Never>?
    /// Until then, a call every 5 s rather than every 15.
    @ObservationIgnored private var callOftenUntil = Date.distantPast
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
    var selectedPi: NearbyPi? { pis.first { $0.id == selectionID } }
    var selectedPaired: PairedPi? { selectedPi.flatMap { pairing.find($0.fingerprint) } }
    var isSessionActive: Bool { phase != .idle }
    var isConnected: Bool { phase == .connected }
    var canPairSelected: Bool { selectedPi?.isPairable == true && selectedPaired == nil && !isSessionActive }

    /// Pis whose pairing window is open and that do not know this Mac.
    var readyToPair: [NearbyPi] { pis.filter { !$0.isPaired } }
    /// Paired Pis that answered.
    var pairedNearby: [NearbyPi] { pis.filter(\.isPaired) }

    var pairedOutOfView: [PairedPi] {
        pairing.pis.filter { paired in !pis.contains { $0.isPaired && $0.fingerprint == paired.fingerprint } }
    }

    func isCurrent(_ pi: NearbyPi) -> Bool {
        isSessionActive && target?.fingerprint == pi.fingerprint
    }

    /// A paired Pi that this session is for, whether or not it answered a call (a Pi reached by address).
    func isCurrent(_ paired: PairedPi) -> Bool {
        isSessionActive && target?.piKey == paired.publicKey
    }

    var windowTitle: String {
        isConnected && !desktopName.isEmpty ? "MaryVNC — \(desktopName)" : "MaryVNC"
    }

    var statusText: String {
        switch phase {
        case .idle:
            pis.isEmpty ? "Looking for Pis nearby" : "Not connected"
        case let .connecting(name):
            "Connecting to \(name)…"
        case let .pairing(name):
            "Pairing with \(name)…"
        case .connected:
            "\(desktopName) · \(Int(desktopSize.width))×\(Int(desktopSize.height)) · \(framesPerSecond) fps · \(ByteCountFormatter.string(fromByteCount: Int64(bytesPerSecond), countStyle: .binary))/s"
        case let .waiting(name, seconds):
            "Lost \(name); looking for it, trying again in \(seconds) s"
        }
    }

    var emptyState: (symbol: String, title: String, message: String) {
        switch phase {
        case .connecting, .connected: ("display", "Connecting…", "Waiting for the Pi’s desktop.")
        case .pairing: ("link", "Pairing…", "The Pi and this Mac are exchanging keys.")
        case .waiting: ("arrow.clockwise", "Reconnecting…", "MaryVNC is looking for the Pi and will keep trying.")
        case .idle where pis.isEmpty && pairing.pis.isEmpty:
            ("dot.radiowaves.left.and.right", "No Pi nearby", "Press the power button on a MaryOS Pi to make it ready to pair, or use Connect to Address….")
        case .idle:
            ("display", "Choose a Pi", "A paired Pi connects on its own. A Pi ready to pair pairs with a click.")
        }
    }

    // MARK: Starting

    func start() async {
        if let startupMessage { sheet = startupMessage }
        handleArguments()
        guard testProfile == nil || arguments.contains("--nearby") else { return }
        do {
            try scanner.start()
        } catch {
            sheet = .message(title: "MaryVNC cannot look for Pis", text: "\(error). Connect to Address… still works.")
            return
        }
        watchTheNetwork()
        lookAgain()
        calling = Task { [weak self] in await self?.keepCalling() }
        for await heard in scanner.heard {
            hear(heard)
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
            connect(Target(name: host, endpoint: endpoint, host: host, piKey: nil, fingerprint: nil))
        } else if let recent = pairing.pis.max(by: { ($0.lastConnected ?? $0.paired) < ($1.lastConnected ?? $1.paired) }) {
            connect(Target(name: host, endpoint: endpoint, host: host, piKey: recent.publicKey, fingerprint: recent.fingerprint))
        } else {
            sheet = .message(title: "No Pi is paired yet",
                             text: "Pair first: ./vnc.sh --connect \(value) --pair while the Pi’s pairing window is open.")
        }
    }

    // MARK: Nearby

    /// Calls again now, and every 5 s for the next two minutes: at start, when the network changes, when the Mac
    /// wakes, on Look Again, and while a lost Pi is being looked for.
    func lookAgain() {
        callOftenUntil = Date().addingTimeInterval(120)
        if phase != .connected { callNow() }
    }

    private func keepCalling() async {
        while !Task.isCancelled {
            let wait: Duration = Date() < callOftenUntil ? .seconds(5) : .seconds(15)
            try? await Task.sleep(for: wait)
            if phase != .connected { callNow() }
        }
    }

    private func callNow() {
        let now = Date()
        pis.removeAll { now.timeIntervalSince($0.lastSeen) > 40 && !isCurrent($0) }
        let recent = pairing.pis.sorted { ($0.lastConnected ?? $0.paired) > ($1.lastConnected ?? $1.paired) }.prefix(Nearby.tagsMax)
        let keys = recent.compactMap { try? NearbyKey(identity: identity, piPublicKey: $0.publicKey) }
        scanner.call(keys: keys, addresses: recent.compactMap(\.lastAddress))
    }

    private func watchTheNetwork() {
        pathMonitor.pathUpdateHandler = { [weak self] path in
            guard path.status == .satisfied else { return }
            Task { @MainActor in self?.lookAgain() }
        }
        pathMonitor.start(queue: DispatchQueue(label: "com.maryos.MaryVNC.path"))
        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.lookAgain() }
        }
    }

    /// An answer to one of this Mac's calls.
    private func hear(_ heard: NearbyHeard) {
        let now = Date()
        let found: NearbyPi
        if let key = heard.piPublicKey, let paired = pairing.find(Fingerprint(publicKey: key)) {
            // The tag proves this is the Pi paired with this Mac, so where it answered from is worth keeping.
            found = NearbyPi(publicKey: key, name: heard.offer?.name ?? paired.name, host: heard.host, port: heard.port,
                             isPaired: true, isPairable: heard.offer != nil, lastSeen: now)
            try? pairing.remember(address: heard.host, for: paired.fingerprint)
        } else if let offer = heard.offer, pairing.find(Fingerprint(publicKey: offer.publicKey)) == nil {
            found = NearbyPi(publicKey: offer.publicKey, name: offer.name, host: heard.host, port: heard.port,
                             isPaired: false, isPairable: true, lastSeen: now)
        } else {
            // An offer without a tag naming a Pi this Mac is paired with proves nothing about where that Pi is.
            return
        }
        if let index = pis.firstIndex(where: { $0.id == found.id }) {
            pis[index] = found
        } else {
            pis.append(found)
            pis.sort { ($0.isPaired ? 0 : 1, $0.name) < ($1.isPaired ? 0 : 1, $1.name) }
        }
        if selectedPi == nil { selectionID = found.id }
        autoConnect()
    }

    private func autoConnect() {
        guard phase == .idle, !stayDisconnected, sheet == nil else { return }
        let inView = Set(pairedNearby.map(\.fingerprint))
        guard let paired = pairing.mostRecent(among: inView), let pi = pairedNearby.first(where: { $0.fingerprint == paired.fingerprint }) else { return }
        selectionID = pi.id
        connect(target(for: pi, key: paired.publicKey))
    }

    private func target(for pi: NearbyPi, key: [UInt8]?) -> Target {
        Target(name: pi.name, endpoint: pi.endpoint, host: pi.host, piKey: key, fingerprint: pi.fingerprint)
    }

    /// The target at the address its Pi answered from most recently.
    private func freshened(_ target: Target) -> Target {
        guard let fingerprint = target.fingerprint, let pi = pairedNearby.first(where: { $0.fingerprint == fingerprint }) else { return target }
        var fresh = target
        fresh.endpoint = pi.endpoint
        fresh.host = pi.host
        return fresh
    }

    // MARK: Commands

    func select(_ pi: NearbyPi) {
        selectionID = pi.id
    }

    func connectSelected() {
        guard let pi = selectedPi else { return }
        if let paired = selectedPaired {
            connect(target(for: pi, key: paired.publicKey))
        } else if pi.isPairable {
            sheet = .pair(pi)
        }
    }

    func pairSelected() {
        if canPairSelected, let pi = selectedPi { sheet = .pair(pi) }
    }

    func confirmPair(_ pi: NearbyPi) {
        sheet = nil
        connect(target(for: pi, key: nil))
    }

    func connectManually(host: String, port: UInt16, pair: Bool) {
        sheet = nil
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port) ?? NWEndpoint.Port(rawValue: MaryVNC.port)!)
        if pair {
            connect(Target(name: host, endpoint: endpoint, host: host, piKey: nil, fingerprint: nil))
        } else if let recent = pairing.pis.max(by: { ($0.lastConnected ?? $0.paired) < ($1.lastConnected ?? $1.paired) }) {
            connect(Target(name: host, endpoint: endpoint, host: host, piKey: recent.publicKey, fingerprint: recent.fingerprint))
        } else {
            sheet = .message(title: "No Pi is paired yet",
                             text: "Tick Pair to pair with \(host) while its pairing window is open: a press of its power button, or maryvncctl pair-window.")
        }
    }

    func askToForgetSelected() {
        if let paired = selectedPaired { sheet = .forget(paired) }
    }

    func forget(_ paired: PairedPi) {
        sheet = nil
        if target?.piKey == paired.publicKey { disconnect() }
        try? pairing.forget(paired.fingerprint)
        pis.removeAll { $0.fingerprint == paired.fingerprint }
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

    /// Connected: the whole screen again. Otherwise: call for Pis again.
    func refresh() {
        if isConnected { session?.send(.refresh) } else { lookAgain() }
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
                    if let index = pis.firstIndex(where: { $0.publicKey == piPublicKey }) {
                        pis[index].isPaired = true
                        pis[index].isPairable = false
                    }
                }
            case let .desktop(name, width, height, fingerprint):
                desktopName = name
                desktopSize = CGSize(width: width, height: height)
                await decoder.resize(width: width, height: height)
                phase = .connected
                backoff.reset()
                try? pairing.touch(fingerprint, address: target?.host)
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
        let name = target?.name ?? "The Pi"
        switch reason {
        case .byViewer:
            phase = .idle
            frame = nil
        case .bye("replaced"):
            settle(title: "Another Mac is watching \(name)", text: "A Pi shows its desktop to one Mac at a time. Connect again to take it back.")
        case .bye("forgotten"):
            // The Pi said so after proving its key: drop the pairing here too.
            if let key = target?.piKey {
                try? pairing.forget(Fingerprint(publicKey: key))
                pis.removeAll { $0.publicKey == key }
            }
            settle(title: "\(name) no longer knows this Mac", text: "Pair again: press the Pi’s power button, then click Pair.")
        case .notPaired:
            // A silent refusal proves nothing about who refused: an address can be stale, or another machine's. So the
            // pairing stays, and Forget This Pi… is there if the Pi really has forgotten this Mac.
            settle(title: "\(name) did not accept this Mac",
                   text: "Either the Pi has forgotten this Mac, or another machine answered at that address. If maryvncctl pairs on the Pi no longer lists this Mac, choose Forget This Pi… and pair again.")
        case .pairingRefused:
            settle(title: "\(name) did not accept pairing",
                   text: "Its pairing window is closed: it closes after two minutes, and as soon as a Mac pairs. Press the Pi’s power button (or run maryvncctl pair-window on it), then click Pair again.")
        case .wrongPi:
            settle(title: "That Pi proved another key", text: "The key it proved is not the one its answer carried, so MaryVNC left before sending this Mac’s key.")
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
        lookAgain()
        reconnect = Task { [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled, let self, !self.stayDisconnected else { return }
            self.connect(self.freshened(target))
        }
    }
}

extension NearbyPi {
    var endpoint: NWEndpoint {
        .hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port) ?? NWEndpoint.Port(rawValue: Nearby.port)!)
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
