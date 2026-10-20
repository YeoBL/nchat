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

    static func queueImage(cachePath: String, chatID: String, messageID: String) -> Bool {
        let profile = URL(fileURLWithPath: cachePath).deletingLastPathComponent()
        let directory = profile.deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("peek-downloads").path
        var st = stat()
        guard lstat(directory, &st) == 0, st.st_mode & S_IFMT == S_IFDIR, st.st_uid == getuid(), st.st_mode & 0o077 == 0,
              let marker = secureRead(directory + "/server"), let pid = Int32(marker), pid > 1, kill(pid, 0) == 0 else { return false }
        let payload = [profile.lastPathComponent, chatID, messageID].map { $0.utf8.map { String(format: "%02x", $0) }.joined() }.joined(separator: "\n") + "\n"
        guard payload.utf8.count <= 8192, !chatID.isEmpty, !messageID.isEmpty else { return false }
        let id = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        do { try writeRequest(payload, path: directory + "/" + id + ".request"); return true }
        catch { return false }
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

    static func writeRequest(_ text: String, path: String) throws {
        let temporary = path + ".tmp"
        let fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
        guard fd >= 0 else { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
        var complete = false
        defer { close(fd); if !complete { unlink(temporary) } }
        let bytes = Array(text.utf8)
        let written = bytes.withUnsafeBytes { raw -> Bool in
            var offset = 0
            while offset < raw.count {
                let count = write(fd, raw.baseAddress!.advanced(by: offset), raw.count - offset)
                if count <= 0 { return false }
                offset += count
            }
            return true
        }
        guard written, fsync(fd) == 0, rename(temporary, path) == 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
        complete = true
    }

}
