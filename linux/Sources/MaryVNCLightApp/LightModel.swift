import AppKit
import CoreGraphics
import MaryVNCKit
import MaryVNCViewer
import Network
import Observation
import os

/// MaryVNC Light's state: the Pis in view, the one session, the portal and the dialog. It calls for Pis every second
/// or so and does what MaryVNCKit's `PortalRules` says: open the portal when a paired Pi's button is pressed, when a
/// paired Pi arrives, or when a lost one comes back, and ask to pair when a Pi is ready to pair.
@MainActor @Observable
final class LightModel {
    enum Dialog: Equatable {
        case pair
        case message(title: String, text: String)
        case forget(PairedPi)
    }

    private(set) var nearby = NearbyList()
    private(set) var pairing: PairingStore
    private(set) var settings: ViewerSettings
    private(set) var identity: NoiseKeyPair?
    /// Something that keeps MaryVNC Light from working, for the menu's first line.
    private(set) var problem: String?
    /// With more than one Pi ready to pair, the one chosen in the dialog.
    var pairChoice: Fingerprint?
    var dialog: Dialog? {
        didSet {
            if dialog == nil { panel.close() } else if dialog != oldValue { panel.show(model: self) }
        }
    }
    let link = PiLink()

    @ObservationIgnored private let portal = PortalController()
    @ObservationIgnored private let panel = PanelController()
    @ObservationIgnored private let arguments: [String]
    /// `--test-profile DIR`: this Mac's key, the pairs and the settings in DIR rather than the Keychain and
    /// Application Support, and no calls unless `--nearby` is given too.
    @ObservationIgnored private let testProfile: URL?
    @ObservationIgnored private let settingsURL: URL
    @ObservationIgnored private var scanner: NearbyScanner?
    @ObservationIgnored private let pathMonitor = NWPathMonitor()
    @ObservationIgnored private var rules = PortalRules(now: Date())
    @ObservationIgnored private var lastCall = 0
    @ObservationIgnored private var passes = 0
    /// When a paired Pi last answered: while one has in the last 30 s, a call goes out every second.
    @ObservationIgnored private var lastPairedAnswer = Date.distantPast
    @ObservationIgnored private var networkProblem = false

    static let logger = Logger(subsystem: "com.maryos.MaryVNC", category: "light")

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
            problem = "The paired Pis could not be read: \(error)"
        }
    }

    // MARK: What the menu shows

    var fingerprint: Fingerprint? { identity.map { Fingerprint(publicKey: $0.publicKey) } }

    /// Pis ready to pair that were not put off with Not Now.
    var offers: [NearbyPi] {
        nearby.readyToPair.filter { !rules.isDismissed($0.fingerprint) }
    }

    func isShowing(_ pi: NearbyPi) -> Bool {
        link.isActive && link.target?.fingerprint == pi.fingerprint
    }

    var statusLine: String {
        if let problem { return problem }
        switch link.phase {
        case .idle:
            if let ready = nearby.readyToPair.first { return "“\(ready.name)” is ready to pair" }
            if let pi = nearby.pairedNearby.first { return "\(pi.name) is nearby: press its power button" }
            return pairing.pis.isEmpty ? "Press a MaryOS Pi’s power button to pair" : "Looking for your Pi"
        case let .connecting(name):
            return "Opening \(name)…"
        case let .pairing(name):
            return "Pairing with \(name)…"
        case .connected:
            return "\(link.desktopName) · \(Int(link.desktopSize.width))×\(Int(link.desktopSize.height)) · \(link.framesPerSecond) fps"
        case let .waiting(name, _):
            return "Lost \(name); looking for it"
        }
    }

    var iconName: String {
        if link.isConnected { return "rectangle.inset.filled" }
        if link.isActive { return "arrow.triangle.2.circlepath" }
        if !nearby.readyToPair.isEmpty { return "link.badge.plus" }
        if !nearby.pairedNearby.isEmpty { return "display" }
        return "dot.radiowaves.left.and.right"
    }

    // MARK: Starting

    func start() {
        loadIdentity()
        link.quality = settings.quality
        link.retryLimit = .seconds(20)
        link.onPaired = { [unowned self] key, target in
            try? pairing.upsert(publicKey: key, name: target.name)
            nearby.paired(key)
            Self.logger.notice("paired with \(target.name, privacy: .public)")
        }
        link.onDesktop = { [unowned self] fingerprint, host in
            try? pairing.touch(fingerprint, address: host)
            portal.show(model: self)
        }
        link.onEnded = { [unowned self] end, target in ended(end, target) }
        link.freshen = { [unowned self] target in freshened(target) }
        portal.onPutAway = { [unowned self] in closePortal() }
        guard testProfile == nil || arguments.contains("--nearby") else { return }
        let scanner = NearbyScanner()
        scanner.sendFailed = { [weak self] error, destination in
            Task { @MainActor in self?.sendFailed(error, destination) }
        }
        do {
            try scanner.start()
        } catch {
            problem = "MaryVNC Light cannot call for Pis: \(error)"
            return
        }
        self.scanner = scanner
        watchTheNetwork()
        Task { [weak self] in await self?.keepCalling() }
        Task { [weak self] in
            for await heard in scanner.heard { self?.hear(heard) }
        }
    }

    private func loadIdentity() {
        do {
            if let testProfile {
                identity = try Identity.loadOrCreate(from: FileIdentityStore(url: testProfile.appending(path: "identity.key")))
            } else {
                identity = try Identity.loadOrCreate(from: KeychainIdentityStore())
            }
        } catch {
            identity = NoiseKeyPair.generate()
            problem = "The Keychain would not give this Mac’s key (\(error)); a Pi paired now will not know this Mac next time"
        }
    }

    private func watchTheNetwork() {
        pathMonitor.pathUpdateHandler = { [weak self] path in
            guard path.status == .satisfied else { return }
            Task { @MainActor in self?.rules.startGrace(now: Date()) }
        }
        pathMonitor.start(queue: DispatchQueue(label: "com.maryos.MaryVNCLight.path"))
        let center = NSWorkspace.shared.notificationCenter
        center.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.rules.startGrace(now: Date()) }
        }
        center.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.rules.sleeping() }
        }
    }

    // MARK: Calling

    private func keepCalling() async {
        while !Task.isCancelled {
            let quick = Date().timeIntervalSince(lastPairedAnswer) < 30
            let cadence: TimeInterval = quick ? 1 : 1.5
            try? await Task.sleep(for: .milliseconds(Int(cadence * 1000)))
            pass(cadence: cadence, quick: quick)
        }
    }

    /// One pass: a call (none while the portal is showing), then the rules' clock.
    private func pass(cadence: TimeInterval, quick: Bool) {
        let now = Date()
        pairing.reloadIfChanged()
        passes += 1
        var called = false, toGroups = false
        if !link.isConnected, let scanner, let identity {
            let recent = pairing.pis.sorted { ($0.lastConnected ?? $0.paired) > ($1.lastConnected ?? $1.paired) }.prefix(Nearby.tagsMax)
            let keys = recent.compactMap { try? NearbyKey(identity: identity, piPublicKey: $0.publicKey) }
            let addresses = recent.compactMap(\.lastAddress)
            // Unicast and group calls never share a pass, so a Pi answers each of this Mac's addresses at most once
            // a pass, well inside its two answers a second.
            toGroups = addresses.isEmpty || passes % (quick ? 3 : 2) == 0
            lastCall = scanner.call(keys: keys, addresses: toGroups ? [] : addresses, multicast: toGroups)
            called = true
        }
        let current = link.isActive ? link.target?.fingerprint : nil
        nearby.prune(now: now) { $0.fingerprint == current }
        let pairs = Set(pairing.pis.map(\.fingerprint))
        let gone = rules.tick(now: now, pairs: pairs, cadence: cadence, called: called && !link.isActive, toGroups: toGroups)
        if !gone.isEmpty, dialog == .pair {
            if offers.isEmpty { dialog = nil } else { panel.fit() }
        }
    }

    /// Calls to the groups now.
    func lookAgain() {
        guard let scanner, let identity, !link.isConnected else { return }
        let keys = pairing.pis.prefix(Nearby.tagsMax).compactMap { try? NearbyKey(identity: identity, piPublicKey: $0.publicKey) }
        lastCall = scanner.call(keys: keys, addresses: [], multicast: true)
    }

    private func sendFailed(_ error: Int32, _ destination: String) {
        Self.logger.info("a call to \(destination, privacy: .public) did not go: \(String(cString: strerror(error)), privacy: .public)")
        // A call to one address fails when nothing is there any more (a Pi's old address), which says nothing about
        // who is away. A call to the groups that cannot go is the network itself, or macOS keeping MaryVNC Light off
        // it: what went unheard meanwhile does not count.
        guard destination.contains(" on ") else { return }
        rules.startGrace(now: Date())
        if error == EHOSTUNREACH, problem == nil || networkProblem {
            networkProblem = true
            problem = "Allow MaryVNC Light in System Settings › Privacy & Security › Local Network"
        }
    }

    private func hear(_ heard: NearbyHeard) {
        let now = Date()
        if networkProblem {
            networkProblem = false
            problem = nil
        }
        guard let result = nearby.hear(heard, pairing: pairing, now: now) else { return }
        let pi = result.pi
        guard pi.isPaired else {
            if rules.heardOffer(pi.fingerprint, call: heard.call, now: now) { offer() }
            return
        }
        lastPairedAnswer = now
        if let address = result.address { try? pairing.remember(address: address, for: pi.fingerprint) }
        guard let summon = rules.heard(pi.fingerprint, pairable: pi.isPairable, call: heard.call, sessionUp: link.isActive, now: now) else { return }
        if Self.screenIsLocked {
            Self.logger.info("\(pi.name, privacy: .public) asked for its portal while the screen is locked")
            return
        }
        switch summon {
        case .pressed: Self.logger.notice("\(pi.name, privacy: .public): its power button was pressed")
        case .arrived: Self.logger.notice("\(pi.name, privacy: .public) arrived")
        case .returned: Self.logger.notice("\(pi.name, privacy: .public) is back")
        }
        open(pi)
    }

    /// A Pi ready to pair: ask, unless something else is under way.
    private func offer() {
        guard !link.isActive, !offers.isEmpty else { return }
        switch dialog {
        case nil:
            Self.logger.notice("asking to pair with \(self.offers.map(\.name).joined(separator: ", "), privacy: .public)")
            dialog = .pair
        case .pair:
            panel.fit()
        default:
            break
        }
    }

    private static var screenIsLocked: Bool {
        guard let session = CGSessionCopyCurrentDictionary() as? [String: Any] else { return false }
        return session["CGSSessionScreenIsLocked"] as? Bool == true
    }

    /// The target at the address its Pi answered from most recently.
    private func freshened(_ target: SessionTarget) -> SessionTarget {
        guard let fingerprint = target.fingerprint, let pi = nearby.pairedNearby.first(where: { $0.fingerprint == fingerprint }) else { return target }
        var fresh = target
        fresh.endpoint = pi.endpoint
        fresh.host = pi.host
        return fresh
    }

    // MARK: Commands

    /// The portal on a paired Pi in view.
    func open(_ pi: NearbyPi) {
        guard let identity, let paired = pairing.find(pi.fingerprint) else { return }
        if isShowing(pi) {
            if link.isConnected { portal.show(model: self) }
            return
        }
        dialog = nil
        link.connect(SessionTarget(pi: pi, key: paired.publicKey), identity: identity)
    }

    func askToPair(_ pi: NearbyPi) {
        pairChoice = pi.fingerprint
        dialog = .pair
    }

    func pair(_ pi: NearbyPi) {
        guard let identity else { return }
        dialog = nil
        link.connect(SessionTarget(pi: pi, key: nil), identity: identity)
    }

    /// Not Now: the Pis asking stay quiet until their windows close.
    func notNow() {
        for pi in offers { rules.dismiss(pi.fingerprint) }
        pairChoice = nil
        dialog = nil
    }

    func dismissDialog() {
        dialog = nil
    }

    /// ⌘Q, ⌘H or ⌘M in the portal, or Close Portal: the session ends, and MaryVNC Light stays in the menu bar.
    func closePortal() {
        let fingerprint = link.target?.fingerprint
        link.disconnect()
        portal.close()
        rules.sessionEnded(fingerprint, lost: false, lastCall: lastCall)
    }

    func askToForget(_ paired: PairedPi) {
        dialog = .forget(paired)
    }

    func forget(_ paired: PairedPi) {
        dialog = nil
        if link.target?.piKey == paired.publicKey { closePortal() }
        try? pairing.forget(paired.fingerprint)
        nearby.remove(paired.fingerprint)
    }

    func setQuality(_ quality: FrameQuality) {
        settings.quality = quality
        try? settings.save(to: settingsURL)
        link.setQuality(quality)
    }

    func setCommandKey(_ key: CommandKey) {
        settings.commandKey = key
        try? settings.save(to: settingsURL)
    }

    private func ended(_ end: PiLink.End, _ target: SessionTarget?) {
        let fingerprint = target?.fingerprint
        portal.close()
        switch end {
        case .closed, .stopped:
            rules.sessionEnded(fingerprint, lost: false, lastCall: lastCall)
        case let .settled(title, text), let .failed(title, text):
            rules.sessionEnded(fingerprint, lost: false, lastCall: lastCall)
            dialog = .message(title: title, text: text)
        case let .forgotten(title, text):
            if let fingerprint {
                try? pairing.forget(fingerprint)
                nearby.remove(fingerprint)
            }
            rules.sessionEnded(fingerprint, lost: false, lastCall: lastCall)
            dialog = .message(title: title, text: text)
        case let .gaveUp(hadDesktop):
            Self.logger.notice("gave up on \(target?.name ?? "the Pi", privacy: .public); its portal comes back when it answers")
            rules.sessionEnded(fingerprint, lost: hadDesktop, lastCall: lastCall)
        }
    }
}
