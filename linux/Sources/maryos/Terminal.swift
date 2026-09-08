import Darwin
import Foundation

/// Puts this terminal in raw mode, feeds its keystrokes to the guest's
/// serial port and watches for Ctrl-] (0x1d), the key that stops the VM.
final class TerminalConsole: @unchecked Sendable {
    static let escapeByte: UInt8 = 0x1d

    private let pipe = Pipe()
    private var original = termios()
    private var raw = false
    private var source: DispatchSourceRead?

    /// The guest reads its console input from here.
    var guestInput: FileHandle { pipe.fileHandleForReading }

    init(onEscape: @escaping @Sendable () -> Void) {
        let writer = pipe.fileHandleForWriting
        let source = DispatchSource.makeReadSource(fileDescriptor: STDIN_FILENO, queue: .global())
        source.setEventHandler { [weak source] in
            var buffer = [UInt8](repeating: 0, count: 1024)
            let count = read(STDIN_FILENO, &buffer, buffer.count)
            guard count > 0 else {
                // End of input (a closed pipe): stop watching, keep the VM running.
                source?.cancel()
                return
            }
            var bytes = Array(buffer[0..<count])
            if bytes.contains(Self.escapeByte) {
                bytes.removeAll { $0 == Self.escapeByte }
                onEscape()
            }
            if !bytes.isEmpty {
                try? writer.write(contentsOf: Data(bytes))
            }
        }
        source.resume()
        self.source = source
    }

    var isTerminal: Bool { isatty(STDIN_FILENO) == 1 }

    func enterRawMode() {
        guard isTerminal, !raw else { return }
        tcgetattr(STDIN_FILENO, &original)
        var settings = original
        cfmakeraw(&settings)
        // Keep output post-processing so this program's own lines still end in CR LF.
        settings.c_oflag |= tcflag_t(OPOST | ONLCR)
        tcsetattr(STDIN_FILENO, TCSANOW, &settings)
        raw = true
    }

    func restore() {
        guard raw else { return }
        tcsetattr(STDIN_FILENO, TCSANOW, &original)
        raw = false
    }
}
