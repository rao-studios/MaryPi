import CryptoKit
import Foundation
import Network

/// How a session starts.
public enum SessionMode: Sendable, Equatable {
    /// Noise XX: pair with a Pi. `expected` is the fingerprint its Nearby answer offered; a Pi that
    /// proves another key is left before this Mac's key is sent.
    case pair(displayName: String, expected: Fingerprint?)
    /// Noise IK: resume with a Pi paired before.
    case resume(piPublicKey: [UInt8], displayName: String)
}

/// Why a session ended.
public enum SessionEnd: Sendable, Equatable {
    case byViewer
    /// The Pi's `bye`: `replaced`, `forgotten`, `shutdown`, `protocol` or `timeout`.
    case bye(String)
    /// The Pi closed an XX handshake without a word: not over the cable, and no pairing window open.
    case pairingRefused
    /// The Pi closed an IK handshake without a word: it does not know this Mac (any more).
    case notPaired
    /// The Pi proved a key other than the one expected.
    case wrongPi
    case failed(String)
}

public enum SessionEvent: Sendable, Equatable {
    case connecting
    case handshaking
    /// XX completed: the Pi's key, for the pairing store.
    case paired(piPublicKey: [UInt8])
    /// `hello`: at the start, and again whenever the Pi's desktop changes size.
    case desktop(name: String, width: Int, height: Int, fingerprint: Fingerprint)
    /// Apply the rectangles, then `ack(seq:)`: the Pi sends at most two frames ahead of the acks.
    case frame(seq: UInt32, rects: [FrameRect])
    case cursor(x: Int, y: Int, shown: Bool)
    /// The Pi's clipboard changed.
    case clipboard(String)
    case ended(SessionEnd)
}

/// One connection to a Pi's maryvncd: the handshake, then sealed messages both ways. Everything it holds is
/// touched on its own serial queue, which is also the connection's, so bytes and events keep their order.
public final class PiSession: @unchecked Sendable {
    public let events: AsyncStream<SessionEvent>
    public let mode: SessionMode

    private let queue = DispatchQueue(label: "com.maryos.MaryVNC.session")
    private let continuation: AsyncStream<SessionEvent>.Continuation
    private let connection: NWConnection
    private var handshake: NoiseHandshake
    private var reader = RecordReader(maximum: MaryVNC.handshakeMax)
    private var sendCipher: NoiseCipher?
    private var receiveCipher: NoiseCipher?
    private var finished = false

    /// `interface` pins the connection to one interface.
    public init(endpoint: NWEndpoint, identity: NoiseKeyPair, mode: SessionMode, interface: NWInterface? = nil) throws {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        tcp.enableKeepalive = true
        // A Pi switched off or out of range sends nothing more, and a viewer waiting for frames sends nothing
        // either: probes every 5 s after 15 s of quiet, and a drop when 3 go unanswered or when data sent is not
        // acknowledged within 20 s, find a vanished Pi within half a minute rather than ten.
        tcp.keepaliveIdle = 15
        tcp.keepaliveInterval = 5
        tcp.keepaliveCount = 3
        tcp.connectionDropTime = 20
        tcp.connectionTimeout = 8
        let parameters = NWParameters(tls: nil, tcp: tcp)
        if let interface { parameters.requiredInterface = interface }
        connection = NWConnection(to: endpoint, using: parameters)
        switch mode {
        case .pair:
            handshake = try NoiseHandshake(pattern: .xx, initiator: true, staticKey: identity)
        case let .resume(piPublicKey, _):
            handshake = try NoiseHandshake(pattern: .ik, initiator: true, staticKey: identity, remoteStatic: piPublicKey)
        }
        self.mode = mode
        (events, continuation) = AsyncStream.makeStream(of: SessionEvent.self)
        continuation.onTermination = { [weak self] _ in self?.close() }
    }

    public func start() {
        queue.async { [self] in
            guard !finished else { return }
            connection.stateUpdateHandler = { [weak self] state in self?.stateChanged(state) }
            continuation.yield(.connecting)
            connection.start(queue: queue)
        }
    }

    /// One message to the Pi; ignored before the handshake completes or after the end.
    public func send(_ message: WireMessage) {
        queue.async { [self] in
            do {
                try seal(message)
            } catch {
                end(.failed("\(error)"))
            }
        }
    }

    /// Says `bye` when there is a session, and closes.
    public func close() {
        queue.async { [self] in
            guard !finished else { return }
            try? seal(.viewerBye)
            end(.byViewer)
        }
    }

    // MARK: On the queue

    private var displayName: String {
        switch mode {
        case let .pair(name, _), let .resume(_, name): Identity.wireName(name)
        }
    }

    private func stateChanged(_ state: NWConnection.State) {
        switch state {
        case .ready:
            beginHandshake()
        case let .waiting(error), let .failed(error):
            end(.failed("cannot reach the Pi: \(error.localizedDescription)"))
        default:
            break
        }
    }

    private func beginHandshake() {
        continuation.yield(.handshaking)
        do {
            // IK's first message is the viewer's last, so it carries the name; XX's first carries none.
            let name = handshake.pattern == .ik ? displayName : ""
            let message = try handshake.writeMessage(payload: HandshakePayload(name: name).encoded())
            write(try Record.first(pattern: handshake.pattern, noise: message))
            receive()
        } catch {
            end(.failed("\(error)"))
        }
    }

    private func receive() {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1 << 16) { [weak self] data, _, isComplete, error in
            guard let self, !finished else { return }
            if let data, !data.isEmpty {
                reader.append(data)
                process()
            }
            guard !finished else { return }
            if isComplete || error != nil {
                closedByPi(error)
                return
            }
            receive()
        }
    }

    private func closedByPi(_ error: NWError?) {
        if handshake.isComplete {
            end(.failed(error.map { "the connection dropped: \($0.localizedDescription)" } ?? "the Pi closed the connection"))
        } else {
            end(handshake.pattern == .xx ? .pairingRefused : .notPaired)
        }
    }

    private func process() {
        do {
            while !finished, let body = try reader.next() {
                if handshake.isComplete {
                    try sessionRecord(body)
                } else {
                    try handshakeRecord(body)
                }
            }
        } catch {
            end(.failed("\(error)"))
        }
    }

    private func handshakeRecord(_ body: [UInt8]) throws {
        let payloadBytes: [UInt8]
        do {
            payloadBytes = try handshake.readMessage(body)
        } catch {
            // Not the Pi this Mac paired with (IK), or not a Pi at all.
            end(.wrongPi)
            return
        }
        let payload = try HandshakePayload.decode(payloadBytes)
        guard payload.version == MaryVNC.version else { throw MaryVNCError("the Pi speaks MaryVNC \(payload.version); this viewer speaks \(MaryVNC.version)") }
        if handshake.pattern == .xx {
            guard let piKey = handshake.remoteStatic else { throw MaryVNCError("the Pi sent no key") }
            if case let .pair(_, expected) = mode, let expected, Fingerprint(publicKey: piKey) != expected {
                end(.wrongPi)                   // before message 3: this Mac's key never leaves
                return
            }
            let third = try handshake.writeMessage(payload: HandshakePayload(name: displayName).encoded())
            write(try Record.framed(third, maximum: MaryVNC.handshakeMax))
            continuation.yield(.paired(piPublicKey: piKey))
        }
        if handshake.isComplete {
            let ciphers = try handshake.transport()
            sendCipher = ciphers.send
            receiveCipher = ciphers.receive
            reader.maximum = MaryVNC.recordMax
        }
    }

    private func sessionRecord(_ body: [UInt8]) throws {
        guard var cipher = receiveCipher else { throw MaryVNCError("a session record before the handshake") }
        let plain = try cipher.open(body)
        receiveCipher = cipher
        // A message of a type a newer Pi added: skipped, as maryvncd skips a newer viewer's.
        if let type = plain.first, !WireMessage.isKnownType(type) { return }
        switch try WireMessage.decode(plain) {
        case let .hello(version, name, width, height, fingerprint):
            guard version == MaryVNC.version else { throw MaryVNCError("the Pi speaks MaryVNC \(version)") }
            guard let piKey = handshake.remoteStatic, Array(SHA256.hash(data: piKey)) == fingerprint else {
                throw MaryVNCError("the Pi's hello names a key other than the one it proved")
            }
            continuation.yield(.desktop(name: name, width: Int(width), height: Int(height), fingerprint: Fingerprint(publicKey: piKey)))
        case let .frame(seq, rects):
            continuation.yield(.frame(seq: seq, rects: rects))
        case let .cursor(x, y, shown):
            continuation.yield(.cursor(x: Int(x), y: Int(y), shown: shown))
        case let .serverClipboard(text):
            continuation.yield(.clipboard(text))
        case let .serverBye(reason):
            end(.bye(reason))
        default:
            throw MaryVNCError("the Pi sent a viewer's message")
        }
    }

    private func seal(_ message: WireMessage) throws {
        guard !finished, var cipher = sendCipher else { return }
        let sealed = try cipher.seal(message.encoded())
        sendCipher = cipher
        write(try Record.framed(sealed))
    }

    private func write(_ bytes: [UInt8]) {
        connection.send(content: Data(bytes), completion: .contentProcessed { [weak self] error in
            if let error { self?.end(.failed("sending failed: \(error.localizedDescription)")) }
        })
    }

    private func end(_ reason: SessionEnd) {
        guard !finished else { return }
        finished = true
        continuation.yield(.ended(reason))
        continuation.finish()
        // Let a last bye leave before the connection goes.
        connection.send(content: nil, contentContext: .finalMessage, isComplete: true, completion: .contentProcessed { [connection] _ in connection.cancel() })
    }
}

/// Reconnection delays: 0.5 s doubling to 8 s, each within ±20%.
public struct Backoff: Sendable {
    public private(set) var attempt = 0
    public let base: Double
    public let cap: Double

    public init(base: Double = 0.5, cap: Double = 8) {
        self.base = base
        self.cap = cap
    }

    public mutating func next(jitter: Double = Double.random(in: 0.8...1.2)) -> Duration {
        let seconds = min(cap, base * pow(2, Double(attempt))) * jitter
        attempt += 1
        return .milliseconds(Int((seconds * 1000).rounded()))
    }

    public mutating func reset() {
        attempt = 0
    }
}
