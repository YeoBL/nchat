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
    var isViewOnce = false
}

let messageLinkDetector = try? NSDataDetector(types: NSTextCheckingResult.CheckingType.link.rawValue)

func clean(_ value: String) -> String {
    value.components(separatedBy: .controlCharacters).joined(separator: " ")
}

func messageDate(_ milliseconds: Int64) -> Date {
    Date(timeIntervalSince1970: Double(milliseconds) / 1000)
}

func decodedFileField(_ info: String, index: Int) -> String? {
    let parts = info.trimmingCharacters(in: .whitespacesAndNewlines).split(separator: ",", omittingEmptySubsequences: false)
    guard parts.indices.contains(index), parts[index].count <= 32768, parts[index].count % 2 == 0 else { return nil }
    let hex = Array(parts[index].utf8)
    var bytes: [UInt8] = []
    for i in stride(from: 0, to: hex.count, by: 2) {
        guard let byte = UInt8(String(decoding: hex[i...i+1], as: UTF8.self), radix: 16) else { return nil }
        bytes.append(byte)
    }
    return String(bytes: bytes, encoding: .utf8)
}

func decodedFilePath(_ info: String) -> String? {
    guard let path = decodedFileField(info, index: 2), path.hasPrefix("/"), !path.contains("\0") else { return nil }
    return path
}

func isViewOnce(_ info: String, text: String = "") -> Bool {
    let label = text.trimmingCharacters(in: .whitespacesAndNewlines)
    return decodedFileField(info, index: 4) == "1" || label.hasPrefix("[View once]") ||
        label.hasPrefix("[View-once ")
}

func isImageReference(_ info: String) -> Bool {
    let name = (decodedFileField(info, index: 2) ?? "").lowercased()
    let mime = decodedFileField(info, index: 3) ?? ""
    return ["[photo]", "[sticker]"].contains(name) || mime.hasPrefix("image/") ||
        ["jpg", "jpeg", "png", "webp", "gif", "heic", "avif", "tiff", "bmp"].contains((name as NSString).pathExtension)
}

func cachedImageURL(_ fileURL: URL, cacheRoot: String) -> URL? {
    guard fileURL.isFileURL else { return nil }
    let url = fileURL.resolvingSymlinksInPath()
    let root = URL(fileURLWithPath: cacheRoot).resolvingSymlinksInPath().path + "/"
    // Only nchat-owned local files; never fetch a URL or follow an outside symlink.
    guard url.path.hasPrefix(root),
          let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
          values.isRegularFile == true, (values.fileSize ?? Int.max) <= 50_000_000,
          let source = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary),
          CGImageSourceGetCount(source) > 0 else { return nil }
    return url
}

func cachedThumbnail(_ info: String, cacheRoot: String) -> NSImage? {
    guard !isViewOnce(info), let path = decodedFilePath(info),
          let url = cachedImageURL(URL(fileURLWithPath: path), cacheRoot: cacheRoot),
          let source = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary),
          let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 640
          ] as CFDictionary) else { return nil }
    return NSImage(cgImage: cg, size: NSSize(width: cg.width, height: cg.height))
}

// Shared by the badge and expanded rows: a filtered event must not reappear on expansion.
let unreadCandidatePredicate = """
    m.isOutgoing = 0 AND m.isRead = 0 AND COALESCE(m.isDeleted, 0) = 0
    AND COALESCE(ch.isArchived, 0) = 0 AND COALESCE(ch.isMuted, 0) = 0
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

// The companion can miss a cross-device read receipt during a disconnect.
// Only the native Mac app's up-to-date, explicit zero can retire its stale badge.
func whatsappChatsConfirmedRead(_ chats: [UnreadChat], databasePath: String) -> Set<String> {
    guard chats.contains(where: { $0.service == "WhatsApp" }) else { return [] }
    var db: OpaquePointer?
    guard sqlite3_open_v2(databasePath, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nil) == SQLITE_OK,
          let db = db else { sqlite3_close(db); return [] }
    defer { sqlite3_close(db) }
    sqlite3_busy_timeout(db, 100)
    var statement: OpaquePointer?
    guard sqlite3_prepare_v2(db, "SELECT ZUNREADCOUNT,ZLASTMESSAGEDATE FROM ZWACHATSESSION WHERE ZCONTACTJID=?1", -1, &statement, nil) == SQLITE_OK,
          let statement = statement else { sqlite3_finalize(statement); return [] }
    defer { sqlite3_finalize(statement) }
    let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
    var confirmed: Set<String> = []
    for chat in chats where chat.service == "WhatsApp" {
        sqlite3_reset(statement)
        sqlite3_clear_bindings(statement)
        guard sqlite3_bind_text(statement, 1, chat.id, -1, transient) == SQLITE_OK,
              sqlite3_step(statement) == SQLITE_ROW,
              sqlite3_column_type(statement, 0) != SQLITE_NULL,
              sqlite3_column_type(statement, 1) != SQLITE_NULL else { continue }
        let nativeLatestUnix = Int64(sqlite3_column_double(statement, 1)) + 978_307_200 // Core Data epoch.
        if sqlite3_column_int64(statement, 0) == 0 && nativeLatestUnix >= chat.latest / 1000 {
            confirmed.insert(chat.key)
        }
    }
    return confirmed
}

final class Cache {
    var dismissed: Set<String> = [] {
        didSet { if oldValue != dismissed { version = -1 } }
    }
    private var dismissalPredicate: String {
        guard !dismissed.isEmpty, let data = try? JSONEncoder().encode(Array(dismissed)) else { return "" }
        // Hex-encoded JSON is data, never interpolated SQL identifiers/text.
        let hex = data.map { String(format: "%02x", $0) }.joined()
        return " AND (m.chatId || char(10) || m.id) NOT IN (SELECT value FROM json_each(CAST(X'\(hex)' AS TEXT)))"
    }
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
                WHERE \(unreadCandidatePredicate)\(knownSettingsPredicate)\(dismissalPredicate)\(membershipPredicate)
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

    func preview(chatID: String) throws -> [PreviewMessage] {
        // A separate bounded SELECT, fully finalized before decoding images.
        // No open-chat action, downloads, or read-state changes are involved.
        var deadline = ProcessInfo.processInfo.systemUptime + readTimeout
        let records: [(PreviewMessage, String)] = try withUnsafeMutablePointer(to: &deadline) { deadline in
            sqlite3_progress_handler(db, 2000, { raw in
                guard let raw = raw else { return 0 }
                return ProcessInfo.processInfo.systemUptime > raw.assumingMemoryBound(to: Double.self).pointee ? 1 : 0
            }, deadline)
            defer { sqlite3_progress_handler(db, 0, nil, nil) }
            let statement = try prepare("""
                WITH progress AS (
                    SELECT MAX(timeSent) AS time FROM messages
                    WHERE chatId = ?1 AND isOutgoing = 0 AND isRead = 1
                )
                SELECT COALESCE(NULLIF(c.name, ''), NULLIF(m.senderId, ''), 'Unknown sender'),
                       COALESCE(m.text, ''), m.timeSent,
                       COALESCE(m.fileInfo, ''), m.id
                FROM messages m LEFT JOIN contacts2 c ON c.id = m.senderId
                LEFT JOIN chats2 ch ON ch.id = m.chatId CROSS JOIN progress p
                WHERE m.chatId = ?1 AND \(unreadCandidatePredicate)\(knownSettingsPredicate)\(dismissalPredicate)\(membershipPredicate)
                    \(service == "Telegram" ? "" : "AND m.timeSent > COALESCE(p.time, 0)")
                ORDER BY m.timeSent DESC, m.id DESC LIMIT 200
                """)
            defer { sqlite3_finalize(statement) }
            let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
            guard sqlite3_bind_text(statement, 1, chatID, -1, transient) == SQLITE_OK else {
                throw CacheError(description: "Cannot select chat")
            }
            var messages: [(PreviewMessage, String)] = []
            var step = sqlite3_step(statement)
            while step == SQLITE_ROW {
                let info = String(cString: sqlite3_column_text(statement, 3))
                let text = String(cString: sqlite3_column_text(statement, 1))
                let hidden = isViewOnce(info, text: text)
                messages.append((PreviewMessage(sender: String(cString: sqlite3_column_text(statement, 0)),
                    text: hidden ? (isImageReference(info) || text.hasPrefix("[View-once photo]") ?
                        "View-once photo" : "View-once attachment") : text, time: sqlite3_column_int64(statement, 2),
                    attachment: !info.isEmpty || hidden, id: String(cString: sqlite3_column_text(statement, 4)),
                    isViewOnce: hidden), info))
                step = sqlite3_step(statement)
            }
            guard step == SQLITE_DONE else { throw CacheError(description: String(cString: sqlite3_errmsg(db))) }
            return messages.reversed()
        }
        let cacheRoot = URL(fileURLWithPath: path).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().path
        // ponytail: cap thumbnail decoding at 20 per expansion; remaining attachments stay labelled.
        var images = 0
        return records.map { message, info in
            var result = message
            if result.isViewOnce {
                result.attachmentStatus = "Hidden · Ctrl+V in nchat to view"
                return result
            }
            if images < 20, let image = cachedThumbnail(info, cacheRoot: cacheRoot) {
                result.image = image
                result.imageURL = decodedFilePath(info).map { URL(fileURLWithPath: $0) }
                images += 1
            }
            let local = decodedFilePath(info).map { FileManager.default.fileExists(atPath: $0) } ?? false
            result.needsDownload = !local && isImageReference(info) && !(decodedFileField(info, index: 1) ?? "").isEmpty
            return result
        }
    }
}

final class Reader {
    let root: String
    let checkBackend: Bool
    private var caches: [String: Cache] = [:]
    private var dismissed: [String: Set<String>] = [:]
    private var dismissalURL: URL { URL(fileURLWithPath: root).deletingLastPathComponent().appendingPathComponent("widget-dismissed.json") }
    init(root: String, checkBackend: Bool = true) {
        self.root = root
        self.checkBackend = checkBackend
        if let data = try? Data(contentsOf: dismissalURL), let stored = try? JSONDecoder().decode([String: Set<String>].self, from: data) {
            dismissed = stored
        }
    }

    func clear(_ chat: UnreadChat) throws {
        var next = dismissed
        next[chat.cachePath, default: []].formUnion(chat.messageIDs.map { chat.id + "\n" + $0 })
        try JSONEncoder().encode(next).write(to: dismissalURL, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: dismissalURL.path)
        dismissed = next
    }

    func preview(_ chat: UnreadChat) throws -> [PreviewMessage] {
        let cache = try Cache(path: chat.cachePath, service: chat.service)
        cache.dismissed = dismissed[chat.cachePath] ?? []
        return try cache.preview(chatID: chat.id)
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
                caches[path]!.dismissed = dismissed[path] ?? []
                chats += try caches[path]!.rows()
            }
            let nativeWhatsAppPath = FileManager.default.homeDirectoryForCurrentUser.path +
                "/Library/Group Containers/group.net.whatsapp.WhatsApp.shared/ChatStorage.sqlite"
            let confirmedRead = whatsappChatsConfirmedRead(chats, databasePath: nativeWhatsAppPath)
            chats.removeAll { confirmedRead.contains($0.key) }
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

final class FlippedView: NSView { override var isFlipped: Bool { true } }

final class ImageButton: NSButton {
    var imageURL: URL?
    var cacheRoot = ""
    override func resetCursorRects() {
        super.resetCursorRects()
        if isEnabled { addCursorRect(bounds, cursor: .pointingHand) }
    }
}

func imageOpenError(_ detail: String) {
    let alert = NSAlert()
    alert.messageText = "Couldn't open image in Preview"
    alert.informativeText = detail
    alert.runModal()
}

func openFullImageInPreview(_ url: URL) {
    guard let preview = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.Preview") else {
        imageOpenError("macOS Preview could not be found.")
        return
    }
    // Open the original cache file, never the resized widget thumbnail.
    NSWorkspace.shared.open([url], withApplicationAt: preview, configuration: NSWorkspace.OpenConfiguration()) { _, error in
        if let error = error { DispatchQueue.main.async { imageOpenError(error.localizedDescription) } }
    }
}

final class MessageCard: NSView {
    override func draw(_ dirtyRect: NSRect) {
        NSColor.labelColor.withAlphaComponent(0.05).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8).fill()
    }
}

final class ChatActionButton: NSButton {
    var chat: UnreadChat?
}

final class ChatButton: NSButton {
    var chat: UnreadChat?
    var expandedHighlight = false { didSet { updateHighlight() } }
    private var hover = false
    private var tracking: NSTrackingArea?
    override func updateTrackingAreas() {
        if let tracking = tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil)
        tracking = area
        addTrackingArea(area)
        super.updateTrackingAreas()
    }
    override func mouseEntered(with event: NSEvent) { hover = true; updateHighlight() }
    override func mouseExited(with event: NSEvent) { hover = false; updateHighlight() }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateHighlight()
    }
    private func updateHighlight() {
        wantsLayer = true
        layer?.cornerRadius = 8
        effectiveAppearance.performAsCurrentDrawingAppearance {
            layer?.backgroundColor = (expandedHighlight ? NSColor.controlAccentColor.withAlphaComponent(0.12) :
                hover ? NSColor.labelColor.withAlphaComponent(0.07) : .clear).cgColor
        }
    }
    override var isFlipped: Bool { true }
    // Labels/icons are decorative; the entire row is one accessible button.
    override func hitTest(_ point: NSPoint) -> NSView? {
        super.hitTest(point) == nil ? nil : self
    }
}

final class UnreadPopoverController: NSViewController, NSTextViewDelegate {
    let scroll = NSScrollView()
    let document = FlippedView()
    let backButton = NSButton(title: "", target: nil, action: nil)
    let header = FlippedView()
    let heading = NSTextField(labelWithString: "Unread")
    let options = NSPopUpButton(frame: .zero, pullsDown: true)
    let closeButton = NSButton(title: "", target: nil, action: nil)
    let headerRule = NSBox()
    let composer = FlippedView()
    let replyTitle = NSTextField(labelWithString: "")
    let replyField = NSTextView()
    let replyScroll = NSScrollView()
    let sendButton = NSButton(title: "Send", target: nil, action: nil)
    let closeReplyButton = NSButton(title: "×", target: nil, action: nil)
    let replyStatus = NSTextField(wrappingLabelWithString: "")
    var replyChat: UnreadChat?
    var replyReference = ""
    var drafts: [String: String] = [:]
    var draftChats: [String: UnreadChat] = [:]
    var draftReferences: [String: String] = [:]
    var sending: Set<String> = []
    var sendStatuses: [String: String] = [:]
    var unconfirmedTexts: [String: String] = [:]
    var state = Snapshot()
    var expanded: Set<String> = []
    var previews: [String: [PreviewMessage]] = [:]
    var errors: [String: String] = [:]
    var onToggle: ((UnreadChat) -> Void)?
    var onClose: (() -> Void)?
    var onCollapse: (() -> Void)?
    var onQuit: (() -> Void)?
    var onSend: ((UnreadChat, String, String) -> Void)?
    var onClear: ((UnreadChat) -> Void)?
    var onMarkRead: ((UnreadChat) -> Void)?
    var markingRead: Set<String> = []
    var actionErrors: [String: String] = [:]
    var onResize: ((NSSize) -> Void)?
    var onOpenImage: (URL) -> Void = openFullImageInPreview
    var onImageOpenError: (String) -> Void = imageOpenError
    var maxHeight: CGFloat = 760
    var width: CGFloat = 454
    var targetContentSize = NSSize(width: 454, height: 100)
    private var serviceIcons: [String: NSImage] = [:]

    override func loadView() {
        view = FlippedView(frame: NSRect(x: 0, y: 0, width: width, height: 100))
        scroll.frame = view.bounds
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.automaticallyAdjustsContentInsets = false
        scroll.documentView = document
        view.addSubview(scroll)
        view.addSubview(header)
        heading.font = .systemFont(ofSize: 13, weight: .semibold)
        header.addSubview(heading)
        options.isBordered = false
        options.addItem(withTitle: "")
        options.item(at: 0)?.image = NSImage(systemSymbolName: "ellipsis", accessibilityDescription: "Options")
        options.addItem(withTitle: "Quit nchat Unread")
        options.item(at: 1)?.target = self
        options.item(at: 1)?.action = #selector(quitApp)
        options.setAccessibilityLabel("Widget options")
        options.toolTip = "Widget options"
        header.addSubview(options)
        closeButton.image = NSImage(systemSymbolName: "xmark", accessibilityDescription: "Close")
        closeButton.isBordered = false
        closeButton.target = self
        closeButton.action = #selector(closePopover)
        closeButton.toolTip = "Close preview (Esc)"
        closeButton.setAccessibilityLabel("Close preview")
        header.addSubview(closeButton)
        headerRule.boxType = .separator
        header.addSubview(headerRule)
        backButton.bezelStyle = .rounded
        backButton.isBordered = false
        backButton.title = "All chats"
        backButton.imagePosition = .imageLeading
        backButton.image = NSImage(systemSymbolName: "chevron.left", accessibilityDescription: "All chats")
        backButton.toolTip = "Back to all chats"
        backButton.font = .systemFont(ofSize: 13, weight: .medium)
        backButton.target = self
        backButton.action = #selector(collapseAll)
        backButton.setAccessibilityLabel("All chats — collapse message previews")
        backButton.isHidden = true
        header.addSubview(backButton)
        composer.isHidden = true
        view.addSubview(composer)
        replyTitle.font = .systemFont(ofSize: 12, weight: .medium)
        replyTitle.lineBreakMode = .byTruncatingTail
        composer.addSubview(replyTitle)
        replyScroll.borderType = .bezelBorder
        replyScroll.hasVerticalScroller = true
        replyScroll.autohidesScrollers = true
        replyField.isRichText = false
        replyField.font = .systemFont(ofSize: 13)
        replyField.textColor = .textColor
        replyField.backgroundColor = .textBackgroundColor
        replyField.textContainerInset = NSSize(width: 6, height: 6)
        replyField.isHorizontallyResizable = false
        replyField.isVerticallyResizable = true
        replyField.autoresizingMask = [.width]
        replyField.textContainer?.widthTracksTextView = true
        replyField.delegate = self
        replyField.setAccessibilityLabel("Reply text. Use the Send button to send.")
        replyScroll.documentView = replyField
        composer.addSubview(replyScroll)
        sendButton.bezelStyle = .rounded
        sendButton.target = self
        sendButton.action = #selector(sendReply)
        composer.addSubview(sendButton)
        replyStatus.font = .systemFont(ofSize: 11)
        replyStatus.textColor = .secondaryLabelColor
        replyStatus.maximumNumberOfLines = 3
        composer.addSubview(replyStatus)
        let cancel = closeReplyButton
        cancel.target = self
        cancel.action = #selector(closeComposer)
        cancel.isBordered = false
        cancel.frame = NSRect(x: width - 36, y: 5, width: 24, height: 24)
        cancel.setAccessibilityLabel("Close reply and keep draft")
        composer.addSubview(cancel)
    }

    override func viewDidLayout() {
        super.viewDidLayout()
        layoutScroll()
    }

    private func layoutScroll() {
        let top: CGFloat = 46
        let bottom: CGFloat = replyChat == nil ? 0 : 158
        backButton.isHidden = expanded.isEmpty
        header.frame = NSRect(x: 0, y: 0, width: width, height: top)
        backButton.frame = NSRect(x: 10, y: 7, width: 88, height: 30)
        heading.frame = NSRect(x: expanded.isEmpty ? 18 : 110, y: 14, width: expanded.isEmpty ? width - 100 : width - 192, height: 20)
        heading.stringValue = state.syncOffline ? "Unread · offline" : (state.error == nil ? "Unread · \(state.total)" : "Unread · unavailable")
        options.frame = NSRect(x: width - 77, y: 7, width: 32, height: 30)
        closeButton.frame = NSRect(x: width - 40, y: 7, width: 30, height: 30)
        headerRule.frame = NSRect(x: 14, y: 45, width: width - 28, height: 1)
        scroll.frame = NSRect(x: 0, y: top, width: width, height: max(0, view.bounds.height - top - bottom))
        composer.isHidden = replyChat == nil
        composer.frame = NSRect(x: 0, y: view.bounds.height - bottom, width: width, height: bottom)
        replyTitle.frame = NSRect(x: 16, y: 10, width: width - 58, height: 18)
        closeReplyButton.frame = NSRect(x: width - 36, y: 5, width: 24, height: 24)
        replyScroll.frame = NSRect(x: 16, y: 36, width: width - 32, height: 65)
        replyField.setFrameSize(NSSize(width: replyScroll.contentSize.width, height: max(65, replyField.frame.height)))
        replyField.textContainer?.containerSize = NSSize(width: replyScroll.contentSize.width, height: .greatestFiniteMagnitude)
        replyStatus.frame = NSRect(x: 16, y: 109, width: width - 108, height: 45)
        sendButton.frame = NSRect(x: width - 88, y: 109, width: 72, height: 30)
        // Short lists must never retain a scrolled origin after the popover resizes.
        if document.frame.height <= scroll.contentView.bounds.height {
            scroll.contentView.scroll(to: .zero)
        }
    }

    override func cancelOperation(_ sender: Any?) {
        if replyChat != nil { closeComposer() }
        else if expanded.isEmpty { onClose?() } else { onCollapse?() }
    }

    @objc func collapseAll() { onCollapse?() }
    @objc func closePopover() { onClose?() }

    func revealChat(_ chat: UnreadChat) {
        view.layoutSubtreeIfNeeded()
        guard let row = document.subviews.compactMap({ $0 as? ChatButton }).first(where: { $0.chat?.key == chat.key }) else { return }
        // Scroll only our message viewport, never the popover's ancestor clip view.
        let y = min(row.frame.minY, max(0, document.frame.height - scroll.contentView.bounds.height))
        scroll.contentView.scroll(to: NSPoint(x: 0, y: y))
        scroll.reflectScrolledClipView(scroll.contentView)
    }

    @discardableResult
    func label(_ text: String, x: CGFloat, y: CGFloat, width: CGFloat, size: CGFloat = 13,
               bold: Bool = false, secondary: Bool = false, lines: Int = 1,
               selectable: Bool = false, parent: NSView? = nil) -> CGFloat {
        let field = NSTextField(wrappingLabelWithString: text)
        field.font = .systemFont(ofSize: size, weight: bold ? .semibold : .regular)
        field.textColor = secondary ? .secondaryLabelColor : .labelColor
        field.isSelectable = selectable
        if selectable, let detector = messageLinkDetector {
            let paragraph = NSMutableParagraphStyle()
            paragraph.lineBreakMode = .byWordWrapping
            let linked = NSMutableAttributedString(string: text, attributes: [.font: field.font!,
                .foregroundColor: field.textColor!, .paragraphStyle: paragraph])
            for match in detector.matches(in: text, range: NSRange(text.startIndex..., in: text)) {
                guard let url = match.url, ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { continue }
                linked.addAttributes([.link: url, .foregroundColor: NSColor.linkColor,
                                      .underlineStyle: NSUnderlineStyle.single.rawValue], range: match.range)
            }
            field.allowsEditingTextAttributes = true
            field.attributedStringValue = linked
        }
        field.maximumNumberOfLines = lines
        field.lineBreakMode = lines == 1 ? .byTruncatingTail : .byWordWrapping
        field.cell?.truncatesLastVisibleLine = lines > 0
        // Measure the same native wrapping layout that will actually be drawn.
        let height = max(size + 4, ceil(field.sizeThatFits(NSSize(width: width, height: .greatestFiniteMagnitude)).height))
        field.frame = NSRect(x: x, y: y, width: width, height: height)
        if lines > 0 { field.toolTip = text }
        (parent ?? document).addSubview(field)
        return height
    }

    func serviceIcon(_ service: String) -> NSImage? {
        if let icon = serviceIcons[service] { return icon }
        let id = service == "WhatsApp" ? "net.whatsapp.WhatsApp" : "ru.keepcoder.Telegram"
        let icon = NSWorkspace.shared.urlForApplication(withBundleIdentifier: id).map { NSWorkspace.shared.icon(forFile: $0.path) }
            ?? NSImage(systemSymbolName: service == "WhatsApp" ? "phone.bubble.fill" : "paperplane.fill", accessibilityDescription: service)
        serviceIcons[service] = icon
        return icon
    }

    func rebuild() {
        _ = view
        width = 454
        let offset = scroll.contentView.bounds.origin.y
        document.subviews.forEach { $0.removeFromSuperview() }
        var y: CGFloat = 8
        if state.syncOffline {
            y += label("nchat is not running", x: 20, y: y + 16, width: width - 40, size: 14, bold: true) + 28
            y += label("Start nchat to sync. Cached messages below may be out of date.", x: 20, y: y, width: width - 40, size: 12, secondary: true) + 20
        } else if state.error != nil || state.chats.isEmpty {
            y += label(state.error == nil ? "You're all caught up" : "Couldn't load messages", x: 20, y: y + 16, width: width - 40, size: 14, bold: true) + 28
            y += label(state.error == nil ? "New messages will appear here." : "Retrying the local cache…", x: 20, y: y, width: width - 40, size: 12, secondary: true) + 20
        }
        for chat in state.chats {
            let button = ChatButton(frame: NSRect(x: 8, y: y, width: width - 16, height: 62))
            button.title = ""
            button.isBordered = false
            button.chat = chat
            button.target = self
            button.action = #selector(toggle(_:))
            let isExpanded = expanded.contains(chat.key)
            button.expandedHighlight = isExpanded
            button.setAccessibilityLabel("\(clean(chat.name)), \(chat.count) unread messages, \(chat.service), \(isExpanded ? "expanded" : "collapsed")")
            button.toolTip = "\(isExpanded ? "Collapse" : "Expand") unread messages"
            let icon = NSImageView(frame: NSRect(x: 10, y: 17, width: 28, height: 28))
            icon.image = serviceIcon(chat.service)
            icon.imageScaling = .scaleProportionallyUpOrDown
            button.addSubview(icon)
            label(clean(chat.name), x: 50, y: 11, width: width - 155, size: 14, bold: true, parent: button)
            label("\(chat.count) unread · \(chat.service)", x: 50, y: 32, width: width - 135, size: 12, secondary: true, parent: button)
            let age = max(0, Int(Date().timeIntervalSince(messageDate(chat.latest))))
            let ageText = age < 60 ? "now" : age < 3600 ? "\(age / 60)m" : age < 86400 ? "\(age / 3600)h" : "\(age / 86400)d"
            label(ageText, x: width - 98, y: 28, width: 45, size: 12, secondary: true, parent: button)
            let chevron = NSImageView(frame: NSRect(x: width - 49, y: 26, width: 13, height: 13))
            chevron.image = NSImage(systemSymbolName: isExpanded ? "chevron.down" : "chevron.right", accessibilityDescription: nil)
            chevron.contentTintColor = .secondaryLabelColor
            button.addSubview(chevron)
            document.addSubview(button)
            y += 66
            for (title, x, action) in [("Clear", CGFloat(58), #selector(clearChat(_:))),
                                        (markingRead.contains(chat.key) ? "Marking…" : "Mark as read", CGFloat(134), #selector(markChatRead(_:)))] {
                let control = ChatActionButton(title: title, target: self, action: action)
                control.chat = chat
                control.bezelStyle = .rounded
                control.font = .systemFont(ofSize: 11)
                control.frame = NSRect(x: x, y: y, width: title == "Clear" ? 68 : 112, height: 24)
                control.isEnabled = !markingRead.contains(chat.key)
                control.toolTip = title == "Clear" ? "Dismiss these notifications only. Messages stay unread in the app." : "Mark these messages as read in \(chat.service). Sends read receipts."
                control.setAccessibilityLabel("\(title): \(chat.name)")
                document.addSubview(control)
            }
            y += 30
            if let error = actionErrors[chat.key] {
                y += label(error, x: 60, y: y, width: width - 85, size: 11, secondary: true, lines: 3) + 8
            }
            if isExpanded {
                if let messages = previews[chat.key] {
                    if messages.isEmpty {
                        y += label("No unread messages remain in the cache.", x: 60, y: y + 6, width: width - 85, secondary: true, lines: 2) + 18
                    }
                    if chat.count > 200 {
                        y += label("Latest 200 of \(chat.count) unread messages", x: 60, y: y + 6, width: width - 85, secondary: true) + 14
                    }
                    let date = DateFormatter()
                    date.dateFormat = "HH:mm"
                    for message in messages {
                        let cardTop = y + 6
                        y += 12
                        label(clean(message.sender), x: 60, y: y, width: width - 143, size: 12, bold: true)
                        label(date.string(from: messageDate(message.time)), x: width - 68, y: y, width: 48, size: 11, secondary: true)
                        y += 22
                        let text = message.text
                        if !text.isEmpty {
                            y += label(text, x: 60, y: y, width: width - 86,
                                       lines: 0, selectable: true) + 5
                        }
                        if !message.isViewOnce, let image = message.image {
                            let scale = min((width - 86) / image.size.width, 200 / image.size.height, 1)
                            let size = NSSize(width: image.size.width * scale, height: image.size.height * scale)
                            let photo = ImageButton(frame: NSRect(origin: NSPoint(x: 60, y: y + 3), size: size))
                            photo.title = ""
                            photo.isBordered = false
                            photo.imagePosition = .imageOnly
                            photo.imageURL = message.imageURL
                            photo.cacheRoot = URL(fileURLWithPath: chat.cachePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().path
                            photo.isEnabled = message.imageURL != nil
                            photo.target = self
                            photo.action = #selector(openImage(_:))
                            photo.image = image
                            photo.imageScaling = .scaleProportionallyUpOrDown
                            photo.wantsLayer = true
                            photo.layer?.cornerRadius = 6
                            photo.layer?.masksToBounds = true
                            photo.toolTip = "Open full image in Preview"
                            photo.setAccessibilityLabel("Open full image in Preview")
                            document.addSubview(photo)
                            y += size.height + 9
                        } else if message.attachment || message.isViewOnce {
                            let status = message.isViewOnce ? "Hidden · Ctrl+V in nchat to view" :
                                (message.attachmentStatus ?? "Attachment · preview not cached or unsupported")
                            y += label(status, x: 60, y: y, width: width - 86, size: 12, secondary: true, lines: 2) + 5
                        }
                        y += 12
                        let card = MessageCard(frame: NSRect(x: 46, y: cardTop, width: width - 62, height: y - cardTop - 3))
                        document.addSubview(card, positioned: .below, relativeTo: nil)
                    }
                } else {
                    y += label(errors[chat.key] ?? "Loading unread messages…", x: 60, y: y + 8, width: width - 85, secondary: true, lines: 2) + 24
                }
                if let reference = previews[chat.key]?.last, !reference.id.isEmpty {
                    let reply = ChatButton(frame: NSRect(x: 54, y: y + 3, width: 82, height: 28))
                    reply.title = "Reply…"
                    reply.bezelStyle = .rounded
                    reply.isBordered = true
                    reply.alignment = .left
                    reply.font = .systemFont(ofSize: 12)
                    reply.chat = chat
                    reply.target = self
                    reply.action = #selector(beginReply(_:))
                    reply.setAccessibilityLabel("Reply to \(clean(chat.name)) on \(chat.service)")
                    document.addSubview(reply)
                    y += 38
                }
            }
            y += 6
        }
        for chat in draftChats.values.sorted(by: { $0.name < $1.name }) where
            !state.chats.contains(where: { $0.key == chat.key }) && !(drafts[chat.key] ?? "").isEmpty && replyChat?.key != chat.key {
            let resume = ChatButton(frame: NSRect(x: 16, y: y + 4, width: width - 32, height: 30))
            resume.title = "Resume reply to \(clean(chat.name)) · \(chat.service)"
            resume.isBordered = false
            resume.alignment = .left
            resume.chat = chat
            resume.target = self
            resume.action = #selector(beginReply(_:))
            document.addSubview(resume)
            y += 38
        }
        y += 8
        document.frame = NSRect(x: 0, y: 0, width: width, height: y)
        let bottom: CGFloat = replyChat == nil ? 0 : 158
        let size = NSSize(width: 454, height: min(maxHeight, y + bottom + 46))
        // Do not publish the final size via preferredContentSize: AppKit can
        // apply that separately and bypass the intermediate animation frames.
        targetContentSize = size
        // A hosted popover must resize its window before its content view.
        // Pre-sizing the view makes NSPopover treat contentSize as unchanged,
        // leaving the old small window clipping the expanded content.
        if let resize = onResize { resize(size) }
        else { view.setFrameSize(size) }
        layoutScroll()
        scroll.contentView.scroll(to: NSPoint(x: 0, y: min(offset, max(0, y - scroll.contentView.bounds.height))))
        scroll.reflectScrolledClipView(scroll.contentView)
        updateComposer()
    }

    @objc func clearChat(_ sender: ChatActionButton) {
        if let chat = sender.chat { onClear?(chat) }
    }
    @objc func markChatRead(_ sender: ChatActionButton) {
        if let chat = sender.chat { onMarkRead?(chat) }
    }
    @objc func toggle(_ sender: ChatButton) {
        if let chat = sender.chat { onToggle?(chat) }
    }
    @objc func openImage(_ sender: ImageButton) {
        // Recheck at click time in case the cache was cleared or the file replaced.
        guard let original = sender.imageURL,
              let url = cachedImageURL(original, cacheRoot: sender.cacheRoot) else {
            onImageOpenError("The cached image is no longer available. Close and reopen this chat's preview to reload it.")
            return
        }
        onOpenImage(url)
    }
    @objc func quitApp() { onQuit?() }

    @objc func beginReply(_ sender: ChatButton) {
        guard let chat = sender.chat, let reference = previews[chat.key]?.last?.id ?? draftReferences[chat.key], !reference.isEmpty else { return }
        saveDraft()
        replyChat = chat
        replyReference = reference
        replyField.string = drafts[chat.key] ?? ""
        rebuild()
        view.window?.makeFirstResponder(replyField)
    }

    func saveDraft() {
        if let chat = replyChat {
            drafts[chat.key] = replyField.string
            draftChats[chat.key] = chat
            draftReferences[chat.key] = replyReference
        }
    }

    func textDidChange(_ notification: Notification) { saveDraft(); updateComposer() }

    func updateComposer() {
        guard let chat = replyChat else { return }
        replyTitle.stringValue = "Reply to \(clean(chat.name)) · \(chat.service)"
        replyTitle.toolTip = replyTitle.stringValue
        let text = replyField.string
        let busy = sending.contains(chat.key)
        sendButton.title = busy ? "Sending…" : "Send"
        sendButton.isEnabled = !busy && !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
            text.utf16.count <= 4000 && unconfirmedTexts[chat.key] != text
        replyStatus.stringValue = text.utf16.count > 4000 ? "Reply is too long (maximum 4,000 characters)." :
            (sendStatuses[chat.key] ?? "Only Send transmits your reply.")
    }

    @objc func closeComposer() {
        saveDraft()
        replyChat = nil
        rebuild()
    }

    @objc func sendReply() {
        guard let chat = replyChat, sendButton.isEnabled else { return }
        saveDraft()
        onSend?(chat, replyReference, replyField.string)
    }

    func completeReply(_ chat: UnreadChat, text: String, outcome: ReplyOutcome) {
        sending.remove(chat.key)
        switch outcome {
        case .accepted:
            sendStatuses[chat.key] = "Submitted to \(chat.service)."
            if drafts[chat.key] == text { drafts[chat.key] = "" }
            if replyChat?.key == chat.key && replyField.string == text { replyField.string = "" }
        case .failed(let detail): sendStatuses[chat.key] = detail
        case .unknown:
            sendStatuses[chat.key] = "Send unconfirmed. Check the chat before resending."
            unconfirmedTexts[chat.key] = text
        }
        updateComposer()
    }
}

final class PopoverSizer: NSObject {
    private var displayLink: CADisplayLink?
    private weak var popover: NSPopover?
    private var target = NSSize.zero
    private var height: CGFloat = 0
    private var velocity: CGFloat = 0
    private var previousTick: CFTimeInterval?
    var onFrame: ((NSSize) -> Void)?
    func resize(_ popover: NSPopover, to size: NSSize) {
        self.popover = popover
        target = size
        guard popover.isShown, let screen = popover.contentViewController?.view.window?.screen ?? NSScreen.main else {
            stop()
            popover.contentSize = size
            return
        }
        // Cache/thumbnail updates retarget the motion without stopping and easing
        // in again. Repeated requests for the same height do not restart it.
        guard displayLink == nil else { return }
        guard popover.contentSize != size else { return }
        height = popover.contentSize.height
        velocity = 0
        previousTick = nil
        let link = screen.displayLink(target: self, selector: #selector(step(_:)))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 120, preferred: 120)
        displayLink = link
        link.add(to: .main, forMode: .common)
    }

    @objc private func step(_ link: CADisplayLink) {
        guard let popover = popover, popover.isShown else { stop(); return }
        let dt = min(1.0 / 60, max(0, link.targetTimestamp - (previousTick ?? link.targetTimestamp)))
        previousTick = link.targetTimestamp
        // Exact critically damped spring: smooth velocity through new targets,
        // no fixed-rate timer competing with the MacBook display's refresh.
        let omega: CGFloat = 18
        let displacement = height - target.height
        let common = velocity + omega * displacement
        let decay = exp(-omega * dt)
        height = target.height + (displacement + common * dt) * decay
        velocity = (velocity - omega * common * dt) * decay
        let finished = abs(height - target.height) < 0.2 && abs(velocity) < 2
        let next = finished ? target : NSSize(width: target.width, height: height.rounded())
        if popover.contentSize != next {
            popover.contentSize = next
            onFrame?(popover.contentSize)
        }
        if finished { stop() }
    }

    private func stop() { displayLink?.invalidate(); displayLink = nil }
    deinit { displayLink?.invalidate() }
}

final class MenuBar: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    let reader: Reader
    let diagnosticPath: String
    private var item: NSStatusItem!
    private var timer: DispatchSourceTimer?
    private var previous = ""
    private let cacheQueue = DispatchQueue(label: "nchat.cache-reader", qos: .utility)
    let content = UnreadPopoverController()
    let popover = NSPopover()
    private var previewRequests: [String: UUID] = [:]
    private var imageRequests: [String: Set<String>] = [:]
    private let sizer = PopoverSizer()
    private var outsideClickMonitor: Any?
    private var localClickMonitor: Any?

    init(root: String, diagnosticPath: String) {
        reader = Reader(root: root)
        self.diagnosticPath = diagnosticPath
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Stable AppKit identity; seed near the right edge only on first run.
        let positionKey = "NSStatusItem Preferred Position nchatUnread"
        if UserDefaults.standard.object(forKey: positionKey) == nil {
            UserDefaults.standard.set(0, forKey: positionKey)
        }
        // Tahoe hosts status items through Control Center, which has its own
        // visibility preference. Seed both the implicit and stable identities.
        for name in ["Item-0", "nchatUnread"] {
            let key = "NSStatusItem VisibleCC \(name)"
            if UserDefaults.standard.object(forKey: key) == nil {
                UserDefaults.standard.set(true, forKey: key)
            }
        }
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.autosaveName = "nchatUnread"
        item.button?.title = "nchat …"
        item.button?.target = self
        item.button?.action = #selector(togglePopover)
        popover.behavior = .transient
        popover.animates = false
        popover.delegate = self
        popover.contentViewController = content
        content.onResize = { [weak self] size in
            if let self = self { self.sizer.resize(self.popover, to: size) }
            self?.recordUIState()
        }
        content.onToggle = { [weak self] chat in self?.toggleChat(chat) }
        content.onClose = { [weak self] in self?.popover.performClose(nil) }
        content.onCollapse = { [weak self] in self?.collapseChats() }
        content.onQuit = { NSApplication.shared.terminate(nil) }
        content.onSend = { [weak self] chat, reference, text in self?.sendReply(chat, reference: reference, text: text) }
        content.onClear = { [weak self] chat in self?.chatAction(chat, markRead: false) }
        content.onMarkRead = { [weak self] chat in self?.chatAction(chat, markRead: true) }
        if let index = CommandLine.arguments.firstIndex(of: "--demo") {
            let service = index + 1 < CommandLine.arguments.count ? CommandLine.arguments[index + 1] : "both"
            render(demoSnapshot(service))
            item.button?.title = "Test \(content.state.total)"
            return
        }
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
        let signature = state.title + lines.joined(separator: "\n") + state.chats.map { "\($0.cachePath):\($0.id):\($0.latest):\($0.revision)" }.joined(separator: "\n")
        if signature == previous { return }
        previous = signature
        item.button?.title = state.syncOffline ? "–" : (state.error == nil ? "\(state.total)" : "?")
        let symbol = state.syncOffline || state.error != nil ? "exclamationmark.bubble" : (state.total > 0 ? "bubble.left.fill" : "bubble.left")
        item.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "nchat unread messages")
        item.button?.image?.isTemplate = true
        item.button?.imagePosition = .imageLeading
        item.button?.toolTip = lines.joined(separator: "\n")
        item.button?.setAccessibilityLabel(state.title + ". " + lines.joined(separator: ". "))
        let oldChats = content.state.chats
        content.state = state
        let keys = Set(state.chats.map { $0.key })
        content.expanded.formIntersection(keys)
        content.previews = content.previews.filter { keys.contains($0.key) }
        content.errors = content.errors.filter { keys.contains($0.key) }
        previewRequests = previewRequests.filter { keys.contains($0.key) }
        content.rebuild()
        for chat in state.chats where content.expanded.contains(chat.key) {
            if oldChats.first(where: { $0.key == chat.key }) != chat { loadPreview(chat) }
        }
        // Diagnostic records the same title/rows assigned to the native widget.
        if let data = try? JSONEncoder().encode(state) {
            try? data.write(to: URL(fileURLWithPath: diagnosticPath), options: [.atomic])
            try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: diagnosticPath)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self = self else { return }
            let placement: [String: Any] = ["visible": self.item.isVisible,
                "window": self.item.button?.window.map { NSStringFromRect($0.frame) } ?? "none",
                "screens": NSScreen.screens.map { NSStringFromRect($0.frame) },
                "title": self.item.button?.title ?? "none"]
            if let data = try? JSONSerialization.data(withJSONObject: placement) {
                try? data.write(to: URL(fileURLWithPath: self.diagnosticPath + ".placement"), options: .atomic)
            }
        }
    }

    @objc func togglePopover() {
        if popover.isShown {
            popover.performClose(nil)
            return
        }
        guard let button = item.button else { return }
        content.maxHeight = min(760, max(200, (button.window?.screen?.visibleFrame.height ?? 800) - 60))
        content.rebuild()
        NSApplication.shared.activate(ignoringOtherApps: true)
        popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        content.view.window?.makeKey()
        for chat in content.state.chats where content.expanded.contains(chat.key) { loadPreview(chat) }
    }

    func toggleChat(_ chat: UnreadChat) {
        if content.expanded.contains(chat.key) {
            imageRequests.removeValue(forKey: chat.key)
            content.expanded.remove(chat.key)
            previewRequests.removeValue(forKey: chat.key)
            content.previews.removeValue(forKey: chat.key)
        } else {
            content.expanded.insert(chat.key)
            content.errors.removeValue(forKey: chat.key)
            loadPreview(chat)
        }
        content.rebuild()
        // Preserve the clicked header, not the old bottom edge, after AppKit's resize pass.
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.content.revealChat(chat)
        }
    }

    func collapseChats() {
        imageRequests.removeAll()
        if content.replyChat != nil { content.closeComposer() }
        previewRequests.removeAll()
        content.expanded.removeAll()
        content.previews.removeAll()
        content.errors.removeAll()
        content.rebuild()
        content.scroll.contentView.scroll(to: .zero)
    }

    func loadPreview(_ chat: UnreadChat) {
        if CommandLine.arguments.contains("--demo") {
            content.previews[chat.key] = Array(demoMessages().prefix(chat.count))
            content.rebuild()
            return
        }
        let request = UUID()
        previewRequests[chat.key] = request
        cacheQueue.async { [weak self] in
            let messages: [PreviewMessage]?
            let issue: String?
            do {
                messages = try self?.reader.preview(chat)
                issue = nil
            } catch {
                messages = nil
                issue = "Cache unavailable. Collapse and expand to retry."
            }
            DispatchQueue.main.async {
                guard let self = self, self.previewRequests[chat.key] == request,
                      self.content.expanded.contains(chat.key) else { return }
                var prepared = messages
                if prepared != nil {
                    for index in prepared!.indices where prepared![index].needsDownload && !prepared![index].isViewOnce {
                        let id = prepared![index].id
                        if !(self.imageRequests[chat.key] ?? []).contains(id), (self.imageRequests[chat.key]?.count ?? 0) < 20,
                           WidgetReplyBridge.queueImage(cachePath: chat.cachePath, chatID: chat.id, messageID: id) {
                            self.imageRequests[chat.key, default: []].insert(id)
                        }
                        prepared![index].attachmentStatus = (self.imageRequests[chat.key] ?? []).contains(id) ?
                            "Photo requested — waiting for nchat to cache it" : "Photo not cached — keep nchat running and reopen this preview"
                    }
                }
                self.content.previews[chat.key] = prepared
                self.content.errors[chat.key] = issue
                self.content.rebuild()
            }
        }
    }

    func chatAction(_ chat: UnreadChat, markRead: Bool) {
        guard !content.markingRead.contains(chat.key) else { return }
        if CommandLine.arguments.contains("--demo") {
            content.actionErrors[chat.key] = "Demo only — no read receipts sent."
            content.rebuild()
            return
        }
        content.actionErrors[chat.key] = nil
        if markRead { content.markingRead.insert(chat.key) }
        content.rebuild()
        if !markRead {
            cacheQueue.async { [weak self] in
                guard let self = self else { return }
                do {
                    try self.reader.clear(chat)
                    let state = self.reader.snapshot()
                    DispatchQueue.main.async { self.render(state) }
                } catch {
                    DispatchQueue.main.async {
                        self.content.actionErrors[chat.key] = "Couldn't save the dismissal. Please retry."
                        self.content.rebuild()
                    }
                }
            }
            return
        }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let outcome = WidgetReplyBridge.markRead(cachePath: chat.cachePath, chatID: chat.id, messageIDs: chat.messageIDs)
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.content.markingRead.remove(chat.key)
                switch outcome {
                case .accepted: break // Wait for confirmed cache updates; never fake a read.
                case .failed(let error): self.content.actionErrors[chat.key] = error
                case .unknown: self.content.actionErrors[chat.key] = "Read status not confirmed. Check the app before retrying."
                }
                self.content.rebuild()
            }
        }
    }

    func sendReply(_ chat: UnreadChat, reference: String, text: String) {
        guard !content.sending.contains(chat.key) else { return }
        if CommandLine.arguments.contains("--demo") {
            content.sendStatuses[chat.key] = "Test mode — nothing sent."
            content.updateComposer()
            return
        }
        content.sending.insert(chat.key)
        content.sendStatuses[chat.key] = "Submitting reply…"
        content.updateComposer()
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let outcome = WidgetReplyBridge.submit(cachePath: chat.cachePath, chatID: chat.id, referenceID: reference, text: text)
            DispatchQueue.main.async { self?.content.completeReply(chat, text: text, outcome: outcome) }
        }
    }

    func popoverDidShow(_ notification: Notification) {
        stopDismissMonitors()
        // Mouse-only monitors exist solely while this popover is open. They do
        // not inspect text, save input, or require Computer Control permission.
        let clicks: NSEvent.EventTypeMask = [.leftMouseDown, .rightMouseDown, .otherMouseDown]
        outsideClickMonitor = NSEvent.addGlobalMonitorForEvents(matching: clicks) { [weak self] _ in
            self?.popover.performClose(nil)
        }
        localClickMonitor = NSEvent.addLocalMonitorForEvents(matching: clicks) { [weak self] event in
            guard let self = self else { return event }
            if event.window !== self.content.view.window && event.window !== self.item.button?.window {
                self.popover.performClose(nil)
            }
            return event
        }
        recordUIState()
    }

    func popoverDidClose(_ notification: Notification) {
        stopDismissMonitors()
        collapseChats() // Every dismissal reopens at the chat list; closeComposer preserves drafts.
        recordUIState()
    }

    func applicationDidResignActive(_ notification: Notification) { popover.performClose(nil) }

    func stopDismissMonitors() {
        if let monitor = outsideClickMonitor { NSEvent.removeMonitor(monitor) }
        if let monitor = localClickMonitor { NSEvent.removeMonitor(monitor) }
        outsideClickMonitor = nil
        localClickMonitor = nil
    }

    func recordUIState() {
        // Only geometry/counts, never message text or image contents.
        let state: [String: Any] = ["popoverShown": popover.isShown,
            "expandedChats": content.expanded.count,
            "previewMessages": content.previews.values.reduce(0) { $0 + $1.count },
            "inlineImages": content.previews.values.flatMap { $0 }.filter { $0.image != nil }.count,
            "width": content.targetContentSize.width, "height": content.targetContentSize.height,
            "documentHeight": content.document.frame.height,
            "viewportHeight": content.scroll.contentView.bounds.height,
            "scrollY": content.scroll.contentView.bounds.origin.y,
            "backButtonVisible": !content.backButton.isHidden]
        if let data = try? JSONSerialization.data(withJSONObject: state) {
            try? data.write(to: URL(fileURLWithPath: diagnosticPath + ".ui"), options: .atomic)
            try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: diagnosticPath + ".ui")
        }
    }
}

func selfTest() throws {
    func expect(_ value: @autoclosure () throws -> Bool) rethrows {
        let result = try value()
        precondition(result)
    }
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let nativePath = root.appendingPathComponent("ChatStorage.sqlite").path
    var native: OpaquePointer?
    assert(sqlite3_open(nativePath, &native) == SQLITE_OK)
    defer { sqlite3_close(native) }
    func nativeSQL(_ value: String) {
        precondition(sqlite3_exec(native, value, nil, nil, nil) == SQLITE_OK)
    }
    nativeSQL("CREATE TABLE ZWACHATSESSION(ZCONTACTJID TEXT,ZUNREADCOUNT INT,ZLASTMESSAGEDATE REAL);")
    nativeSQL("INSERT INTO ZWACHATSESSION VALUES('wa-chat',0,1);")
    let waCandidate = UnreadChat(id: "wa-chat", name: "Test", service: "WhatsApp", count: 1,
        latest: 978_307_201_000, cachePath: "test")
    expect(whatsappChatsConfirmedRead([waCandidate], databasePath: nativePath) == [waCandidate.key])
    nativeSQL("UPDATE ZWACHATSESSION SET ZUNREADCOUNT=1;")
    expect(whatsappChatsConfirmedRead([waCandidate], databasePath: nativePath).isEmpty)
    nativeSQL("UPDATE ZWACHATSESSION SET ZUNREADCOUNT=0,ZLASTMESSAGEDATE=0;")
    expect(whatsappChatsConfirmedRead([waCandidate], databasePath: nativePath).isEmpty)
    expect(whatsappChatsConfirmedRead([waCandidate], databasePath: nativePath + ".missing").isEmpty)
    print("PASS: native WhatsApp zero suppresses only caught-up chats; unread, stale and absent databases keep alerts")
    var db: OpaquePointer?
    let profile = root.appendingPathComponent("history/Telegram_fixture")
    try FileManager.default.createDirectory(at: profile, withIntermediateDirectories: true)
    let path = profile.appendingPathComponent("db.sqlite").path
    assert(sqlite3_open(path, &db) == SQLITE_OK)
    defer { sqlite3_close(db) }
    func sql(_ value: String) {
        precondition(sqlite3_exec(db, value, nil, nil, nil) == SQLITE_OK, String(cString: sqlite3_errmsg(db)))
    }
    sql("CREATE TABLE messages(chatId TEXT, id TEXT, senderId TEXT, fileInfo TEXT, text TEXT, isOutgoing INT DEFAULT 0, isRead INT DEFAULT 0, isDeleted INT DEFAULT 0, timeSent INT);")
    sql("CREATE TABLE chats2(id TEXT, isArchived INT DEFAULT 0, isMuted INT DEFAULT 0); CREATE TABLE contacts2(id TEXT, name TEXT);")
    sql("INSERT INTO chats2(id) VALUES('one'),('two'),('events'),('archive'),('status@broadcast');")
    sql("UPDATE chats2 SET isArchived=1 WHERE id='archive'; INSERT INTO contacts2 VALUES('one','Friends 🐢');")
    sql("INSERT INTO messages(chatId,id,text,timeSent) VALUES('one','old','orphan',1),('one','read','read',2),('one','new','photo',3),('two','new','hello',4),('archive','new','hidden',5),('status@broadcast','x','hidden',6);")
    sql("UPDATE messages SET isRead=1 WHERE id='read';")
    for (i, event) in ["[Joined]", "[Left]", "[Added Alice]", "[Removed Bob]"].enumerated() {
        sql("INSERT INTO messages(chatId,id,text,timeSent) VALUES('events','\(i)','\(event)',10);")
    }
    let cache = try Cache(path: path, service: "Telegram")
    // Reading a later message in another forum topic must not hide this one.
    try expect(cache.preview(chatID: "one").count == 2)
    try expect(Snapshot(chats: cache.rows()).total == 3)
    let whatsappCache = try Cache(path: path, service: "WhatsApp")
    try expect(whatsappCache.preview(chatID: "one").count == 1)
    sql("UPDATE messages SET isRead=1 WHERE id='old';")
    print("PASS: Telegram topic reads preserve older unread messages; WhatsApp progress fallback unchanged")
    var rows = try cache.rows()
    assert(rows.count == 2 && rows[0].id == "two" && rows[1].name == "Friends 🐢")
    assert(Snapshot(chats: rows).total == 2)
    sql("CREATE TABLE chat_membership(chatId TEXT PRIMARY KEY,isMember INTEGER NOT NULL);")
    let bytesBeforeMembershipRead = try Data(contentsOf: URL(fileURLWithPath: path))
    try expect(cache.rows().count == 2) // Unknown membership is not a confirmed leave.
    try expect(Data(contentsOf: URL(fileURLWithPath: path)) == bytesBeforeMembershipRead)
    sql("INSERT INTO chat_membership VALUES('one',0);")
    try expect(cache.rows().map { $0.id } == ["two"])
    try expect(cache.preview(chatID: "one").isEmpty)
    let afterRestart = try Cache(path: path, service: "Telegram")
    try expect(afterRestart.rows().map { $0.id } == ["two"])
    sql("INSERT INTO messages(chatId,id,text,timeSent) VALUES('one','left-channel-post','new cached post',50);")
    try expect(cache.rows().map { $0.id } == ["two"])
    sql("UPDATE chats2 SET isMuted=0,isArchived=0 WHERE id='one';")
    try expect(cache.preview(chatID: "one").isEmpty)
    sql("UPDATE chat_membership SET isMember=1 WHERE chatId='one';")
    try expect(cache.preview(chatID: "one").count == 2) // Rejoining restores eligibility.
    sql("DELETE FROM messages WHERE id='left-channel-post';")
    print("PASS: left channels excluded from count/list/preview after restart and new cached posts; metadata refresh preserves leave; rejoin restores eligibility")
    let dismissReader = Reader(root: root.appendingPathComponent("history").path, checkBackend: false)
    let selected = dismissReader.snapshot().chats.first { $0.id == "one" }!
    let cacheBeforeClear = try Data(contentsOf: URL(fileURLWithPath: path))
    try dismissReader.clear(selected)
    expect(dismissReader.snapshot().chats.map { $0.id } == ["two"])
    try expect(dismissReader.preview(selected).isEmpty)
    try expect(Data(contentsOf: URL(fileURLWithPath: path)) == cacheBeforeClear)
    let reopened = Reader(root: root.appendingPathComponent("history").path, checkBackend: false)
    expect(reopened.snapshot().chats.map { $0.id } == ["two"])
    sql("INSERT INTO messages(chatId,id,text,timeSent) VALUES('one','same-time-new','new arrival',3);")
    expect(reopened.snapshot().chats.first { $0.id == "one" }?.count == 1)
    try expect(reopened.preview(selected).map { $0.id } == ["same-time-new"])
    sql("DELETE FROM messages WHERE id='same-time-new';")
    try FileManager.default.removeItem(at: root.appendingPathComponent("widget-dismissed.json"))
    print("PASS: Clear persists exact displayed IDs, keeps new arrivals (including equal timestamps), and never changes cache/read flags")
    for service in ["Telegram", "WhatsApp"] {
        let muteCache = try Cache(path: path, service: service)
        try expect(muteCache.rows().count == 2)
        sql("UPDATE chats2 SET isMuted=1 WHERE id='one';")
        let unmutedRows = try muteCache.rows()
        assert(unmutedRows.map { $0.id } == ["two"] && Snapshot(chats: unmutedRows).total == 1)
        try expect(muteCache.preview(chatID: "one").isEmpty)
        sql("UPDATE chats2 SET isMuted=1 WHERE id='two';")
        try expect(muteCache.rows().isEmpty)
        sql("UPDATE chats2 SET isMuted=0 WHERE id IN ('one','two');")
        try expect(muteCache.rows().count == 2)
        try expect(muteCache.preview(chatID: "one").count == 1)
    }
    print("PASS: Telegram/WhatsApp muted chats excluded from list, count and previews; live mute/unmute refreshes without marking read")
    for service in ["Telegram", "WhatsApp"] {
        let missingSettingsCache = try Cache(path: path, service: service)
        sql("DELETE FROM chats2 WHERE id='one';")
        let unknownVisible = service == "Telegram" ? 0 : 1
        try expect(missingSettingsCache.rows().count == 1 + unknownVisible)
        try expect(missingSettingsCache.preview(chatID: "one").count == unknownVisible)
        sql("INSERT INTO chats2(id,isMuted,isArchived) VALUES('one',0,NULL);")
        try expect(missingSettingsCache.rows().count == 1 + unknownVisible)
        try expect(missingSettingsCache.preview(chatID: "one").count == unknownVisible)
        sql("DELETE FROM chats2 WHERE id='one';")
        sql("INSERT INTO chats2(id,isMuted) VALUES('one',1);")
        try expect(missingSettingsCache.rows().map { $0.id } == ["two"])
        try expect(missingSettingsCache.preview(chatID: "one").isEmpty)
        sql("UPDATE chats2 SET isMuted=0,isArchived=1 WHERE id='one';")
        try expect(missingSettingsCache.preview(chatID: "one").isEmpty)
        try expect(missingSettingsCache.rows().map { $0.id } == ["two"])
        sql("UPDATE chats2 SET isArchived=0 WHERE id='one';")
        try expect(missingSettingsCache.rows().count == 2)
    }
    print("PASS: unknown Telegram settings wait for sync; archive/unarchive and mute updates refresh lists and previews; WhatsApp missing-row behavior preserved")
    sql("INSERT INTO contacts2 VALUES('alice','Alice 🐢'); UPDATE messages SET senderId='alice' WHERE chatId='one';")
    let beforePreview = try cache.rows()
    let bytesBeforePreview = try Data(contentsOf: URL(fileURLWithPath: path))
    let messages = try cache.preview(chatID: "one")
    try expect(Data(contentsOf: URL(fileURLWithPath: path)) == bytesBeforePreview)
    assert(messages.count == 1 && messages.first?.time == 3)
    assert(messages.first?.sender == "Alice 🐢")
    try expect(cache.rows() == beforePreview) // Preview must not change unread state.
    try expect(cache.preview(chatID: "one' OR 1=1 --").isEmpty) // Chat IDs are bound, not SQL.
    try expect(cache.preview(chatID: "missing").isEmpty)
    assert(clean("A\nB") == "A B")
    assert(messageDate(1_000).timeIntervalSince1970 == 1)
    try expect(cache.preview(chatID: "events").isEmpty)
    try expect(cache.preview(chatID: "archive").isEmpty)
    try expect(cache.preview(chatID: "status@broadcast").isEmpty)
    sql("INSERT INTO messages(chatId,id,text,isOutgoing,timeSent) VALUES('two','sent','outgoing',1,20);")
    try expect(cache.preview(chatID: "two").count == 1) // No outgoing or already-read messages.
    try expect(cache.rows().map { $0.count } == rows.map { $0.count }) // Sending does not erase incoming unread.
    sql("UPDATE messages SET isRead=1 WHERE chatId='two' AND id='new';")
    rows = try cache.rows(); assert(rows.count == 1 && rows[0].id == "one")
    sql("UPDATE messages SET isDeleted=1 WHERE chatId='one' AND id='new';")
    try expect(cache.preview(chatID: "one").isEmpty)
    try expect(cache.rows().isEmpty)
    sql("INSERT INTO messages(chatId,id,text,timeSent) VALUES('one','ordinary','[Left] literally means left',30);")
    try expect(cache.rows().count == 1) // Only exact service-event forms are suppressed.
    sql("UPDATE messages SET isRead=1 WHERE id='ordinary';")
    try expect(cache.rows().isEmpty)
    assert(menuLines(Snapshot()) == ["No unread messages"])
    assert(Snapshot(error: "busy").title == "nchat ?")
    let offlineReader = Reader(root: root.appendingPathComponent("history").path)
    assert(offlineReader.snapshot().syncOffline)
    assert(offlineReader.snapshot().title == "nchat offline")
    assert(!menuLines(offlineReader.snapshot()).contains("No unread messages"))
    let markerDirectory = root.appendingPathComponent("widget-replies")
    try FileManager.default.createDirectory(at: markerDirectory, withIntermediateDirectories: true)
    let marker = markerDirectory.appendingPathComponent("server").path
    for value in ["invalid", "2147483647 " + String(repeating: "a", count: 32), "\(getpid()) " + String(repeating: "a", count: 32)] {
        try WidgetReplyBridge.writeRequest(value, path: marker)
        assert(offlineReader.snapshot().syncOffline) // Missing/dead/reused PID is not a live nchat.
    }
    assert(!Reader(root: root.appendingPathComponent("history").path, checkBackend: false).snapshot().syncOffline)
    assert(Reader(root: root.appendingPathComponent("missing").path).snapshot().error != nil)
    sql("INSERT INTO chats2(id) VALUES('long'); WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<220) INSERT INTO messages(chatId,id,text,timeSent,isRead) SELECT 'long',printf('%03d',x),'message',x,0 FROM n;")
    let limited = try cache.preview(chatID: "long")
    assert(limited.count == 200 && limited.first?.time == 21 && limited.last?.time == 220)
    let expired = try Cache(path: path, service: "WhatsApp", readTimeout: 0)
    do { _ = try expired.rows(); assertionFailure("Expired query budget must interrupt") }
    catch { assert(String(describing: error) == "interrupted") }
    do { _ = try expired.preview(chatID: "long"); assertionFailure("Expired preview budget must interrupt") }
    catch { assert(String(describing: error) == "interrupted") }
    try expect(cache.preview(chatID: "long").count == 200)
    assert(offlineReader.snapshot().total == 220) // Retain cached unread messages while offline.
    print("PASS: list and preview deadlines still interrupt; subsequent normal reads succeed")
    do {
        // Preparing writes is allowed by SQLite, executing them must be rejected.
        let write = try cache.prepare("UPDATE messages SET isRead=1")
        defer { sqlite3_finalize(write) }
        assert(sqlite3_step(write) == SQLITE_READONLY)
    }
    sql("DROP TABLE messages;")
    do { _ = try cache.rows(); assertionFailure("Missing schema must fail") } catch { }
    do { _ = try cache.preview(chatID: "one"); assertionFailure("Preview schema errors must surface") } catch { }
    // Construct the real native view without activating it or opening a window.
    _ = NSApplication.shared
    let controller = UnreadPopoverController()
    let fixture = demoSnapshot()
    controller.state = fixture
    controller.rebuild()
    let collapsedHeight = controller.targetContentSize.height
    let chat = fixture.chats[1]
    controller.expanded.insert(chat.key)
    controller.previews[chat.key] = demoMessages()
    controller.rebuild()
    controller.view.layoutSubtreeIfNeeded()
    assert(!controller.backButton.isHidden && controller.backButton.superview === controller.header)
    assert(controller.options.itemTitles.contains("Quit nchat Unread"))
    assert(!controller.document.subviews.compactMap { $0 as? NSButton }.contains { $0.title == "Quit nchat Unread" })
    var closedByButton = false
    controller.onClose = { closedByButton = true }
    controller.closeButton.performClick(nil)
    assert(closedByButton)
    assert(controller.view.hitTest(controller.closeButton.convert(NSPoint(x: controller.closeButton.bounds.midX, y: controller.closeButton.bounds.midY), to: controller.view.superview)) === controller.closeButton)
    assert(controller.header.frame.maxY == controller.scroll.frame.minY)
    assert(controller.document.subviews.filter { $0 is MessageCard }.count == demoMessages().count)
    assert(controller.scroll.contentView.bounds.origin.y == 0)
    assert(controller.targetContentSize.height > collapsedHeight)
    let afterText = controller.document.frame.height
    let testImage = NSImage(size: NSSize(width: 600, height: 400))
    controller.previews[chat.key]![2].image = testImage
    controller.rebuild()
    assert(controller.document.frame.height > afterText)
    let photo = controller.document.subviews.compactMap { $0 as? ImageButton }.first!
    assert(abs(photo.frame.width / photo.frame.height - 1.5) < 0.001)
    controller.previews[chat.key]![2].image = NSImage(size: NSSize(width: 400, height: 800))
    controller.rebuild()
    let portrait = controller.document.subviews.compactMap { $0 as? ImageButton }.first!
    assert(abs(portrait.frame.width / portrait.frame.height - 0.5) < 0.001)
    let caption = String(repeating: "A longer caption should wrap above the image, not leave unused space. ", count: 5)
    controller.previews[chat.key] = [PreviewMessage(sender: "Jamie", text: caption, time: 1000, attachment: true, image: testImage)]
    controller.rebuild()
    let captionField = controller.document.subviews.compactMap { $0 as? NSTextField }.first { $0.stringValue == caption }!
    let captionPhoto = controller.document.subviews.compactMap { $0 as? ImageButton }.first!
    assert(captionField.lineBreakMode == .byWordWrapping && captionField.maximumNumberOfLines == 0)
    assert(captionField.isSelectable && !captionField.cell!.truncatesLastVisibleLine)
    assert(captionField.frame.height > 60)
    assert(abs(captionPhoto.frame.minY - captionField.frame.maxY - 8) < 0.001)
    let webLink = "https://example.org/path/to/a/long/link?one=1&two=2"
    let linkedText = "See \(webLink) for details."
    controller.previews[chat.key] = [PreviewMessage(sender: "Jamie", text: linkedText, time: 1000, attachment: false)]
    controller.rebuild()
    let linkedField = controller.document.subviews.compactMap { $0 as? NSTextField }.first { $0.stringValue == linkedText }!
    let linkIndex = (linkedText as NSString).range(of: webLink).location
    assert(linkedField.isSelectable && linkedField.allowsEditingTextAttributes)
    assert((linkedField.attributedStringValue.attribute(.link, at: linkIndex, effectiveRange: nil) as? URL)?.absoluteString == webLink)
    assert((linkedField.attributedStringValue.attribute(.paragraphStyle, at: linkIndex, effectiveRange: nil) as? NSParagraphStyle)?.lineBreakMode == .byWordWrapping)
    controller.previews[chat.key] = (0..<200).map { _ in PreviewMessage(sender: "Long name 🐢", text: String(repeating: "Wrapped message ", count: 40), time: 1000, attachment: false) }
    controller.rebuild()
    assert(controller.targetContentSize.height == controller.maxHeight)
    assert(controller.document.frame.height > controller.maxHeight)
    controller.scroll.contentView.scroll(to: NSPoint(x: 0, y: 1000))
    assert(controller.backButton.frame.maxY < controller.scroll.frame.minY) // Pinned header, outside the scrolling messages.
    assert(controller.view.bounds.contains(controller.backButton.frame))
    let origin = controller.view.bounds.origin
    controller.revealChat(chat)
    assert(controller.view.bounds.origin == origin)
    assert(controller.view.hitTest(controller.backButton.convert(NSPoint(x: controller.backButton.bounds.midX, y: controller.backButton.bounds.midY), to: controller.view.superview)) === controller.backButton)
    var collapsed = false
    controller.onCollapse = { collapsed = true }
    controller.backButton.performClick(nil)
    assert(collapsed)
    collapsed = false
    controller.cancelOperation(nil)
    assert(collapsed) // Escape backs out of expanded messages before closing.
    controller.expanded.removeAll()
    controller.rebuild()
    assert(controller.targetContentSize.height == collapsedHeight)
    assert(controller.backButton.isHidden)
    var clicked = false
    controller.onToggle = { clicked = $0.key == fixture.chats[0].key }
    let button = controller.document.subviews.compactMap { $0 as? ChatButton }.first!
    button.performClick(nil)
    assert(clicked)
    var cleared: String?, marked: String?
    controller.onClear = { cleared = $0.key }
    controller.onMarkRead = { marked = $0.key }
    let actions = controller.document.subviews.compactMap { $0 as? ChatActionButton }
    let clearControl = actions.first { $0.title == "Clear" }!
    let readControl = actions.first { $0.title == "Mark as read" }!
    for control in [clearControl, readControl] {
        assert(controller.document.hitTest(NSPoint(x: control.frame.midX, y: control.frame.midY)) === control)
    }
    clearControl.performClick(nil)
    assert(cleared == fixture.chats[0].key && marked == nil)
    readControl.performClick(nil)
    assert(marked == fixture.chats[0].key)
    controller.markingRead.insert(fixture.chats[0].key)
    controller.rebuild()
    assert(controller.document.subviews.compactMap { $0 as? ChatActionButton }.filter { $0.chat?.key == fixture.chats[0].key }.allSatisfy { !$0.isEnabled })
    controller.markingRead.removeAll()
    print("PASS: native Clear/Mark as read buttons hit correct chat independently; duplicate actions disabled while pending")
    var closed = false
    controller.onClose = { closed = true }
    controller.cancelOperation(nil)
    assert(closed)
    controller.state = Snapshot()
    controller.rebuild()
    assert(controller.document.subviews.compactMap { $0 as? ChatButton }.isEmpty)
    assert(decodedFilePath("0,,zz,xx") == nil)
    assert(decodedFilePath("0,,2f746d702f612e706e67,") == "/tmp/a.png")
    assert(cachedThumbnail("0,,2f746d702f612e706e67,", cacheRoot: root.path) == nil)
    let photoURL = root.appendingPathComponent("orientation.jpg")
    let context = CGContext(data: nil, width: 1200, height: 800, bitsPerComponent: 8, bytesPerRow: 0,
        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    context.setFillColor(NSColor.systemTeal.cgColor)
    context.fill(CGRect(x: 0, y: 0, width: 1200, height: 800))
    let destination = CGImageDestinationCreateWithURL(photoURL as CFURL, "public.jpeg" as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, context.makeImage()!, [kCGImagePropertyOrientation: 6] as CFDictionary)
    assert(CGImageDestinationFinalize(destination))
    func fileInfo(_ path: String) -> String { "0,," + path.utf8.map { String(format: "%02x", $0) }.joined() + "," }
    let thumb = cachedThumbnail(fileInfo(photoURL.path), cacheRoot: root.path)!
    assert(abs(thumb.size.width / thumb.size.height - 2.0 / 3.0) < 0.01) // EXIF rotation respected.
    assert(max(thumb.size.width, thumb.size.height) <= 640)
    assert(cachedThumbnail(fileInfo(photoURL.path), cacheRoot: root.appendingPathComponent("elsewhere").path) == nil)
    let emptyURL = root.appendingPathComponent("broken.jpg")
    try Data("not an image".utf8).write(to: emptyURL)
    assert(cachedThumbnail(fileInfo(emptyURL.path), cacheRoot: root.path) == nil)
    // Cache -> real native button -> original URL, without launching an app or
    // touching message read flags. The source is larger than the 640px thumbnail.
    sql("CREATE TABLE messages(chatId TEXT, id TEXT, senderId TEXT, fileInfo TEXT, text TEXT, isOutgoing INT DEFAULT 0, isRead INT DEFAULT 0, isDeleted INT DEFAULT 0, timeSent INT);")
    sql("INSERT INTO messages(chatId,id,fileInfo,text,timeSent) VALUES('one','photo','\(fileInfo(photoURL.path))','Full image',100);")
    let fullMessages = try cache.preview(chatID: "one")
    assert(fullMessages.count == 1 && fullMessages[0].imageURL == photoURL)
    let hiddenStatus = "Hidden · Ctrl+V in nchat to view"
    let markedInfo = fileInfo(photoURL.path) + ",31\n" // Actual C++ serialization ends with a newline.
    assert(isViewOnce(markedInfo) && !isViewOnce(fileInfo(photoURL.path)))
    assert(!isViewOnce(fileInfo(photoURL.path) + ",30"))
    assert(cachedThumbnail(markedInfo, cacheRoot: root.path) == nil)
    let remoteInfo = "0," + Data("fixture-image-id".utf8).map { String(format: "%02x", $0) }.joined() + "," +
        Data(root.appendingPathComponent("uncached.png").path.utf8).map { String(format: "%02x", $0) }.joined() + "," +
        Data("image/png".utf8).map { String(format: "%02x", $0) }.joined()
    sql("INSERT INTO chats2(id) VALUES('view-once');")
    for (index, fixture) in [(markedInfo, "Flagged photo"), (remoteInfo + ",31", "Uncached flagged photo"),
                              (fileInfo(photoURL.path), "[View once] [Photo]"),
                              (fileInfo(photoURL.path), "[View-once photo]"),
                              (remoteInfo, "Normal uncached photo")].enumerated() {
        sql("INSERT INTO messages(chatId,id,fileInfo,text,timeSent) VALUES('view-once','\(index)','\(fixture.0)','\(fixture.1)',\(index + 200));")
    }
    var protectedMessages = try cache.preview(chatID: "view-once")
    assert(protectedMessages.count == 5)
    for message in protectedMessages.prefix(4) {
        assert(message.isViewOnce && message.text == "View-once photo" && message.attachmentStatus == hiddenStatus)
        assert(message.image == nil && message.imageURL == nil && !message.needsDownload)
    }
    assert(!protectedMessages[4].isViewOnce && protectedMessages[4].needsDownload)
    sql("INSERT INTO messages(chatId,id,fileInfo,text,timeSent) VALUES('view-once','voice','','[View-once voice note]',300);")
    let protectedVoice = try cache.preview(chatID: "view-once").last!
    assert(protectedVoice.isViewOnce && protectedVoice.text == "View-once attachment" &&
        protectedVoice.image == nil && !protectedVoice.needsDownload && protectedVoice.attachmentStatus == hiddenStatus)
    // Rendering also rejects an accidentally retained old thumbnail; there is
    // no image button or alternate reveal action in the widget.
    protectedMessages.removeLast()
    protectedMessages[0].image = thumb
    protectedMessages[0].imageURL = photoURL
    let hiddenController = UnreadPopoverController()
    let protectedChat = UnreadChat(id: "view-once", name: "Protected fixture", service: "Telegram", count: 4,
        latest: 203, cachePath: path)
    hiddenController.state = Snapshot(chats: [protectedChat])
    hiddenController.expanded = [protectedChat.key]
    hiddenController.previews[protectedChat.key] = protectedMessages
    hiddenController.rebuild()
    assert(hiddenController.document.subviews.compactMap { $0 as? ImageButton }.isEmpty)
    assert(hiddenController.document.subviews.compactMap { $0 as? NSTextField }.contains { $0.stringValue == hiddenStatus })
    print("PASS: view-once flags and legacy labels hide cached/uncached thumbnails and automatic requests; widget exposes only Ctrl+V guidance")
    let imageChat = UnreadChat(id: "one", name: "Image test", service: "Telegram", count: 1, latest: 100, cachePath: path)
    controller.state = Snapshot(chats: [imageChat])
    controller.expanded = [imageChat.key]
    controller.previews[imageChat.key] = fullMessages
    controller.rebuild()
    let imageButton = controller.document.subviews.compactMap { $0 as? ImageButton }.first!
    assert(imageButton.isEnabled && imageButton.action == #selector(UnreadPopoverController.openImage(_:)))
    let hit = controller.document.hitTest(NSPoint(x: imageButton.frame.midX, y: imageButton.frame.midY))
    assert(hit === imageButton)
    let originalBytes = try Data(contentsOf: photoURL)
    let databaseBytes = try Data(contentsOf: URL(fileURLWithPath: path))
    var opened: [URL] = []
    var imageErrors = 0
    controller.onOpenImage = { opened.append($0) }
    controller.onImageOpenError = { _ in imageErrors += 1 }
    imageButton.performClick(nil)
    assert(opened == [photoURL.resolvingSymlinksInPath()])
    let fullSource = CGImageSourceCreateWithURL(opened[0] as CFURL, nil)!
    let fullProperties = CGImageSourceCopyPropertiesAtIndex(fullSource, 0, nil)! as NSDictionary
    assert(fullProperties[kCGImagePropertyPixelWidth] as? Int == 1200)
    assert(fullProperties[kCGImagePropertyPixelHeight] as? Int == 800)
    try expect(Data(contentsOf: photoURL) == originalBytes)
    try expect(Data(contentsOf: URL(fileURLWithPath: path)) == databaseBytes)
    assert(NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.Preview") != nil)
    imageButton.imageURL = emptyURL
    imageButton.performClick(nil)
    imageButton.imageURL = root.appendingPathComponent("missing.jpg")
    imageButton.performClick(nil)
    assert(opened.count == 1 && imageErrors == 2)
    let restricted = root.appendingPathComponent("restricted")
    try FileManager.default.createDirectory(at: restricted, withIntermediateDirectories: true)
    let link = restricted.appendingPathComponent("outside.jpg")
    try FileManager.default.createSymbolicLink(at: link, withDestinationURL: photoURL)
    assert(cachedImageURL(link, cacheRoot: restricted.path) == nil)
    assert(cachedImageURL(URL(string: "https://example.com/image.jpg")!, cacheRoot: root.path) == nil)
    print("PASS: native image click opens original 1200×800 file, not thumbnail; cache unchanged; missing/corrupt/outside files rejected; Preview available")
    for service in ["WhatsApp", "Telegram", "both"] {
        controller.state = demoSnapshot(service)
        controller.rebuild()
        assert(controller.document.subviews.compactMap { $0 as? ChatButton }.count == (service == "both" ? 4 : 2))
        assert(controller.state.total == (service == "WhatsApp" ? 4 : service == "Telegram" ? 5 : 9))
        for button in controller.document.subviews.compactMap({ $0 as? ChatButton }) {
            let labels = button.subviews.compactMap { $0 as? NSTextField }
            assert(labels[0].frame.maxY <= labels[1].frame.minY)
            assert(labels.allSatisfy { $0.frame.maxX <= button.bounds.width })
        }
    }
    controller.state = fixture
    controller.expanded = [chat.key]
    controller.previews[chat.key] = demoMessages()
    controller.rebuild()
    let reply = controller.document.subviews.compactMap { $0 as? ChatButton }.first { $0.title == "Reply…" }!
    var sends = 0
    controller.onSend = { selected, reference, text in
        assert(selected.key == chat.key && reference == "demo3" && text.contains("Draft 🐢"))
        sends += 1
        controller.sending.insert(selected.key)
        controller.updateComposer()
    }
    reply.performClick(nil)
    assert(controller.replyChat?.key == chat.key && sends == 0 && !controller.sendButton.isEnabled)
    controller.replyField.string = "Draft 🐢"
    controller.replyField.insertNewline(nil)
    controller.textDidChange(Notification(name: NSText.didChangeNotification))
    assert(sends == 0 && controller.sendButton.isEnabled) // Enter cannot send.
    let draft = controller.replyField.string
    controller.state = Snapshot() // Read on another device during composition.
    controller.rebuild()
    assert(controller.replyField.string == draft && controller.replyChat?.key == chat.key)
    controller.sendButton.performClick(nil)
    controller.sendButton.performClick(nil)
    assert(sends == 1 && !controller.sendButton.isEnabled)
    controller.completeReply(chat, text: draft, outcome: .failed("Test failure"))
    assert(controller.replyField.string == draft && controller.sendButton.isEnabled)
    controller.completeReply(chat, text: draft, outcome: .unknown)
    assert(!controller.sendButton.isEnabled && controller.replyField.string == draft)
    controller.replyField.string = "A different draft"
    controller.textDidChange(Notification(name: NSText.didChangeNotification))
    controller.completeReply(chat, text: draft, outcome: .accepted)
    assert(controller.replyField.string == "A different draft") // A late ack cannot erase newer typing.
    controller.closeComposer()
    assert(controller.drafts[chat.key] == "A different draft")
    let resume = controller.document.subviews.compactMap { $0 as? ChatButton }.first { $0.title.hasPrefix("Resume reply") }!
    resume.performClick(nil)
    assert(controller.replyField.string == "A different draft")
    controller.completeReply(chat, text: "A different draft", outcome: .accepted)
    assert(controller.replyField.string.isEmpty && controller.drafts[chat.key] == "")
    if case .failed = WidgetReplyBridge.submit(cachePath: path, chatID: "test", referenceID: "id", text: " ") {} else {
        assertionFailure("Empty reply must not queue anything")
    }
    assert(isImageReference("30,61,5b50686f746f5d,"))
    assert(!isImageReference("30,61,66696c652e706466,"))
    assert(decodedFilePath("30,61,5b50686f746f5d,") == nil)
    let queue = root.appendingPathComponent("peek-downloads")
    try FileManager.default.createDirectory(at: queue, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    try WidgetReplyBridge.writeRequest(String(getpid()), path: queue.appendingPathComponent("server").path)
    let imageCache = root.appendingPathComponent("history/Telegram_test/db.sqlite").path
    assert(WidgetReplyBridge.queueImage(cachePath: imageCache, chatID: "exact-chat", messageID: "exact-message"))
    let requests = try FileManager.default.contentsOfDirectory(atPath: queue.path).filter { $0.hasSuffix(".request") }
    assert(requests.count == 1)
    let payload = try String(contentsOf: queue.appendingPathComponent(requests[0]), encoding: .utf8)
    let expected = ["Telegram_test", "exact-chat", "exact-message"].map { $0.utf8.map { String(format: "%02x", $0) }.joined() }.joined(separator: "\n") + "\n"
    assert(payload == expected)
    print("PASS: image references distinguished from documents; cache-only download queue uses exact profile/chat/message")
    controller.closeComposer()
    controller.state = fixture
    for height: CGFloat in [240, 420, 760] {
        controller.maxHeight = height
        controller.expanded = [chat.key]
        controller.previews[chat.key] = (0..<200).map { _ in demoMessages()[0] }
        controller.rebuild()
        for fraction: CGFloat in [0, 0.5, 1] {
            controller.scroll.contentView.scroll(to: NSPoint(x: 0, y: fraction * (controller.document.frame.height - controller.scroll.contentView.bounds.height)))
            controller.view.layoutSubtreeIfNeeded()
            assert(controller.view.bounds.contains(controller.backButton.frame))
            assert(controller.backButton.frame.maxY < controller.scroll.frame.minY)
            assert(controller.view.hitTest(controller.backButton.convert(NSPoint(x: controller.backButton.bounds.midX, y: controller.backButton.bounds.midY), to: controller.view.superview)) === controller.backButton)
        }
    }
    let navigation = MenuBar(root: root.path, diagnosticPath: root.appendingPathComponent("test-state").path)
    navigation.content.state = fixture
    navigation.content.expanded = [chat.key]
    navigation.content.previews[chat.key] = demoMessages()
    navigation.content.rebuild()
    navigation.content.beginReply(navigation.content.document.subviews.compactMap { $0 as? ChatButton }.first { $0.title == "Reply…" }!)
    navigation.content.replyField.string = "Keep this draft"
    navigation.content.onCollapse = { navigation.collapseChats() }
    navigation.content.backButton.performClick(nil)
    assert(navigation.content.expanded.isEmpty && navigation.content.replyChat == nil)
    assert(navigation.content.drafts[chat.key] == "Keep this draft")
    assert(navigation.content.scroll.contentView.bounds.origin == .zero)
    navigation.content.expanded = [chat.key]
    navigation.content.previews[chat.key] = demoMessages()
    navigation.content.rebuild()
    navigation.content.beginReply(navigation.content.document.subviews.compactMap { $0 as? ChatButton }.first { $0.title == "Reply…" }!)
    navigation.content.replyField.string = "Draft survives dismissal"
    navigation.popoverDidClose(Notification(name: NSPopover.didCloseNotification, object: navigation.popover))
    navigation.content.rebuild() // The next open rebuilds the same controller.
    assert(navigation.content.expanded.isEmpty && navigation.content.previews.isEmpty)
    assert(navigation.content.replyChat == nil && navigation.content.backButton.isHidden)
    assert(navigation.content.drafts[chat.key] == "Draft survives dismissal")
    assert(navigation.content.scroll.contentView.bounds.origin == .zero)
    print("PASS: popover dismissal resets expansion, previews, composer and scroll while retaining unsent drafts")
    print("PASS: left back button hit-testing at three menu heights and scroll positions; real collapse action restores main list and preserves reply draft")
    print("PASS: unread-only selection, membership events, archives, outgoing/read/deleted exclusions, read progress, live changes, query errors, SQL binding, read-only enforcement; millisecond timestamps, native row clicks, expand/collapse, Escape, long text, bounded scrolling, landscape/portrait aspect ratios and empty state")
    print("PASS: reply recipient, explicit Send only, Enter/newlines, duplicate-click guard, draft retention through refresh/read/close, failure and uncertain-send handling, late acknowledgements and cleared sent drafts")
}

// Local visual fixtures. Never inserted into nchat's database or sent to a chat.
func demoSnapshot(_ service: String = "both") -> Snapshot {
    let now = Int64(Date().timeIntervalSince1970 * 1000)
    let chats = [
        UnreadChat(id: "family", name: "Family", service: "WhatsApp", count: 3, latest: now - 120_000, cachePath: "demo-wa"),
        UnreadChat(id: "study", name: "Study group", service: "Telegram", count: 3, latest: now - 240_000, cachePath: "demo-tg"),
        UnreadChat(id: "sam", name: "Sam", service: "Telegram", count: 2, latest: now - 360_000, cachePath: "demo-tg"),
        UnreadChat(id: "alex", name: "Alex", service: "WhatsApp", count: 1, latest: now - 480_000, cachePath: "demo-wa")
    ]
    return Snapshot(chats: chats.filter { service == "both" || $0.service == service })
}

func demoMessages() -> [PreviewMessage] {
    let now = Int64(Date().timeIntervalSince1970 * 1000)
    return [PreviewMessage(sender: "Jamie", text: "Anyone free for coffee tomorrow?", time: now - 360_000, attachment: false, id: "demo1"),
        PreviewMessage(sender: "Priya", text: "I can do 3!", time: now - 300_000, attachment: false, id: "demo2"),
        PreviewMessage(sender: "Jamie", text: "Great, see you there.", time: now - 240_000, attachment: false, id: "demo3")]
}

func renderUITests(_ directory: String) throws {
    _ = NSApplication.shared
    let root = URL(fileURLWithPath: directory)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let controller = UnreadPopoverController()
    controller.maxHeight = 760
    let appearance = NSAppearance(named: .darkAqua)!
    controller.view.appearance = appearance
    // Rasterize the actual AppKit content view, not a separately drawn mockup.
    func save(_ name: String) throws {
        controller.rebuild()
        controller.view.wantsLayer = true
        controller.view.effectiveAppearance.performAsCurrentDrawingAppearance {
            controller.view.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        }
        let size = controller.view.bounds.size
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * 2), pixelsHigh: Int(size.height * 2),
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        bitmap.size = size
        controller.view.cacheDisplay(in: controller.view.bounds, to: bitmap)
        try bitmap.representation(using: .png, properties: [:])!.write(to: root.appendingPathComponent(name + ".png"))
    }
    for (service, filename) in [("WhatsApp", "1-whatsapp"), ("Telegram", "2-telegram"), ("both", "3-both")] {
        controller.state = demoSnapshot(service)
        try save(filename)
    }
    let chat = controller.state.chats[1]
    controller.expanded.insert(chat.key)
    controller.previews[chat.key] = demoMessages()
    try save("4-expanded-text")
    // Known-aspect fixture tests pixel geometry without reusing private chat photos.
    let image = NSImage(size: NSSize(width: 600, height: 400), flipped: false) { rect in
        NSColor.systemTeal.setFill(); rect.fill()
        NSColor.white.setFill(); NSBezierPath(ovalIn: NSRect(x: 225, y: 125, width: 150, height: 150)).fill()
        return true
    }
    controller.previews[chat.key]![2] = PreviewMessage(sender: "Jamie", text: "Image preview test", time: Int64(Date().timeIntervalSince1970 * 1000), attachment: true, image: image)
    try save("5-expanded-image")
    controller.view.appearance = NSAppearance(named: .aqua)
    try save("6-light-image")
    controller.view.appearance = appearance
    controller.previews[chat.key] = demoMessages()
    controller.rebuild()
    let reply = controller.document.subviews.compactMap { $0 as? ChatButton }.first { $0.title == "Reply…" }!
    reply.performClick(nil)
    controller.replyField.string = "Sounds good, see you there!"
    controller.textDidChange(Notification(name: NSText.didChangeNotification))
    try save("7-reply-composer")
    controller.closeComposer()
    controller.expanded.removeAll()
    controller.state = Snapshot()
    try save("8-empty")
    controller.state.error = "Test error"
    try save("9-error")
    controller.state = Snapshot(syncOffline: true)
    try save("9a-offline-empty")
    controller.state = Snapshot(chats: [chat], syncOffline: true)
    try save("9b-offline-cached")
    controller.drafts.removeAll()
    controller.state = Snapshot(chats: [chat])
    controller.expanded = [chat.key]
    controller.previews[chat.key] = [PreviewMessage(sender: "Jamie", text: "You can sign up for student discounts with your university email. Check the offers throughout the year before buying a new laptop. Some shops run special promotions at the start of term, so it may be worth waiting until you have compared the available prices.", time: Int64(Date().timeIntervalSince1970 * 1000), attachment: true, image: image)]
    try save("10-wrapped-caption-image")
    print("Rendered actual native views: \(directory)")
}

func testPopoverLayout() {
    _ = NSApplication.shared
    NSApplication.shared.setActivationPolicy(.accessory)
    let copyItem = NSApplication.shared.mainMenu?.item(withTitle: "Edit")?.submenu?.item(withTitle: "Copy")
    precondition(copyItem?.keyEquivalent == "c" && copyItem?.action == #selector(NSText.copy(_:)))
    let host = NSWindow(contentRect: NSRect(x: -2000, y: -2000, width: 500, height: 100), styleMask: [.borderless], backing: .buffered, defer: false)
    let anchor = NSView(frame: NSRect(x: 0, y: 0, width: 30, height: 20))
    host.contentView!.addSubview(anchor)
    let controller = UnreadPopoverController()
    let popover = NSPopover()
    popover.animates = false
    popover.contentViewController = controller
    let sizer = PopoverSizer()
    var frames: [NSSize] = []
    var windowHeights: [CGFloat] = []
    sizer.onFrame = {
        frames.append($0)
        windowHeights.append(controller.view.window!.frame.height)
    }
    controller.onResize = { sizer.resize(popover, to: $0) }
    controller.state = demoSnapshot("Telegram")
    controller.rebuild()
    host.alphaValue = 0
    host.orderBack(nil)
    popover.show(relativeTo: anchor.bounds, of: anchor, preferredEdge: .minY)
    controller.view.window?.alphaValue = 0
    func settle(_ label: String) {
        RunLoop.current.run(until: Date().addingTimeInterval(1.2))
        precondition(popover.isShown, "Test must use an actual hosted popover")
        precondition(popover.contentSize == controller.targetContentSize, "Popover window did not resize: \(label)")
        precondition(popover.contentSize.width == 454, "Menu width changed with content: \(label)")
        precondition(controller.view.visibleRect.contains(controller.view.bounds), "Popover clips content: \(label)")
        if !controller.expanded.isEmpty {
            precondition(controller.backButton.visibleRect.contains(controller.backButton.bounds), "Back arrow clipped: \(label)")
            let point = NSPoint(x: controller.backButton.frame.midX, y: controller.backButton.frame.midY)
            precondition(controller.view.hitTest(controller.view.convert(point, to: controller.view.superview)) === controller.backButton)
        }
    }
    settle("collapsed")
    precondition(controller.scroll.frame.minX == 0 && controller.document.frame.width == 454, "Collapsed menu retains an empty gutter")
    let initialHeight = popover.contentSize.height
    let initialWindowHeight = controller.view.window!.frame.height
    let chat = controller.state.chats[0]
    controller.expanded = [chat.key]
    controller.previews[chat.key] = demoMessages()
    controller.rebuild()
    precondition(popover.contentSize.height == initialHeight, "Height snapped before animation")
    Thread.sleep(forTimeInterval: 0.35) // Simulate a busy main loop before its first animation tick.
    settle("expanded")
    controller.view.window?.makeKey()
    let copyField = controller.document.subviews.compactMap { $0 as? NSTextField }.first { $0.stringValue == demoMessages()[0].text }!
    copyField.selectText(nil)
    precondition(controller.view.window?.firstResponder is NSTextView, "Selecting a preview must focus its field editor")
    precondition(frames.contains { $0.height > initialHeight && $0.height < controller.targetContentSize.height }, "Expected intermediate animation frames")
    precondition(frames.allSatisfy { $0.width == 454 })
    let intermediate = windowHeights.filter { $0 > initialWindowHeight && $0 < controller.view.window!.frame.height }
    precondition(intermediate.count >= 10, "Actual window did not animate through enough visible sizes")
    let heightRange = controller.targetContentSize.height - initialHeight
    let actualFrames = [initialWindowHeight] + windowHeights
    precondition(zip(actualFrames, actualFrames.dropFirst()).allSatisfy { abs($1 - $0) <= heightRange * 0.15 + 2 }, "Actual window snapped between frames")
    // Repeated cache refreshes and thumbnail arrivals must not restart motion.
    controller.previews[chat.key] = (0..<30).map { _ in demoMessages()[0] }
    controller.rebuild()
    for delay in [0.03, 0.06, 0.09] {
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            let before = popover.contentSize
            controller.rebuild()
            precondition(popover.contentSize == before, "Refresh jumped instead of retargeting")
        }
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
        let before = popover.contentSize
        controller.previews[chat.key] = demoMessages()
        controller.rebuild()
        precondition(popover.contentSize == before, "Direction change jumped")
    }
    settle("rapid refresh and reversal")
    controller.onCollapse = {
        if controller.replyChat != nil { controller.closeComposer() }
        controller.expanded.removeAll()
        controller.rebuild()
    }
    for height: CGFloat in [240, 420, 760] {
        controller.maxHeight = height
        controller.previews[chat.key] = (0..<200).map { _ in demoMessages()[0] }
        controller.rebuild()
        controller.revealChat(chat)
        settle("long preview at \(height)")
        controller.scroll.contentView.scroll(to: NSPoint(x: 0, y: controller.document.frame.height - controller.scroll.contentView.bounds.height))
        settle("scrolled to bottom at \(height)")
    }
    controller.previews[chat.key] = demoMessages()
    controller.rebuild()
    settle("short preview after long preview")
    let reply = controller.document.subviews.compactMap { $0 as? ChatButton }.first { $0.title == "Reply…" }!
    reply.performClick(nil)
    controller.replyField.string = "Draft preserved"
    settle("reply composer")
    controller.backButton.performClick(nil)
    settle("back to main list")
    precondition(controller.scroll.frame.minX == 0 && controller.document.frame.width == 454)
    precondition(controller.expanded.isEmpty && controller.replyChat == nil)
    precondition(controller.drafts[chat.key] == "Draft preserved")
    popover.close()
    host.orderOut(nil)
    print("PASS: display-synchronised actual-window animation, bounded frame jumps, refresh retargeting/reversal, delayed first frame, fixed width, no collapsed gutter, scrolling, reply and Back")
}

func installCopyMenu() {
    let mainMenu = NSMenu()
    let editItem = NSMenuItem(title: "Edit", action: nil, keyEquivalent: "")
    let editMenu = NSMenu(title: "Edit")
    editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    mainMenu.addItem(editItem)
    mainMenu.setSubmenu(editMenu, for: editItem)
    NSApplication.shared.mainMenu = mainMenu
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
if args.contains("--test-popover-layout") {
    installCopyMenu()
    testPopoverLayout()
} else if args.contains("--self-test") {
    do { try selfTest() } catch { fputs("\(error)\n", stderr); exit(1) }
} else if let index = args.firstIndex(of: "--render-ui-tests"), index + 1 < args.count {
    try renderUITests(args[index + 1])
} else if args.contains("--snapshot") {
    let data = try JSONEncoder().encode(Reader(root: root).snapshot())
    print(String(decoding: data, as: UTF8.self))
} else {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    installCopyMenu()
    let delegate = MenuBar(root: root, diagnosticPath: home + "/.config/nchat/menubar-state.json")
    app.delegate = delegate
    app.run()
}
}
}
