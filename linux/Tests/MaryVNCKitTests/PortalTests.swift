import Foundation
import Testing
@testable import MaryVNCKit

@Suite struct PortalRulesTests {
    let a = Fingerprint(publicKey: NoiseKeyPair.generate().publicKey)
    let t0 = Date(timeIntervalSince1970: 2_000_000)

    /// A pass of the calling loop once a second from `from` to `to`, each a call to the groups.
    func watch(_ rules: inout PortalRules, pairs: Set<Fingerprint>, from: TimeInterval, to: TimeInterval, called: Bool = true) {
        for t in stride(from: from, through: to, by: 1) {
            _ = rules.tick(now: t0.addingTimeInterval(t), pairs: pairs, cadence: 1, called: called, toGroups: true)
        }
    }

    /// What each answer, in order, summoned: (pairable, call, seconds after t0).
    func answers(_ rules: inout PortalRules, _ pi: Fingerprint, _ heard: [(Bool, Int, TimeInterval)], sessionUp: Bool = false) -> [PortalRules.Summon?] {
        heard.map { pairable, call, t in rules.heard(pi, pairable: pairable, call: call, sessionUp: sessionUp, now: t0.addingTimeInterval(t)) }
    }

    @Test func aPressCountsOnceTheWindowHasBeenSeenClosed() {
        var rules = PortalRules(now: t0)
        watch(&rules, pairs: [a], from: 0, to: 3)
        // Open since before launch, then closed, then pressed, then still open.
        let got = answers(&rules, a, [(true, 1, 3), (false, 2, 4), (true, 3, 5), (true, 4, 6)])
        #expect(got == [nil, nil, .pressed(a), nil])
    }

    @Test func copiesAndStragglersCountForNothing() {
        var rules = PortalRules(now: t0)
        // An answer to an older call than one already heard is a straggler.
        let before = answers(&rules, a, [(false, 5, 0), (true, 4, 0), (true, 6, 0)])
        #expect(before == [nil, nil, .pressed(a)])
        rules.sessionEnded(a, lost: false, lastCall: 6)
        // From before the session ended; not seen closed since; closed; pressed.
        let after = answers(&rules, a, [(false, 6, 0), (true, 7, 0), (false, 8, 0), (true, 9, 0)])
        #expect(after == [nil, nil, nil, .pressed(a)])
    }

    @Test func aPiThatWasAwayArrivesAndOneThatWasThereDoesNot() {
        let b = Fingerprint(publicKey: NoiseKeyPair.generate().publicKey)
        var rules = PortalRules(now: t0)
        var call = 0
        var fromB: [PortalRules.Summon?] = []
        for t in stride(from: 0.0, through: 50, by: 1) {
            _ = rules.tick(now: t0.addingTimeInterval(t), pairs: [a, b], cadence: 1, called: true, toGroups: true)
            call += 1
            fromB.append(rules.heard(b, pairable: false, call: call, sessionUp: false, now: t0.addingTimeInterval(t)))
        }
        #expect(fromB.allSatisfy { $0 == nil })
        // a said nothing for the 35 s watched after the grace.
        let fromA = answers(&rules, a, [(false, call, 50.2), (false, call + 1, 51)])
        #expect(fromA == [.arrived(a), nil])
    }

    @Test func nothingIsWatchedWithoutCallsOrInTheGraceAfterSleep() {
        var rules = PortalRules(now: t0)
        watch(&rules, pairs: [a], from: 0, to: 40, called: false)             // a session was up: no calls went out
        let afterSession = answers(&rules, a, [(false, 1, 40)])
        watch(&rules, pairs: [a], from: 41, to: 71)                           // 31 s watched: more than enough
        _ = rules.tick(now: t0.addingTimeInterval(3671), pairs: [a], cadence: 1, called: true, toGroups: true)   // an hour asleep
        let afterWake = answers(&rules, a, [(false, 2, 3672)])
        watch(&rules, pairs: [a], from: 3673, to: 3720)
        let duringSession = answers(&rules, a, [(false, 3, 3720)], sessionUp: true)
        #expect(afterSession == [nil])
        #expect(afterWake == [nil])
        #expect(duringSession == [nil])
    }

    @Test func aLostPortalReturnsWhenItsPiAnswersAgain() {
        var rules = PortalRules(now: t0)
        rules.sessionEnded(a, lost: true, lastCall: 10)
        let got = answers(&rules, a, [(false, 10, 0), (false, 11, 0), (false, 12, 0)])
        #expect(got == [nil, .returned(a), nil])
    }

    @Test func aDismissedOfferStaysHiddenUntilItHasGoneQuiet() {
        var rules = PortalRules(now: t0)
        let first = rules.heardOffer(a, call: 1, now: t0.addingTimeInterval(1))
        rules.dismiss(a)
        let dismissed = rules.isDismissed(a)
        let again = rules.heardOffer(a, call: 2, now: t0.addingTimeInterval(1))
        let gone = [1.0, 5, 9, 11].map { rules.tick(now: t0.addingTimeInterval($0), pairs: [], cadence: 5, called: true, toGroups: true) }
        let back = rules.heardOffer(a, call: 3, now: t0.addingTimeInterval(12))
        #expect(first)
        #expect(dismissed)
        #expect(!again)
        // The loop's first pass counts nothing; two calls to the groups are not enough; three and 10 s are.
        #expect(gone == [[], [], [], [a]])
        #expect(back)
    }
}

@Suite struct NearbyListTests {
    let t0 = Date(timeIntervalSince1970: 2_000_000)

    func store(pairedWith keys: [[UInt8]]) throws -> PairingStore {
        var store = PairingStore(emptyAt: FileManager.default.temporaryDirectory.appending(path: "maryvnc-tests-\(UUID().uuidString)/pairs.json"))
        for key in keys { try store.upsert(publicKey: key, name: "maryos") }
        return store
    }

    @Test func aTagListsAPairedPiAnOfferAPiReadyToPairAndNothingElseCounts() throws {
        let piKey = NoiseKeyPair.generate().publicKey, stranger = NoiseKeyPair.generate().publicKey
        let pairing = try store(pairedWith: [piKey])
        defer { try? FileManager.default.removeItem(at: pairing.url.deletingLastPathComponent()) }
        var list = NearbyList()
        let tagged = list.hear(NearbyHeard(host: "10.0.0.74", port: 5901, piPublicKey: piKey, offer: nil, call: 1), pairing: pairing, now: t0)
        let offer = list.hear(NearbyHeard(host: "10.0.0.9", port: 5901, piPublicKey: nil, offer: .init(publicKey: stranger, name: "kitchen"), call: 1),
                              pairing: pairing, now: t0)
        // An offer carrying a paired Pi's key without the tag proves nothing about where that Pi is.
        let untagged = list.hear(NearbyHeard(host: "10.0.0.66", port: 5901, piPublicKey: nil, offer: .init(publicKey: piKey, name: "maryos"), call: 2),
                                 pairing: pairing, now: t0)
        #expect(tagged?.pi.isPaired == true)
        #expect(tagged?.pi.isPairable == false)
        #expect(tagged?.address == "10.0.0.74")
        #expect(tagged?.isNew == true)
        #expect(offer?.pi.isPaired == false)
        #expect(offer?.address == nil)
        #expect(untagged == nil)
        #expect(list.pairedNearby.map(\.host) == ["10.0.0.74"])
        #expect(list.readyToPair.map(\.name) == ["kitchen"])
        list.prune(now: t0.addingTimeInterval(41)) { $0.publicKey == piKey }
        #expect(list.pis.map(\.name) == ["maryos"])
    }

    @Test func aPiKeepsTheAddressItIsListedWithWhileThatAddressAnswers() throws {
        let piKey = NoiseKeyPair.generate().publicKey
        var pairing = try store(pairedWith: [piKey])
        defer { try? FileManager.default.removeItem(at: pairing.url.deletingLastPathComponent()) }
        var list = NearbyList()
        let first = list.hear(NearbyHeard(host: "10.0.0.74", port: 5901, piPublicKey: piKey, offer: nil, call: 1), pairing: pairing, now: t0)
        try pairing.remember(address: try #require(first?.address), for: Fingerprint(publicKey: piKey))
        // The same call to the groups, answered from the link-local address too.
        let other = list.hear(NearbyHeard(host: "fe80::1%en0", port: 5901, piPublicKey: piKey, offer: nil, call: 1), pairing: pairing, now: t0.addingTimeInterval(0.01))
        let later = list.hear(NearbyHeard(host: "fe80::1%en0", port: 5901, piPublicKey: piKey, offer: nil, call: 2), pairing: pairing, now: t0.addingTimeInterval(20))
        // 10.0.0.74 has been silent for 31 s: the Pi moves.
        let moved = list.hear(NearbyHeard(host: "fe80::1%en0", port: 5901, piPublicKey: piKey, offer: nil, call: 3), pairing: pairing, now: t0.addingTimeInterval(31))
        #expect(other?.pi.host == "10.0.0.74")
        #expect(other?.address == nil)
        #expect(later?.pi.host == "10.0.0.74")
        #expect(moved?.pi.host == "fe80::1%en0")
        #expect(moved?.address == "fe80::1%en0")
    }
}

@Suite struct SessionOutcomeTests {
    @Test func onlyALostLinkIsTriedAgainAndOnlyAPiThatSaysSoIsForgotten() {
        #expect(SessionEnd.byViewer.outcome(name: "maryos") == .closed)
        #expect(SessionEnd.bye("shutdown").outcome(name: "maryos") == .retry)
        #expect(SessionEnd.failed("the connection dropped").outcome(name: "maryos") == .retry)
        if case .forget = SessionEnd.bye("forgotten").outcome(name: "maryos") {} else { Issue.record("bye forgotten forgets the Pi") }
        for end in [SessionEnd.bye("replaced"), .notPaired, .pairingRefused, .wrongPi] {
            if case .settle = end.outcome(name: "maryos") {} else { Issue.record("\(end) settles and forgets nothing") }
        }
    }
}
