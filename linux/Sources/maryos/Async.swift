import Foundation

private final class ResultBox<T>: @unchecked Sendable {
    var result: Result<T, any Error>?
}

/// Run async work from a synchronous command: the work runs on the main
/// actor while the real main thread pumps the main run loop, which is what
/// drains the main dispatch queue (MainActor jobs, Virtualization.framework
/// callbacks, signal sources) until the work is done.
func runBlocking<T: Sendable>(_ body: @escaping @MainActor @Sendable () async throws -> T) throws -> T {
    let box = ResultBox<T>()
    Task { @MainActor in
        do {
            box.result = .success(try await body())
        } catch {
            box.result = .failure(error)
        }
        CFRunLoopStop(CFRunLoopGetMain())
    }
    while box.result == nil {
        if !RunLoop.main.run(mode: .default, before: .distantFuture) {
            Thread.sleep(forTimeInterval: 0.05)
        }
    }
    return try box.result!.get()
}

/// SIGINT, SIGTERM and SIGHUP delivered on the main queue.
func installSignalHandlers(_ handler: @escaping @Sendable () -> Void) -> [DispatchSourceSignal] {
    [SIGINT, SIGTERM, SIGHUP].map { number in
        signal(number, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
        source.setEventHandler(handler: handler)
        source.resume()
        return source
    }
}
