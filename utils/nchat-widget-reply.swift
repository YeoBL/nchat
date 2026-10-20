// Explicit reply/read actions only. This helper never writes message-cache flags.
import Foundation
import Darwin

enum WidgetReplyBridge {
    static func backendRunning(configRoot: String) -> Bool {
        guard let marker = secureRead(configRoot + "/widget-replies/server") else { return false }
        let fields = marker.split(separator: " ")
        guard fields.count >= 2, let pid = Int32(fields[0]), pid > 1,
              fields[1].count == 32, fields[1].allSatisfy({ "0123456789abcdef".contains($0) }),
              kill(pid, 0) == 0 else { return false }
        // A marker can survive a crash/reboot and its PID can be reused.
        var path = [CChar](repeating: 0, count: 4 * Int(MAXPATHLEN))
        guard proc_pidpath(pid, &path, UInt32(path.count)) > 0 else { return false }
        return URL(fileURLWithPath: String(cString: path)).lastPathComponent == "nchat"
    }

    static func secureRead(_ path: String) -> String? {
        let fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        var st = stat()
        guard fstat(fd, &st) == 0, st.st_mode & S_IFMT == S_IFREG, st.st_uid == getuid(),
              st.st_mode & 0o077 == 0, st.st_size > 0, st.st_size <= 512 else { return nil }
        var bytes = [UInt8](repeating: 0, count: Int(st.st_size))
        guard read(fd, &bytes, bytes.count) == bytes.count else { return nil }
        return String(bytes: bytes, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
    }

}
