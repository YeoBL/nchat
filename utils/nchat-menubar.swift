// Native cache viewer. Downloads and explicit replies use narrow nchat bridges;
// previews never select a chat or issue read receipts.
import AppKit
import SQLite3
import ImageIO
import Darwin
import QuartzCore

struct UnreadChat: Codable, Equatable {
    let id: String
    let name: String
    let service: String
    let count: Int
    let latest: Int64
    let cachePath: String
    var revision: Int64 = 0
    var messageIDs: [String] = []
    var key: String { cachePath + "\n" + id }
}

struct PreviewMessage {
    let sender: String
    let text: String
    let time: Int64
    let attachment: Bool
    var image: NSImage? = nil
    var imageURL: URL? = nil
    var id: String = ""
    var needsDownload = false
    var attachmentStatus: String? = nil
}

func clean(_ value: String) -> String {
    value.components(separatedBy: .controlCharacters).joined(separator: " ")
}

func messageDate(_ milliseconds: Int64) -> Date {
    Date(timeIntervalSince1970: Double(milliseconds) / 1000)
}

// Shared by the badge and expanded rows: a filtered event must not reappear on expansion.
let unreadCandidatePredicate = """
    m.isOutgoing = 0 AND m.isRead = 0 AND COALESCE(m.isDeleted, 0) = 0

    AND m.chatId != 'status@broadcast'
    AND COALESCE(m.text, '') NOT IN ('[Joined]', '[Left]')
    AND COALESCE(m.text, '') NOT GLOB '[[]Added *]'
    AND COALESCE(m.text, '') NOT GLOB '[[]Removed *]'
    """

struct Snapshot: Codable {
    var chats: [UnreadChat] = []
    var error: String? = nil
    var syncOffline = false
    var total: Int { chats.reduce(0) { $0 + $1.count } }
    var title: String { syncOffline ? "nchat offline" : (error == nil ? "nchat \(total)" : "nchat ?") }
}

struct CacheError: Error, CustomStringConvertible {
    let description: String
}

final class Cache {
    // Telegram now backfills full settings on sync. Unknown is not unarchived.
    private var knownSettingsPredicate: String {
        service == "Telegram" ? " AND ch.isArchived IS NOT NULL AND ch.isMuted IS NOT NULL" : ""
    }
    private var membershipPredicate: String {
        // Older caches still open while nchat upgrades them on its next start.
        guard service == "Telegram", let check = try? prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name='chat_membership'") else { return "" }
        defer { sqlite3_finalize(check) }
        guard sqlite3_step(check) == SQLITE_ROW else { return "" }
        return " AND NOT EXISTS (SELECT 1 FROM chat_membership cm WHERE cm.chatId=m.chatId AND cm.isMember=0)"
    }
    let path: String
    let service: String
    private var db: OpaquePointer?
    private var version: Int64 = -1
    private var cached: [UnreadChat] = []
    private let inode: NSNumber?
    private let readTimeout: TimeInterval

    init(path: String, service: String, readTimeout: TimeInterval = 3) throws {
        self.path = path
        self.service = service
        self.readTimeout = readTimeout
        inode = (try FileManager.default.attributesOfItem(atPath: path))[.systemFileNumber] as? NSNumber
        guard sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nil) == SQLITE_OK else {
            let detail = db.map { String(cString: sqlite3_errmsg($0)) } ?? "Cannot open cache"
            sqlite3_close(db); db = nil
            throw CacheError(description: detail)
        }
        sqlite3_busy_timeout(db, 250)
    }

    deinit { sqlite3_close(db) }

    func replaced() -> Bool {
        let current = try? FileManager.default.attributesOfItem(atPath: path)[.systemFileNumber] as? NSNumber
        return current != inode
    }

    func prepare(_ sql: String) throws -> OpaquePointer {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &statement, nil) == SQLITE_OK, let result = statement else {
            throw CacheError(description: String(cString: sqlite3_errmsg(db)))
        }
        return result
    }

    func rows() throws -> [UnreadChat] {
        let currentVersion: Int64 = try {
            let check = try prepare("PRAGMA data_version")
            defer { sqlite3_finalize(check) }
            guard sqlite3_step(check) == SQLITE_ROW else { throw CacheError(description: "Cache is busy") }
            return sqlite3_column_int64(check, 0)
        }()
        if currentVersion == version { return cached }

        // Still bounded for nchat's writer, with scheduling headroom under load.
        // Use a monotonic clock so wall-clock corrections cannot cancel reads.
        var deadline = ProcessInfo.processInfo.systemUptime + readTimeout
        return try withUnsafeMutablePointer(to: &deadline) { deadline in
            sqlite3_progress_handler(db, 2000, { raw in
                guard let raw = raw else { return 0 }
                return ProcessInfo.processInfo.systemUptime > raw.assumingMemoryBound(to: Double.self).pointee ? 1 : 0
            }, deadline)
            defer { sqlite3_progress_handler(db, 0, nil, nil) }
            return try queryRows(version: currentVersion)
        }
    }

    private func queryRows(version currentVersion: Int64) throws -> [UnreadChat] {
        // A later incoming read retires older orphaned WhatsApp unread flags.
        // Outgoing messages are not read progress. This never rewrites flags.
        let statement = try prepare("""
            WITH candidates AS MATERIALIZED (
                SELECT m.chatId, m.id, COALESCE(NULLIF(c.name, ''), m.chatId) AS name, m.timeSent
                FROM messages m
                LEFT JOIN chats2 ch ON ch.id = m.chatId
                LEFT JOIN contacts2 c ON c.id = m.chatId
                WHERE \(unreadCandidatePredicate)
            ), progress AS (
                SELECT chatId, MAX(timeSent) AS time FROM messages
                WHERE isOutgoing = 0 AND isRead = 1
                    AND chatId IN (SELECT chatId FROM candidates)
                GROUP BY chatId
            )
            SELECT m.chatId, m.name, COUNT(*), MAX(m.timeSent), json_group_array(m.id)
            FROM candidates m
            LEFT JOIN progress p ON p.chatId = m.chatId
            WHERE \(service == "Telegram" ? "1" : "m.timeSent > COALESCE(p.time, 0)")
            GROUP BY m.chatId
            ORDER BY MAX(m.timeSent) DESC, m.chatId
            """)
        defer { sqlite3_finalize(statement) }
        var result: [UnreadChat] = []
        var step = sqlite3_step(statement)
        while step == SQLITE_ROW {
            result.append(UnreadChat(id: String(cString: sqlite3_column_text(statement, 0)),
                name: String(cString: sqlite3_column_text(statement, 1)), service: service,
                count: Int(sqlite3_column_int64(statement, 2)), latest: sqlite3_column_int64(statement, 3), cachePath: path, revision: currentVersion,
                messageIDs: try JSONDecoder().decode([String].self, from: Data(String(cString: sqlite3_column_text(statement, 4)).utf8))))
            step = sqlite3_step(statement)
        }
        guard step == SQLITE_DONE else { throw CacheError(description: String(cString: sqlite3_errmsg(db))) }
        cached = result
        version = currentVersion
        return result
    }

}

final class Reader {
    let root: String
    let checkBackend: Bool
    private var caches: [String: Cache] = [:]
    init(root: String, checkBackend: Bool = true) {
        self.root = root
        self.checkBackend = checkBackend
    }

    func snapshot() -> Snapshot {
        let offline = checkBackend && !WidgetReplyBridge.backendRunning(configRoot: URL(fileURLWithPath: root).deletingLastPathComponent().path)
        do {
            let profiles = try FileManager.default.contentsOfDirectory(atPath: root).filter {
                $0.hasPrefix("Telegram_") || $0.hasPrefix("WhatsAppMd_")
            }.sorted()
            guard !profiles.isEmpty else { throw CacheError(description: "No nchat caches found") }
            var chats: [UnreadChat] = []
            for profile in profiles {
                let path = root + "/" + profile + "/db.sqlite"
                if caches[path] == nil || caches[path]!.replaced() {
                    caches[path] = try Cache(path: path, service: profile.hasPrefix("Telegram_") ? "Telegram" : "WhatsApp")
                }
                chats += try caches[path]!.rows()
            }
            return Snapshot(chats: chats.sorted {
                $0.latest != $1.latest ? $0.latest > $1.latest : ($0.name, $0.service, $0.id) < ($1.name, $1.service, $1.id)
            }, syncOffline: offline)
        } catch {
            // Never label an unreadable/busy database as 'all read'. Retry next poll.
            return Snapshot(error: String(describing: error), syncOffline: offline)
        }
    }
}

func menuLines(_ snapshot: Snapshot) -> [String] {
    if snapshot.syncOffline { return ["nchat is not running — unread count may be out of date", "Start nchat to resume syncing."] }
    if let error = snapshot.error { return ["Cache unavailable — retrying", error] }
    if snapshot.chats.isEmpty { return ["No unread messages"] }
    return snapshot.chats.map {
        // Control characters in a chat name must not create fake menu rows.
        let name = $0.name.components(separatedBy: .controlCharacters).joined(separator: " ")
        return "\($0.count) · \(name) — \($0.service)"
    }
}

final class MenuBar: NSObject, NSApplicationDelegate {
    let reader: Reader
    let diagnosticPath: String
    private var item: NSStatusItem!
    private var timer: DispatchSourceTimer?
    private let cacheQueue = DispatchQueue(label: "nchat.cache-reader", qos: .utility)
    init(root: String, diagnosticPath: String) {
        reader = Reader(root: root)
        self.diagnosticPath = diagnosticPath
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.autosaveName = "nchatUnread"
        let source = DispatchSource.makeTimerSource(queue: cacheQueue)
        source.schedule(deadline: .now(), repeating: 2, leeway: .milliseconds(200))
        source.setEventHandler { [weak self] in
            guard let self = self else { return }
            let state = self.reader.snapshot()
            DispatchQueue.main.async { self.render(state) }
        }
        timer = source
        source.resume()
    }
    func render(_ state: Snapshot) {
        let lines = menuLines(state)
        item.button?.title = state.syncOffline ? "–" : state.error == nil ? "\(state.total)" : "?"
        item.button?.image = NSImage(systemSymbolName: state.total > 0 ? "bubble.left.fill" : "bubble.left", accessibilityDescription: "nchat unread messages")
        item.button?.image?.isTemplate = true
        item.button?.toolTip = lines.joined(separator: "\n")
        let menu = NSMenu()
        for line in lines { menu.addItem(withTitle: line, action: nil, keyEquivalent: "") }
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit nchat Unread", action: #selector(quitApp), keyEquivalent: "q").target = self
        item.menu = menu
        if let data = try? JSONEncoder().encode(state) {
            try? data.write(to: URL(fileURLWithPath: diagnosticPath), options: .atomic)
        }
    }
    @objc func quitApp() { NSApplication.shared.terminate(nil) }
}

@main
struct UnreadApp {
    static func main() throws {
        let args = CommandLine.arguments
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let root: String = {
            if let index = args.firstIndex(of: "--history-root"), index + 1 < args.count { return args[index + 1] }
            return home + "/.config/nchat/history"
        }()
        if args.contains("--snapshot") {
            let data = try JSONEncoder().encode(Reader(root: root).snapshot())
            print(String(decoding: data, as: UTF8.self))
        } else {
            let app = NSApplication.shared
            app.setActivationPolicy(.accessory)

            let delegate = MenuBar(root: root, diagnosticPath: home + "/.config/nchat/menubar-state.json")
            app.delegate = delegate
            app.run()
        }
    }
}
