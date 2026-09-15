import Foundation
import Network

/// The Pis in view, kept from the answers to this Mac's calls: the rules MaryVNC and MaryVNC Light share.
public struct NearbyList: Sendable {
    public private(set) var pis: [NearbyPi] = []
    /// When each listed Pi last answered from the address it is listed with.
    private var hostHeard: [String: Date] = [:]

    /// A Pi keeps the address it is listed with while that address still answers within this long, so answers from
    /// its IPv4 and link-local addresses (a call to the groups brings both) do not take turns.
    public static let addressSticks: TimeInterval = 30
    /// A Pi silent for this long leaves the list.
    public static let expires: TimeInterval = 40

    public init() {}

    /// Pis whose pairing window is open and that do not know this Mac.
    public var readyToPair: [NearbyPi] { pis.filter { !$0.isPaired } }
    /// Paired Pis that answered.
    public var pairedNearby: [NearbyPi] { pis.filter(\.isPaired) }

    public func find(_ fingerprint: Fingerprint) -> NearbyPi? {
        pis.first { $0.fingerprint == fingerprint }
    }

    /// What an answer changed.
    public struct Heard: Equatable, Sendable {
        public let pi: NearbyPi
        /// A paired Pi's address that its pairing does not have yet, for `PairingStore.remember`.
        public let address: String?
        public let isNew: Bool
    }

    /// An answer to one of this Mac's calls; nil for one that proves nothing.
    public mutating func hear(_ heard: NearbyHeard, pairing: PairingStore, now: Date = Date()) -> Heard? {
        var found: NearbyPi
        if let key = heard.piPublicKey, let paired = pairing.find(Fingerprint(publicKey: key)) {
            // The tag proves this is the Pi paired with this Mac, so where it answered from is worth keeping.
            found = NearbyPi(publicKey: key, name: heard.offer?.name ?? paired.name, host: heard.host, port: heard.port,
                             isPaired: true, isPairable: heard.offer != nil, lastSeen: now)
        } else if let offer = heard.offer, pairing.find(Fingerprint(publicKey: offer.publicKey)) == nil {
            found = NearbyPi(publicKey: offer.publicKey, name: offer.name, host: heard.host, port: heard.port,
                             isPaired: false, isPairable: true, lastSeen: now)
        } else {
            // An offer without a tag naming a Pi this Mac is paired with proves nothing about where that Pi is.
            return nil
        }
        let index = pis.firstIndex { $0.id == found.id }
        if let index, pis[index].host != found.host, let at = hostHeard[found.id], now.timeIntervalSince(at) < Self.addressSticks {
            found.host = pis[index].host
            found.port = pis[index].port
        } else {
            hostHeard[found.id] = now
        }
        let address = found.isPaired && pairing.find(found.fingerprint)?.lastAddress != found.host ? found.host : nil
        if let index {
            pis[index] = found
        } else {
            pis.append(found)
            pis.sort { ($0.isPaired ? 0 : 1, $0.name) < ($1.isPaired ? 0 : 1, $1.name) }
        }
        return Heard(pi: found, address: address, isNew: index == nil)
    }

    /// Drops the Pis silent for `expires`, but for those `keeping` holds on to (the one a session is for).
    public mutating func prune(now: Date = Date(), keeping: (NearbyPi) -> Bool = { _ in false }) {
        pis.removeAll { now.timeIntervalSince($0.lastSeen) > Self.expires && !keeping($0) }
        hostHeard = hostHeard.filter { id, _ in pis.contains { $0.id == id } }
    }

    /// XX completed with the Pi: it is paired now, and its window closed with the pairing.
    public mutating func paired(_ publicKey: [UInt8]) {
        guard let index = pis.firstIndex(where: { $0.publicKey == publicKey }) else { return }
        pis[index].isPaired = true
        pis[index].isPairable = false
    }

    public mutating func remove(_ fingerprint: Fingerprint) {
        pis.removeAll { $0.fingerprint == fingerprint }
    }
}

extension NearbyPi {
    /// Where its maryvncd listens.
    public var endpoint: NWEndpoint {
        .hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port) ?? NWEndpoint.Port(rawValue: Nearby.port)!)
    }
}
