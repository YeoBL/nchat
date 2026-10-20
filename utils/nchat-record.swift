import AppKit
import AVFoundation
import AVKit
import AudioToolbox
import Darwin

var resultURL: URL?

func emitJSON(_ data: Data) throws {
    if let url = resultURL {
        try data.write(to: url, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data("\n".utf8))
}

func fail(_ message: String) -> Never {
    if let data = try? JSONSerialization.data(withJSONObject: ["error": message]) { try? emitJSON(data) }
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

struct Recording: Codable {
    let path: String
    let mediaKind: String
    let durationSec: Double
    let mimeType: String
    let waveform: String?
    let length: Int?
}

func metadata(_ url: URL, video: Bool) throws -> Recording {
    let asset = AVURLAsset(url: url)
    let duration = CMTimeGetSeconds(asset.duration)
    guard duration.isFinite, duration > 0,
          (try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0) > 0 else {
        throw NSError(domain: "nchat-record", code: 1,
                      userInfo: [NSLocalizedDescriptionKey: "The recording is empty. Please record again."])
    }
    var length: Int?
    if video {
        guard let track = asset.tracks(withMediaType: .video).first else {
            throw NSError(domain: "nchat-record", code: 12,
                          userInfo: [NSLocalizedDescriptionKey: "The recording has no video track."])
        }
        let bounds = CGRect(origin: .zero, size: track.naturalSize).applying(track.preferredTransform)
        let width = abs(bounds.width)
        let height = abs(bounds.height)
        guard width.isFinite, height.isFinite, width > 0, abs(width - height) < 0.5 else {
            throw NSError(domain: "nchat-record", code: 13,
                          userInfo: [NSLocalizedDescriptionKey: "Video notes must be square. Please record again."])
        }
        length = Int(width.rounded())
    }
    return Recording(path: url.path, mediaKind: video ? "video" : "voice",
                     durationSec: duration, mimeType: video ? "video/mp4" : "audio/mp4", waveform: nil,
                     length: length)
}

func appendLittleEndian<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
    var value = value.littleEndian
    withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
}

func opusOgg(_ source: URL, to destination: URL) throws {
    // ponytail: CoreAudio encodes Opus into CAF but cannot write Ogg on this Mac.
    // Mux its mono packets without re-encoding; one packet/page keeps framing simple.
    var handle: AudioFileID?
    guard AudioFileOpenURL(source as CFURL, .readPermission, 0, &handle) == noErr, let file = handle else {
        throw NSError(domain: "nchat-record", code: 7, userInfo: [NSLocalizedDescriptionKey: "Could not read the encoded voice note."])
    }
    defer { AudioFileClose(file) }
    var packetCount: UInt64 = 0
    var packetCountSize = UInt32(MemoryLayout<UInt64>.size)
    var packetSize: UInt32 = 0
    var packetSizeSize = UInt32(MemoryLayout<UInt32>.size)
    var table = AudioFilePacketTableInfo()
    var tableSize = UInt32(MemoryLayout<AudioFilePacketTableInfo>.size)
    guard AudioFileGetProperty(file, kAudioFilePropertyAudioDataPacketCount, &packetCountSize, &packetCount) == noErr,
          AudioFileGetProperty(file, kAudioFilePropertyPacketSizeUpperBound, &packetSizeSize, &packetSize) == noErr,
          AudioFileGetProperty(file, kAudioFilePropertyPacketTableInfo, &tableSize, &table) == noErr,
          packetCount > 0, packetSize > 0, packetSize < 65025, table.mNumberValidFrames > 0,
          table.mPrimingFrames >= 0, table.mPrimingFrames <= 65535 else {
        throw NSError(domain: "nchat-record", code: 8, userInfo: [NSLocalizedDescriptionKey: "The voice encoder returned incomplete audio."])
    }
    let serial = UInt32.random(in: 1...UInt32.max)
    var sequence: UInt32 = 0
    var output = Data()
    func page(_ packet: Data, flags: UInt8, granule: UInt64) {
        var header = Data("OggS".utf8)
        header.append(contentsOf: [0, flags])
        appendLittleEndian(granule, to: &header)
        appendLittleEndian(serial, to: &header)
        appendLittleEndian(sequence, to: &header)
        appendLittleEndian(UInt32(0), to: &header)
        var lacing = [UInt8](repeating: 255, count: packet.count / 255)
        lacing.append(UInt8(packet.count % 255))
        header.append(UInt8(lacing.count))
        header.append(contentsOf: lacing)
        header.append(packet)
        var crc: UInt32 = 0
        for byte in header {
            crc ^= UInt32(byte) << 24
            for _ in 0..<8 { crc = (crc & 0x80000000 != 0) ? (crc << 1) ^ 0x04c11db7 : crc << 1 }
        }
        var checksum = Data()
        appendLittleEndian(crc, to: &checksum)
        header.replaceSubrange(22..<26, with: checksum)
        output.append(header)
        sequence += 1
    }
    var head = Data("OpusHead".utf8)
    head.append(contentsOf: [1, 1])
    appendLittleEndian(UInt16(table.mPrimingFrames), to: &head)
    appendLittleEndian(UInt32(48000), to: &head)
    head.append(contentsOf: [0, 0, 0])
    page(head, flags: 2, granule: 0)
    var tags = Data("OpusTags".utf8)
    appendLittleEndian(UInt32(5), to: &tags)
    tags.append(Data("nchat".utf8))
    appendLittleEndian(UInt32(0), to: &tags)
    page(tags, flags: 0, granule: 0)
    var frames: UInt64 = 0
    for index in 0..<packetCount {
        var packet = Data(count: Int(packetSize))
        var bytes = packetSize
        var count: UInt32 = 1
        var description = AudioStreamPacketDescription()
        let result = packet.withUnsafeMutableBytes {
            AudioFileReadPacketData(file, false, &bytes, &description, Int64(index), &count, $0.baseAddress!)
        }
        guard result == noErr, count == 1 else {
            throw NSError(domain: "nchat-record", code: 9, userInfo: [NSLocalizedDescriptionKey: "Could not read a voice-note packet."])
        }
        packet.count = Int(bytes)
        frames += UInt64(description.mVariableFramesInPacket == 0 ? 960 : description.mVariableFramesInPacket)
        let last = index == packetCount - 1
        let end = UInt64(table.mNumberValidFrames + Int64(table.mPrimingFrames))
        page(packet, flags: last ? 4 : 0, granule: last ? min(frames, end) : frames)
    }
    try output.write(to: destination, options: .atomic)
}

func voiceRecording(_ source: URL, runConverter: (Process) throws -> Void = { try $0.run() }) throws -> Recording {
    let info = try metadata(source, video: false)
    let audio = try AVAudioFile(forReading: source)
    let buffer = AVAudioPCMBuffer(pcmFormat: audio.processingFormat, frameCapacity: 4096)!
    var peaks = [Float](repeating: 0, count: 64)
    var offset: AVAudioFramePosition = 0
    while offset < audio.length {
        try audio.read(into: buffer)
        guard buffer.frameLength > 0 else { break }
        if let samples = buffer.floatChannelData {
            for frame in 0..<Int(buffer.frameLength) {
                let bin = min(63, Int((offset + AVAudioFramePosition(frame)) * 64 / max(1, audio.length)))
                peaks[bin] = max(peaks[bin], abs(samples[0][frame]))
            }
        }
        offset += AVAudioFramePosition(buffer.frameLength)
    }
    let highest = peaks.max() ?? 0
    let waveform = Data(peaks.map { highest > 0 ? UInt8(min(100, max(0, $0 / highest * 100))) : 0 })
    let destination = source.deletingPathExtension().appendingPathExtension("ogg")
    let intermediate = source.deletingPathExtension().appendingPathExtension("caf")
    defer { try? FileManager.default.removeItem(at: intermediate) }
    let converter = Process()
    converter.executableURL = URL(fileURLWithPath: "/usr/bin/afconvert")
    converter.arguments = [source.path, intermediate.path, "-f", "caff", "-d", "opus@48000#960", "-c", "1", "-b", "32000"]
    converter.standardOutput = FileHandle.standardError
    try runConverter(converter)
    converter.waitUntilExit()
    guard converter.terminationStatus == 0 else {
        try? FileManager.default.removeItem(at: destination)
        throw NSError(domain: "nchat-record", code: 6,
                      userInfo: [NSLocalizedDescriptionKey: "Could not prepare the voice note for sending."])
    }
    try opusOgg(intermediate, to: destination)
    try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: destination.path)
    return Recording(path: destination.path, mediaKind: "voice", durationSec: info.durationSec,
                     mimeType: "audio/ogg", waveform: waveform.base64EncodedString(), length: nil)
}

func emit(_ recording: Recording) throws {
    try emitJSON(JSONEncoder().encode(recording))
}

func squareVideoExporter(_ raw: URL, to destination: URL) throws -> AVAssetExportSession {
    let asset = AVURLAsset(url: raw)
    guard let track = asset.tracks(withMediaType: .video).first,
          let exporter = AVAssetExportSession(asset: asset, presetName: AVAssetExportPresetHighestQuality) else {
        throw NSError(domain: "nchat-record", code: 10, userInfo: [NSLocalizedDescriptionKey: "Could not prepare video playback."])
    }
    let sourceRect = CGRect(origin: .zero, size: track.naturalSize).applying(track.preferredTransform)
    let side = min(abs(sourceRect.width), abs(sourceRect.height))
    guard side > 0 else {
        throw NSError(domain: "nchat-record", code: 11, userInfo: [NSLocalizedDescriptionKey: "The video is empty. Please record again."])
    }
    let composition = AVMutableVideoComposition()
    composition.renderSize = CGSize(width: side, height: side)
    composition.frameDuration = CMTime(value: 1, timescale: 30)
    let instruction = AVMutableVideoCompositionInstruction()
    instruction.timeRange = CMTimeRange(start: .zero, duration: asset.duration)
    let layer = AVMutableVideoCompositionLayerInstruction(assetTrack: track)
    let offsetX = -sourceRect.minX - (abs(sourceRect.width) - side) / 2
    let offsetY = -sourceRect.minY - (abs(sourceRect.height) - side) / 2
    layer.setTransform(track.preferredTransform.concatenating(CGAffineTransform(translationX: offsetX, y: offsetY)), at: .zero)
    instruction.layerInstructions = [layer]
    composition.instructions = [instruction]
    exporter.videoComposition = composition
    exporter.outputURL = destination
    exporter.outputFileType = .mp4
    exporter.shouldOptimizeForNetworkUse = true
    return exporter
}

func testVideo(_ directory: URL) throws {
    let raw = directory.appendingPathComponent("landscape.mov")
    let writer = try AVAssetWriter(outputURL: raw, fileType: .mov)
    let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
        AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: 640, AVVideoHeightKey: 480
    ])
    let adapter = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input,
        sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32ARGB])
    writer.add(input)
    precondition(writer.startWriting())
    writer.startSession(atSourceTime: .zero)
    var pixel: CVPixelBuffer?
    precondition(CVPixelBufferCreate(kCFAllocatorDefault, 640, 480, kCVPixelFormatType_32ARGB, nil, &pixel) == kCVReturnSuccess)
    CVPixelBufferLockBaseAddress(pixel!, [])
    memset(CVPixelBufferGetBaseAddress(pixel!), 0, CVPixelBufferGetDataSize(pixel!))
    CVPixelBufferUnlockBaseAddress(pixel!, [])
    let deadline = Date().addingTimeInterval(10)
    for frame in 0..<8 {
        while !input.isReadyForMoreMediaData && Date() < deadline { Thread.sleep(forTimeInterval: 0.01) }
        precondition(adapter.append(pixel!, withPresentationTime: CMTime(value: Int64(frame), timescale: 30)))
    }
    input.markAsFinished()
    let finished = DispatchSemaphore(value: 0)
    writer.finishWriting { finished.signal() }
    precondition(finished.wait(timeout: .now() + 10) == .success && writer.status == .completed)
    let final = directory.appendingPathComponent("square.mp4")
    let exporter = try squareVideoExporter(raw, to: final)
    exporter.exportAsynchronously { finished.signal() }
    precondition(finished.wait(timeout: .now() + 20) == .success && exporter.status == .completed)
    let track = AVURLAsset(url: final).tracks(withMediaType: .video).first!
    precondition(track.naturalSize == CGSize(width: 480, height: 480))
    let description = track.formatDescriptions.first as! CMFormatDescription
    precondition(CMFormatDescriptionGetMediaSubType(description) == kCMVideoCodecType_H264)
    let info = try metadata(final, video: true)
    precondition(info.mediaKind == "video" && info.mimeType == "video/mp4" && info.durationSec > 0.2)
    precondition(info.length == 480)
}

let arguments = Array(CommandLine.arguments.dropFirst())
if arguments == ["--help"] {
    print("Usage: nchat-record --voice|--video [--output-dir DIRECTORY] [--result FILE]\n       nchat-record --self-test")
    exit(0)
}
if arguments == ["--self-test"] {
    // Synthetic silence tests the actual AAC output and metadata path without accessing devices.
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    do {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("silence.m4a")
        var file: AVAudioFile? = try AVAudioFile(forWriting: url, settings: [
            AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 44100,
            AVNumberOfChannelsKey: 1, AVEncoderBitRateKey: 64000
        ])
        let buffer = AVAudioPCMBuffer(pcmFormat: file!.processingFormat, frameCapacity: 11025)!
        buffer.frameLength = 11025
        memset(buffer.floatChannelData![0], 0, Int(buffer.frameLength) * MemoryLayout<Float>.size)
        try file!.write(from: buffer)
        file = nil
        let recording = try voiceRecording(url)
        precondition(recording.durationSec >= 0.24 && recording.durationSec < 0.4)
        precondition(recording.mediaKind == "voice" && recording.mimeType == "audio/ogg")
        precondition(recording.length == nil)
        precondition(Data(base64Encoded: recording.waveform!) == Data(repeating: 0, count: 64))
        let encoded = try Data(contentsOf: URL(fileURLWithPath: recording.path))
        precondition(encoded.starts(with: Data("OggS".utf8)))
        let decoded = directory.appendingPathComponent("decoded.wav")
        let decoder = Process()
        decoder.executableURL = URL(fileURLWithPath: "/usr/bin/afconvert")
        decoder.arguments = [recording.path, decoded.path, "-f", "WAVE", "-d", "LEI16@48000"]
        try decoder.run()
        decoder.waitUntilExit()
        precondition(decoder.terminationStatus == 0)
        let decodedAudio = try AVAudioFile(forReading: decoded)
        precondition(abs(Double(decodedAudio.length) / decodedAudio.fileFormat.sampleRate - recording.durationSec) < 0.03)
        try testVideo(directory)
        try emit(recording)
        exit(0)
    } catch { fail("Recorder self-test failed: \(error.localizedDescription)") }
}
var mode: String?
var outputPath: String?
var argumentIndex = 0
while argumentIndex < arguments.count {
    let argument = arguments[argumentIndex]
    switch argument {
    case "--voice", "--video":
        guard mode == nil else { fail("Choose either --voice or --video.") }
        mode = argument
    case "--output-dir", "--result":
        argumentIndex += 1
        guard argumentIndex < arguments.count else { fail("Missing value for \(argument).") }
        if argument == "--result" { resultURL = URL(fileURLWithPath: arguments[argumentIndex]) }
        else { outputPath = arguments[argumentIndex] }
    default: fail("Unknown recorder argument: \(argument)")
    }
    argumentIndex += 1
}
guard mode != nil else { fail("Usage: nchat-record --voice|--video [--output-dir DIRECTORY] [--result FILE]") }
let isVideo = mode == "--video"
let outputDirectory = outputPath != nil
    ? URL(fileURLWithPath: outputPath!, isDirectory: true)
    : FileManager.default.temporaryDirectory.appendingPathComponent("nchat-recordings", isDirectory: true)
do {
    try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true,
                                          attributes: [.posixPermissions: 0o700])
} catch { fail("Cannot create recording folder: \(error.localizedDescription)") }

final class Recorder: NSObject, NSApplicationDelegate, NSWindowDelegate,
                      AVAudioRecorderDelegate, AVCaptureFileOutputRecordingDelegate {
    private var window: NSWindow!
    private let status = NSTextField(labelWithString: "")
    private let clock = NSTextField(labelWithString: "00:00")
    private let recordButton = NSButton(title: "Record", target: nil, action: nil)
    private let useButton = NSButton(title: "Use recording", target: nil, action: nil)
    private let playerView = AVPlayerView()
    private var audio: AVAudioRecorder?
    private var capture: AVCaptureSession?
    private var movie: AVCaptureMovieFileOutput?
    private var preview: AVCaptureVideoPreviewLayer?
    private var timer: Timer?
    private var startTime: Date?
    private var recordingURL: URL?
    private var rawURL: URL?
    private var recording = false
    private var preparing = false
    private let captureQueue = DispatchQueue(label: "io.nchat.record.capture")
    private let voiceQueue = DispatchQueue(label: "io.nchat.record.voice")
    private let work = DispatchGroup()
    private let stateLock = NSLock()
    private var didCancel = false
    private var voiceConverter: Process?
    private var exporter: AVAssetExportSession?
    private var signals = [DispatchSourceSignal]()

    private var cancelled: Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return didCancel
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        for number in [SIGINT, SIGTERM] {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
            source.setEventHandler { self.cancelRecording() }
            source.resume()
            signals.append(source)
        }
        NSApp.setActivationPolicy(.accessory)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: isVideo ? 570 : 310),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = isVideo ? "Record a video note" : "Record a voice note"
        window.delegate = self
        window.isReleasedWhenClosed = false
        let content = NSStackView()
        content.orientation = .vertical
        content.alignment = .centerX
        content.spacing = 18
        content.edgeInsets = NSEdgeInsets(top: 24, left: 24, bottom: 24, right: 24)
        content.translatesAutoresizingMaskIntoConstraints = false
        window.contentView!.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor),
            content.topAnchor.constraint(equalTo: window.contentView!.topAnchor),
            content.bottomAnchor.constraint(equalTo: window.contentView!.bottomAnchor)
        ])
        status.stringValue = "Click Record when you’re ready."
        status.alignment = .center
        status.maximumNumberOfLines = 3
        status.lineBreakMode = .byWordWrapping
        status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        content.addArrangedSubview(status)
        status.widthAnchor.constraint(equalToConstant: 410).isActive = true
        clock.font = .monospacedDigitSystemFont(ofSize: 30, weight: .medium)
        content.addArrangedSubview(clock)
        playerView.controlsStyle = .inline
        playerView.wantsLayer = true
        content.addArrangedSubview(playerView)
        playerView.widthAnchor.constraint(equalToConstant: isVideo ? 300 : 410).isActive = true
        playerView.heightAnchor.constraint(equalToConstant: isVideo ? 300 : 56).isActive = true
        let buttons = NSStackView()
        buttons.orientation = .horizontal
        buttons.spacing = 12
        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancelRecording))
        cancel.keyEquivalent = "\u{1b}"
        recordButton.target = self
        recordButton.action = #selector(toggleRecording)
        useButton.target = self
        useButton.action = #selector(useRecording)
        useButton.isEnabled = false
        [cancel, recordButton, useButton].forEach { buttons.addArrangedSubview($0) }
        content.addArrangedSubview(buttons)
        let help = NSTextField(labelWithString: isVideo
            ? "Video notes stop at 60 seconds. Review before sending."
            : "Review the recording before returning to nchat.")
        help.font = .systemFont(ofSize: 11)
        help.textColor = .secondaryLabelColor
        content.addArrangedSubview(help)
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool { cancelRecording(); return false }

    private func permission(_ media: AVMediaType, completion: @escaping (Bool) -> Void) {
        switch AVCaptureDevice.authorizationStatus(for: media) {
        case .authorized: completion(true)
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: media) { granted in
                DispatchQueue.main.async { completion(granted) }
            }
        default: completion(false)
        }
    }

    @objc private func toggleRecording() {
        if recording { stopRecording(); return }
        guard !preparing else { return }
        preparing = true
        recordButton.isEnabled = false
        useButton.isEnabled = false
        permission(.audio) { granted in
            guard granted else { self.showError("Microphone access is needed. Enable nchat Recorder in System Settings → Privacy & Security → Microphone."); return }
            if isVideo {
                self.permission(.video) { camera in
                    guard camera else { self.showError("Camera access is needed. Enable nchat Recorder in System Settings → Privacy & Security → Camera."); return }
                    self.startRecording()
                }
            } else { self.startRecording() }
        }
    }

    private func startRecording() {
        guard !cancelled else { return }
        clearFiles()
        playerView.player?.pause()
        playerView.player = nil
        clock.stringValue = "00:00"
        let name = "nchat-\(UUID().uuidString)"
        recordingURL = outputDirectory.appendingPathComponent(name + (isVideo ? ".mp4" : ".m4a"))
        if !isVideo {
            do {
                audio = try AVAudioRecorder(url: recordingURL!, settings: [
                    AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 44100,
                    AVNumberOfChannelsKey: 1, AVEncoderBitRateKey: 64000,
                    AVEncoderAudioQualityKey: AVAudioQuality.high.rawValue
                ])
                audio!.delegate = self
                guard audio!.record() else { throw NSError(domain: "nchat-record", code: 2, userInfo: [NSLocalizedDescriptionKey: "Could not start the microphone."]) }
                didStart()
            } catch { showError(error.localizedDescription) }
            return
        }
        status.stringValue = "Starting camera…"
        let raw = outputDirectory.appendingPathComponent(name + ".mov")
        rawURL = raw
        work.enter()
        captureQueue.async {
            defer { self.work.leave() }
            guard !self.cancelled else { return }
            do {
                let session = AVCaptureSession()
                session.beginConfiguration()
                session.sessionPreset = .vga640x480
                for kind in [AVMediaType.video, .audio] {
                    guard let device = AVCaptureDevice.default(for: kind) else {
                        throw NSError(domain: "nchat-record", code: 3, userInfo: [NSLocalizedDescriptionKey: "No \(kind == .video ? "camera" : "microphone") is available."])
                    }
                    let input = try AVCaptureDeviceInput(device: device)
                    guard session.canAddInput(input) else { throw NSError(domain: "nchat-record", code: 4, userInfo: [NSLocalizedDescriptionKey: "Cannot use the recording device."]) }
                    session.addInput(input)
                }
                let output = AVCaptureMovieFileOutput()
                guard session.canAddOutput(output) else { throw NSError(domain: "nchat-record", code: 5, userInfo: [NSLocalizedDescriptionKey: "Cannot create a video recording."]) }
                session.addOutput(output)
                if let connection = output.connection(with: .video) {
                    output.setOutputSettings([AVVideoCodecKey: AVVideoCodecType.h264], for: connection)
                }
                session.commitConfiguration()
                self.capture = session
                self.movie = output
                DispatchQueue.main.async {
                    guard !self.cancelled else { return }
                    let layer = AVCaptureVideoPreviewLayer(session: session)
                    layer.videoGravity = .resizeAspectFill
                    layer.frame = self.playerView.bounds
                    layer.cornerRadius = 12
                    layer.masksToBounds = true
                    self.playerView.layer?.addSublayer(layer)
                    self.preview = layer
                }
                session.startRunning()
                guard !self.cancelled else { session.stopRunning(); return }
                self.work.enter()
                output.startRecording(to: raw, recordingDelegate: self)
            } catch { DispatchQueue.main.async { self.showError(error.localizedDescription) } }
        }
    }

    private func didStart() {
        guard !cancelled else { return }
        preparing = false
        recording = true
        recordButton.isEnabled = true
        recordButton.title = "Stop"
        status.stringValue = "Recording…"
        startTime = Date()
        timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            guard let self = self, let start = self.startTime else { return }
            let seconds = Int(Date().timeIntervalSince(start))
            self.clock.stringValue = String(format: "%02d:%02d", seconds / 60, seconds % 60)
            if isVideo && seconds >= 60 { self.stopRecording() }
        }
    }

    private func stopRecording() {
        guard recording else { return }
        recording = false
        preparing = true
        timer?.invalidate()
        timer = nil
        recordButton.isEnabled = false
        status.stringValue = "Preparing playback…"
        if isVideo {
            captureQueue.async { self.movie?.stopRecording() }
        } else {
            audio?.stop()
            audio = nil
            ready()
        }
    }

    func audioRecorderEncodeErrorDidOccur(_ recorder: AVAudioRecorder, error: Error?) {
        guard !cancelled else { return }
        audio?.stop()
        audio = nil
        showError(error?.localizedDescription ?? "The microphone recording failed. Please record again.")
    }

    func audioRecorderDidFinishRecording(_ recorder: AVAudioRecorder, successfully flag: Bool) {
        guard !cancelled else { return }
        if !flag {
            audio = nil
            showError("The recording was interrupted. Please record again.")
        } else if recording {
            // An external interruption can finish successfully; the Stop path already calls ready().
            recording = false
            timer?.invalidate()
            timer = nil
            audio = nil
            ready()
        }
    }

    func fileOutput(_ output: AVCaptureFileOutput, didStartRecordingTo fileURL: URL,
                    from connections: [AVCaptureConnection]) {
        DispatchQueue.main.async { self.didStart() }
    }

    func fileOutput(_ output: AVCaptureFileOutput, didFinishRecordingTo url: URL,
                    from connections: [AVCaptureConnection], error: Error?) {
        captureQueue.async { self.capture?.stopRunning(); self.capture = nil; self.movie = nil }
        DispatchQueue.main.async {
            defer { self.work.leave() }
            self.preview?.removeFromSuperlayer()
            self.preview = nil
            guard !self.cancelled else { return }
            if let error = error,
               !((error as NSError).userInfo[AVErrorRecordingSuccessfullyFinishedKey] as? Bool ?? false) {
                self.showError(error.localizedDescription)
                return
            }
            self.exportVideo(url)
        }
    }

    private func exportVideo(_ raw: URL) {
        guard let destination = recordingURL else { return }
        let exporter: AVAssetExportSession
        do { exporter = try squareVideoExporter(raw, to: destination) }
        catch { showError(error.localizedDescription); return }
        self.exporter = exporter
        work.enter()
        exporter.exportAsynchronously {
            DispatchQueue.main.async {
                defer { self.work.leave() }
                self.exporter = nil
                guard !self.cancelled else { return }
                if exporter.status == .completed {
                    try? FileManager.default.removeItem(at: raw)
                    self.rawURL = nil
                    self.ready()
                } else { self.showError(exporter.error?.localizedDescription ?? "Could not encode the video.") }
            }
        }
    }

    private func ready() {
        guard let url = recordingURL else { return }
        do {
            let info = try metadata(url, video: isVideo)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
            clock.stringValue = String(format: "%02d:%02d", Int(info.durationSec) / 60, Int(info.durationSec) % 60)
            playerView.player = AVPlayer(url: url)
            status.stringValue = "Play it back, then choose Use recording."
            preparing = false
            recordButton.title = "Record again"
            recordButton.isEnabled = true
            useButton.isEnabled = true
        } catch { showError(error.localizedDescription) }
    }

    private func showError(_ message: String) {
        guard !cancelled else { return }
        preparing = false
        recording = false
        timer?.invalidate()
        timer = nil
        status.stringValue = message
        recordButton.title = "Try again"
        recordButton.isEnabled = true
        useButton.isEnabled = false
    }

    private func clearFiles() {
        var urls = [recordingURL, rawURL].compactMap({ $0 })
        if let url = recordingURL, url.pathExtension == "m4a" {
            urls += ["caf", "ogg"].map { url.deletingPathExtension().appendingPathExtension($0) }
        }
        for url in urls { try? FileManager.default.removeItem(at: url) }
        recordingURL = nil
        rawURL = nil
    }

    @objc private func useRecording() {
        guard !recording, !preparing, let url = recordingURL else { return }
        playerView.player?.pause()
        if isVideo {
            do { try emit(metadata(url, video: true)); exit(0) }
            catch { showError(error.localizedDescription) }
            return
        }
        preparing = true
        recordButton.isEnabled = false
        useButton.isEnabled = false
        status.stringValue = "Preparing voice note…"
        work.enter()
        voiceQueue.async {
            defer { self.work.leave() }
            do {
                let info = try voiceRecording(url) { converter in
                    self.stateLock.lock()
                    defer { self.stateLock.unlock() }
                    guard !self.didCancel else { throw CancellationError() }
                    self.voiceConverter = converter
                    try converter.run()
                }
                DispatchQueue.main.async {
                    guard !self.cancelled else { return }
                    do { try emit(info); try? FileManager.default.removeItem(at: url); exit(0) }
                    catch { self.showError(error.localizedDescription) }
                }
            } catch { DispatchQueue.main.async { self.showError(error.localizedDescription) } }
        }
    }

    @objc private func cancelRecording() {
        stateLock.lock()
        guard !didCancel else { stateLock.unlock(); return }
        didCancel = true
        if voiceConverter?.isRunning == true { voiceConverter?.terminate() }
        stateLock.unlock()
        audio?.stop()
        timer?.invalidate()
        playerView.player?.pause()
        exporter?.cancelExport()
        status.stringValue = "Cancelling…"
        recordButton.isEnabled = false
        useButton.isEnabled = false
        captureQueue.async {
            if self.movie?.isRecording == true { self.movie?.stopRecording() }
            self.capture?.stopRunning()
        }
        work.notify(queue: .main) {
            self.clearFiles()
            if let data = try? JSONSerialization.data(withJSONObject: ["cancelled": true]) { try? emitJSON(data) }
            exit(2)
        }
    }
}

let app = NSApplication.shared
let recorder = Recorder()
app.delegate = recorder
app.run()
