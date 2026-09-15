import Foundation

/// When MaryVNC Light opens its portal without being asked, kept apart from clocks and sockets so it can be tested.
/// The app hands it every pass of its calling loop and every answer, and does what comes back.
///
/// - **Pressed.** A paired Pi's answer carried this Mac's tag and an open pairing window: its power button was just
///   pressed. That counts only once the Pi has been heard with its window closed, since launch or since the last
///   session ended, so a window opened on purpose while the portal was up (to pair another Mac) never reopens it.
/// - **Arrived.** A paired Pi answered after `away` of watching without a word from it: it has just started, most
///   likely from the press that booted it. Watching means a call went out and the grace after launch, wake or a
///   network change had passed (answers from Pis that were there all along come in during it).
/// - **Returned.** A paired Pi whose portal was lost, not closed, answered again.
///
/// Answers to calls older than the newest one heard from that Pi, or from before the last session ended, are
/// copies or stragglers, and count for nothing.
public struct PortalRules: Sendable {
    public enum Summon: Equatable, Sendable {
        case pressed(Fingerprint)
        case arrived(Fingerprint)
        case returned(Fingerprint)
    }

    public static let grace: TimeInterval = 15
    public static let away: TimeInterval = 30
    /// An offer is gone after this long without it, across at least `offerGoneCalls` calls to the groups.
    public static let offerGone: TimeInterval = 10
    public static let offerGoneCalls = 3

    private struct Paired: Sendable {
        var armed = false
        var lost = false
        var newestCall = 0
        var watchedSinceHeard: TimeInterval = 0
    }

    private struct Offer: Sendable {
        var lastHeard: Date
        var groupCalls = 0
        var dismissed = false
    }

    private var paired: [Fingerprint: Paired] = [:]
    private var offers: [Fingerprint: Offer] = [:]
    private var graceUntil: Date
    private var lastTick: Date?
    private var callFloor = 0

    public init(now: Date) {
        graceUntil = now.addingTimeInterval(Self.grace)
    }

    /// Launch, wake, a network change, a call that could not go out.
    public mutating func startGrace(now: Date) {
        graceUntil = max(graceUntil, now.addingTimeInterval(Self.grace))
    }

    public func inGrace(now: Date) -> Bool {
        now < graceUntil
    }

    /// One pass of the calling loop. `pairs`: the Pis this Mac is paired with. `called`: a call went out (none goes
    /// while a session is up), `toGroups` to the groups. `cadence`: the loop's interval, to notice a Mac that slept.
    /// Returns the offers that have gone quiet, for a dialog showing one to close.
    public mutating func tick(now: Date, pairs: Set<Fingerprint>, cadence: TimeInterval, called: Bool, toGroups: Bool) -> [Fingerprint] {
        for key in paired.keys where !pairs.contains(key) { paired[key] = nil }
        for key in pairs where paired[key] == nil { paired[key] = Paired() }
        defer { lastTick = now }
        guard let last = lastTick else { return [] }
        let gap = now.timeIntervalSince(last)
        guard gap >= 0, gap <= cadence * 3 else {
            // The Mac slept, or the loop was held up: what went unheard meanwhile says nothing about who is away.
            startGrace(now: now)
            return []
        }
        if called && !inGrace(now: now) {
            for key in paired.keys { paired[key]!.watchedSinceHeard += gap }
        }
        guard toGroups else { return [] }
        var gone: [Fingerprint] = []
        for (key, var offer) in offers {
            offer.groupCalls += 1
            if offer.groupCalls >= Self.offerGoneCalls && now.timeIntervalSince(offer.lastHeard) >= Self.offerGone {
                offers[key] = nil
                gone.append(key)
            } else {
                offers[key] = offer
            }
        }
        return gone
    }

    /// The Mac is going to sleep: nothing unheard so far counts.
    public mutating func sleeping() {
        for key in paired.keys { paired[key]!.watchedSinceHeard = 0 }
    }

    /// An answer from a paired Pi carrying this Mac's tag, to call number `call`. `pairable`: its window is open.
    /// `sessionUp`: a session (or an attempt to bring one back) is under way.
    public mutating func heard(_ pi: Fingerprint, pairable: Bool, call: Int, sessionUp: Bool, now: Date) -> Summon? {
        var state = paired[pi] ?? Paired()
        defer { paired[pi] = state }
        guard call >= callFloor, call >= state.newestCall else { return nil }
        state.newestCall = call
        let watched = state.watchedSinceHeard
        state.watchedSinceHeard = 0
        guard !sessionUp else { return nil }
        if state.lost {
            state.lost = false
            return .returned(pi)
        }
        if pairable {
            guard state.armed else { return nil }
            state.armed = false
            return .pressed(pi)
        }
        state.armed = true
        return watched >= Self.away && !inGrace(now: now) ? .arrived(pi) : nil
    }

    /// A session ended. `lost`: the link went and did not come back, after the desktop had been showing.
    /// `lastCall`: the number of the newest call so far.
    public mutating func sessionEnded(_ pi: Fingerprint?, lost: Bool, lastCall: Int) {
        callFloor = lastCall + 1
        guard let pi else { return }
        var state = paired[pi] ?? Paired()
        state.armed = false
        state.lost = lost
        state.watchedSinceHeard = 0
        paired[pi] = state
    }

    /// An offer from a Pi this Mac is not paired with, to call number `call`; whether to show it.
    public mutating func heardOffer(_ pi: Fingerprint, call: Int, now: Date) -> Bool {
        guard call >= callFloor else { return false }
        var offer = offers[pi] ?? Offer(lastHeard: now)
        offer.lastHeard = now
        offer.groupCalls = 0
        offers[pi] = offer
        return !offer.dismissed
    }

    /// Not Now: the offer is not shown again until it has gone quiet.
    public mutating func dismiss(_ pi: Fingerprint) {
        offers[pi]?.dismissed = true
    }

    public func isDismissed(_ pi: Fingerprint) -> Bool {
        offers[pi]?.dismissed == true
    }
}
