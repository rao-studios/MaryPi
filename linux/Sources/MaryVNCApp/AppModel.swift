import AppKit
import CoreGraphics
import LiquidPlatinum
import MaryVNCKit
import MaryVNCViewer
import Network
import Observation

/// The viewer: the Pis in view, the paired ones, and at most one session.
@MainActor @Observable
final class AppModel {
    typealias Phase = PiLink.Phase

    enum Sheet: Equatable {
        case pair(NearbyPi)
        case connect
        case message(title: String, text: String)
        case forget(PairedPi)
    }

    private(set) var nearby = NearbyList()
    var selectionID: String?
    var sheet: Sheet?
    var settings: ViewerSettings
    var pairing: PairingStore
    let identity: NoiseKeyPair
    let link = PiLink()

    @ObservationIgnored private let arguments: [String]
    /// `--test-profile DIR`: this Mac's key, the pairs and the settings in DIR rather than the Keychain and
    /// Application Support, and no Nearby calls unless `--nearby` is given too, so macOS asks for nothing.
    @ObservationIgnored private let testProfile: URL?
    @ObservationIgnored private let settingsURL: URL
    @ObservationIgnored private let scanner = NearbyScanner()
    @ObservationIgnored private let pathMonitor = NWPathMonitor()
    @ObservationIgnored private var calling: Task<Void, Never>?
    /// Until then, a call every 5 s rather than every 15.
    @ObservationIgnored private var callOftenUntil = Date.distantPast
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
        link.quality = settings.quality
        link.onPaired = { [unowned self] key, target in
            try? pairing.upsert(publicKey: key, name: target.name)
            nearby.paired(key)
        }
        link.onDesktop = { [unowned self] fingerprint, host in
            try? pairing.touch(fingerprint, address: host)
        }
        link.onEnded = { [unowned self] end, target in ended(end, target) }
        link.onRetry = { [unowned self] in lookAgain() }
        link.freshen = { [unowned self] target in freshened(target) }
    }

    // MARK: What the window shows

    var pis: [NearbyPi] { nearby.pis }
    var phase: Phase { link.phase }
    var desktopName: String { link.desktopName }
    var desktopSize: CGSize { link.desktopSize }
    var frame: CGImage? { link.frame }
    var cursorShown: Bool { link.cursorShown }
    var framesPerSecond: Int { link.framesPerSecond }
    var bytesPerSecond: Int { link.bytesPerSecond }

    var fingerprint: Fingerprint { Fingerprint(publicKey: identity.publicKey) }
    var selectedPi: NearbyPi? { pis.first { $0.id == selectionID } }
    var selectedPaired: PairedPi? { selectedPi.flatMap { pairing.find($0.fingerprint) } }
    var isSessionActive: Bool { link.isActive }
    var isConnected: Bool { link.isConnected }
    var canPairSelected: Bool { selectedPi?.isPairable == true && selectedPaired == nil && !isSessionActive }

    /// Pis whose pairing window is open and that do not know this Mac.
    var readyToPair: [NearbyPi] { nearby.readyToPair }
    /// Paired Pis that answered.
    var pairedNearby: [NearbyPi] { nearby.pairedNearby }

    var pairedOutOfView: [PairedPi] {
        pairing.pis.filter { paired in !pis.contains { $0.isPaired && $0.fingerprint == paired.fingerprint } }
    }

    func isCurrent(_ pi: NearbyPi) -> Bool {
        isSessionActive && link.target?.fingerprint == pi.fingerprint
    }

    /// A paired Pi that this session is for, whether or not it answered a call (a Pi reached by address).
    func isCurrent(_ paired: PairedPi) -> Bool {
        isSessionActive && link.target?.piKey == paired.publicKey
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
            connect(SessionTarget(name: host, endpoint: endpoint, host: host, piKey: nil, fingerprint: nil))
        } else if let recent = pairing.pis.max(by: { ($0.lastConnected ?? $0.paired) < ($1.lastConnected ?? $1.paired) }) {
            connect(SessionTarget(name: host, endpoint: endpoint, host: host, piKey: recent.publicKey, fingerprint: recent.fingerprint))
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
        if !isConnected { callNow() }
    }

    private func keepCalling() async {
        while !Task.isCancelled {
            let wait: Duration = Date() < callOftenUntil ? .seconds(5) : .seconds(15)
            try? await Task.sleep(for: wait)
            if !isConnected { callNow() }
        }
    }

    private func callNow() {
        pairing.reloadIfChanged()
        let current = isSessionActive ? link.target?.fingerprint : nil
        nearby.prune { $0.fingerprint == current }
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
        guard let result = nearby.hear(heard, pairing: pairing) else { return }
        if let address = result.address { try? pairing.remember(address: address, for: result.pi.fingerprint) }
        if selectedPi == nil { selectionID = result.pi.id }
        autoConnect()
    }

    private func autoConnect() {
        guard phase == .idle, !link.stayDisconnected, sheet == nil else { return }
        let inView = Set(pairedNearby.map(\.fingerprint))
        guard let paired = pairing.mostRecent(among: inView), let pi = pairedNearby.first(where: { $0.fingerprint == paired.fingerprint }) else { return }
        selectionID = pi.id
        connect(SessionTarget(pi: pi, key: paired.publicKey))
    }

    /// The target at the address its Pi answered from most recently.
    private func freshened(_ target: SessionTarget) -> SessionTarget {
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
            connect(SessionTarget(pi: pi, key: paired.publicKey))
        } else if pi.isPairable {
            sheet = .pair(pi)
        }
    }

    func pairSelected() {
        if canPairSelected, let pi = selectedPi { sheet = .pair(pi) }
    }

    func confirmPair(_ pi: NearbyPi) {
        sheet = nil
        connect(SessionTarget(pi: pi, key: nil))
    }

    func connectManually(host: String, port: UInt16, pair: Bool) {
        sheet = nil
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port) ?? NWEndpoint.Port(rawValue: MaryVNC.port)!)
        if pair {
            connect(SessionTarget(name: host, endpoint: endpoint, host: host, piKey: nil, fingerprint: nil))
        } else if let recent = pairing.pis.max(by: { ($0.lastConnected ?? $0.paired) < ($1.lastConnected ?? $1.paired) }) {
            connect(SessionTarget(name: host, endpoint: endpoint, host: host, piKey: recent.publicKey, fingerprint: recent.fingerprint))
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
        if link.target?.piKey == paired.publicKey { disconnect() }
        try? pairing.forget(paired.fingerprint)
        nearby.remove(paired.fingerprint)
    }

    func disconnect() {
        link.disconnect()
    }

    /// Connected: the whole screen again. Otherwise: call for Pis again.
    func refresh() {
        if isConnected { link.refresh() } else { lookAgain() }
    }

    func send(_ message: WireMessage) {
        link.send(message)
    }

    func setQuality(_ quality: FrameQuality) {
        settings.quality = quality
        try? settings.save(to: settingsURL)
        link.setQuality(quality)
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

    private func connect(_ target: SessionTarget) {
        link.connect(target, identity: identity)
    }

    private func ended(_ end: PiLink.End, _ target: SessionTarget?) {
        switch end {
        case .closed, .stopped, .gaveUp:
            break
        case let .settled(title, text), let .failed(title, text):
            sheet = .message(title: title, text: text)
        case let .forgotten(title, text):
            // The Pi said so after proving its key: drop the pairing here too.
            if let key = target?.piKey {
                try? pairing.forget(Fingerprint(publicKey: key))
                nearby.remove(Fingerprint(publicKey: key))
            }
            sheet = .message(title: title, text: text)
        }
    }
}
