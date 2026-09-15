import CoreGraphics
import Foundation
import MaryVNCKit
import Observation

/// One session with a Pi, and what it shows: the handshake, the frames, and trying again when the link goes. The
/// rules MaryVNC and MaryVNC Light share; what a pairing, an address or an ending means to a viewer reaches it
/// through the callbacks.
@MainActor @Observable
public final class PiLink {
    public enum Phase: Equatable {
        case idle
        case connecting(name: String)
        case pairing(name: String)
        case connected
        case waiting(name: String, seconds: Int)
    }

    /// How a session ended, once nothing will try it again.
    public enum End: Equatable {
        /// This viewer closed it.
        case closed
        /// It stays disconnected, and says why.
        case settled(title: String, text: String)
        /// The Pi said it has forgotten this Mac: forget the Pi (the target's key) too, and say so.
        case forgotten(title: String, text: String)
        /// A session that is not tried again (a pairing, say) failed, and says why.
        case failed(title: String, text: String)
        /// A session that is not tried again ended with nothing to say.
        case stopped
        /// The link went and did not come back within `retryLimit`; `hadDesktop`: the desktop had been showing.
        case gaveUp(hadDesktop: Bool)
    }

    public private(set) var phase: Phase = .idle
    public private(set) var target: SessionTarget?
    public private(set) var desktopName = ""
    public private(set) var desktopSize = CGSize.zero
    public private(set) var frame: CGImage?
    public private(set) var cursorShown = false
    public private(set) var framesPerSecond = 0
    public private(set) var bytesPerSecond = 0
    /// Set by `disconnect` and by an ending that settles: nothing reconnects until the next `connect`.
    public private(set) var stayDisconnected = false

    /// Sent to the Pi when its desktop arrives.
    public var quality: FrameQuality = .best
    /// How long a lost link keeps trying before it gives up; nil keeps trying.
    public var retryLimit: Duration?

    /// XX completed: the Pi's key, and the target now carrying it.
    public var onPaired: @MainActor (_ piPublicKey: [UInt8], _ target: SessionTarget) -> Void = { _, _ in }
    /// The desktop arrived: the Pi's fingerprint and the address it was reached at.
    public var onDesktop: @MainActor (_ fingerprint: Fingerprint, _ host: String?) -> Void = { _, _ in }
    /// The session ended for good.
    public var onEnded: @MainActor (_ end: End, _ target: SessionTarget?) -> Void = { _, _ in }
    /// The link went and will be tried again: a moment to call for Pis again.
    public var onRetry: @MainActor () -> Void = {}
    /// Just before each new attempt: the target at the address its Pi answered from most recently.
    public var freshen: @MainActor (_ target: SessionTarget) -> SessionTarget = { $0 }

    @ObservationIgnored private var identity: NoiseKeyPair?
    @ObservationIgnored private var session: PiSession?
    @ObservationIgnored private let decoder = FrameDecoder()
    @ObservationIgnored private var backoff = Backoff()
    @ObservationIgnored private var reconnect: Task<Void, Never>?
    @ObservationIgnored private var lostAt: ContinuousClock.Instant?
    @ObservationIgnored private var hadDesktop = false
    @ObservationIgnored private var statFrames = 0
    @ObservationIgnored private var statBytes = 0
    @ObservationIgnored private var statStart = Date()

    public init() {}

    public var isActive: Bool { phase != .idle }
    public var isConnected: Bool { phase == .connected }

    public func connect(_ target: SessionTarget, identity: NoiseKeyPair) {
        lostAt = nil
        hadDesktop = false
        start(target, identity: identity)
    }

    public func disconnect() {
        stayDisconnected = true
        reconnect?.cancel()
        reconnect = nil
        session?.close()
        session = nil
        phase = .idle
        frame = nil
    }

    public func send(_ message: WireMessage) {
        guard isConnected else { return }
        session?.send(message)
    }

    /// The whole screen again.
    public func refresh() {
        session?.send(.refresh)
    }

    public func setQuality(_ quality: FrameQuality) {
        self.quality = quality
        session?.send(.quality(quality))
    }

    private func start(_ target: SessionTarget, identity: NoiseKeyPair) {
        reconnect?.cancel()
        reconnect = nil
        session?.close()
        stayDisconnected = false
        self.target = target
        self.identity = identity
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
            onEnded(.failed(title: "MaryVNC could not start a session", text: "\(error)"), target)
        }
    }

    private func run(_ session: PiSession) async {
        for await event in session.events {
            guard session === self.session else { continue }
            switch event {
            case .connecting, .handshaking:
                break
            case let .paired(piPublicKey):
                if var target {
                    target.piKey = piPublicKey
                    target.fingerprint = Fingerprint(publicKey: piPublicKey)
                    self.target = target
                    onPaired(piPublicKey, target)
                }
            case let .desktop(name, width, height, fingerprint):
                desktopName = name
                desktopSize = CGSize(width: width, height: height)
                await decoder.resize(width: width, height: height)
                phase = .connected
                backoff.reset()
                lostAt = nil
                hadDesktop = true
                onDesktop(fingerprint, target?.host)
                session.send(.quality(quality))
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
        switch reason.outcome(name: target?.name ?? "The Pi") {
        case .closed:
            phase = .idle
            frame = nil
            onEnded(.closed, target)
        case let .settle(title, text):
            settle()
            onEnded(.settled(title: title, text: text), target)
        case let .forget(title, text):
            settle()
            onEnded(.forgotten(title: title, text: text), target)
        case .retry:
            retry(reason)
        }
    }

    private func settle() {
        stayDisconnected = true
        phase = .idle
        frame = nil
    }

    private func retry(_ reason: SessionEnd) {
        guard let target, target.piKey != nil, !stayDisconnected, let identity else {
            phase = .idle
            frame = nil
            if case let .failed(text) = reason {
                onEnded(.failed(title: "The session ended", text: text), target)
            } else {
                onEnded(.stopped, target)
            }
            return
        }
        let now = ContinuousClock.now
        let since = lostAt ?? now
        lostAt = since
        if let retryLimit, now - since >= retryLimit {
            phase = .idle
            frame = nil
            onEnded(.gaveUp(hadDesktop: hadDesktop), target)
            return
        }
        let delay = backoff.next()
        let seconds = max(1, Int((Double(delay.components.seconds) + Double(delay.components.attoseconds) / 1e18).rounded(.up)))
        phase = .waiting(name: target.name, seconds: seconds)
        onRetry()
        reconnect = Task { [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled, let self, !self.stayDisconnected else { return }
            self.start(self.freshen(target), identity: identity)
        }
    }
}
