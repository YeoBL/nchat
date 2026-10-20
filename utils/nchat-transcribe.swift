import Foundation
import Speech
import Darwin

struct Options {
    var input = ""
    var output: String?
    var result: String?
    var locale = Locale.current.identifier
    var json = false
    var selfTest = false
    var interactive: Bool { output == nil && result == nil && !json && !selfTest }

    init(_ arguments: [String]) throws {
        var index = 0
        while index < arguments.count {
            let argument = arguments[index]
            switch argument {
            case "--output", "--result", "--locale":
                index += 1
                guard index < arguments.count else { throw Failure("Missing value for \(argument).") }
                if argument == "--output" { output = arguments[index] }
                else if argument == "--result" { result = arguments[index] }
                else { locale = arguments[index] }
            case "--json": json = true
            case "--self-test": selfTest = true
            default:
                guard !argument.hasPrefix("--"), input.isEmpty else { throw Failure("Usage: nchat-transcribe [--output FILE] [--result JSON_FILE] [--json] [--locale LANGUAGE] AUDIO") }
                input = argument
            }
            index += 1
        }
        guard selfTest || !input.isEmpty else { throw Failure("Select an audio message first.") }
    }
}

struct Failure: Error, CustomStringConvertible {
    let description: String
    init(_ message: String) { description = message }
}

var interrupted = false
func wait(until predicate: () -> Bool, seconds: TimeInterval) -> Bool {
    let deadline = Date().addingTimeInterval(seconds)
    while !predicate() && !interrupted && Date() < deadline {
        _ = RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
    }
    return predicate() && !interrupted
}

func convert(_ input: URL, to output: URL) throws {
    let converter = Process()
    converter.executableURL = URL(fileURLWithPath: "/usr/bin/afconvert")
    converter.arguments = [input.path, output.path, "-f", "WAVE", "-d", "LEI16@16000"]
    converter.standardError = FileHandle.nullDevice
    try converter.run()
    while converter.isRunning && !interrupted {
        _ = RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
    }
    if interrupted {
        converter.terminate()
        converter.waitUntilExit()
        throw Failure("Transcription cancelled.")
    }
    converter.waitUntilExit()
    guard converter.terminationStatus == 0 else { throw Failure("This audio format could not be converted for transcription.") }
}

func transcribe(_ options: Options) throws -> String {
    let input = URL(fileURLWithPath: options.input)
    guard FileManager.default.fileExists(atPath: input.path) else { throw Failure("Audio file is unavailable.") }
    guard let recognizer = SFSpeechRecognizer(locale: Locale(identifier: options.locale)),
          recognizer.supportsOnDeviceRecognition else {
        throw Failure("On-device speech recognition is unavailable for this Mac or language.")
    }

    let wav = FileManager.default.temporaryDirectory.appendingPathComponent("nchat-\(UUID().uuidString).wav")
    defer { try? FileManager.default.removeItem(at: wav) }
    try convert(input, to: wav)

    var authorization: SFSpeechRecognizerAuthorizationStatus?
    SFSpeechRecognizer.requestAuthorization { status in
        DispatchQueue.main.async { authorization = status }
    }
    guard wait(until: { authorization != nil }, seconds: 180), authorization == .authorized else {
        throw Failure(interrupted ? "Transcription cancelled." : "Speech Recognition permission is required. Check System Settings → Privacy & Security → Speech Recognition.")
    }

    if options.interactive { print("Transcribing on this Mac…"); fflush(stdout) }
    let request = SFSpeechURLRecognitionRequest(url: wav)
    request.requiresOnDeviceRecognition = true
    request.shouldReportPartialResults = false
    var transcript: String?
    var recognitionError: Error?
    let task = recognizer.recognitionTask(with: request) { result, error in
        DispatchQueue.main.async {
            if let result = result, result.isFinal { transcript = result.bestTranscription.formattedString }
            if let error = error { recognitionError = error }
        }
    }
    defer { task.cancel() }
    guard wait(until: { transcript != nil || recognitionError != nil }, seconds: 180),
          let text = transcript?.trimmingCharacters(in: .whitespacesAndNewlines), !text.isEmpty else {
        throw Failure("Transcription failed: \(interrupted ? "cancelled" : recognitionError?.localizedDescription ?? "no speech detected or timed out")")
    }
    return text
}

func writeResult(_ text: String, options: Options) throws {
    if let path = options.output {
        try (text + "\n").write(to: URL(fileURLWithPath: path), atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: path)
    }
    if options.json || options.result != nil {
        let result = ["text": text, "language": options.locale, "sourcePath": options.input]
        let data = try JSONSerialization.data(withJSONObject: result, options: [.sortedKeys])
        if let path = options.result {
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: path)
        }
        FileHandle.standardOutput.write(data + Data([10]))
    } else {
        print(text)
    }
}

func selfTest() throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("nchat-transcribe-test-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    // Exercise arguments and output independently of Speech permissions or private audio.
    let result = directory.appendingPathComponent("transcript.txt")
    let options = try Options(["--output", result.path, "--locale", "en_GB", "test audio.m4a"])
    guard !options.interactive, options.locale == "en_GB", options.input == "test audio.m4a" else { throw Failure("Argument test failed.") }
    try writeResult("A complete\nmultiline transcript.", options: options)
    let saved = try String(contentsOf: result, encoding: .utf8)
    guard saved == "A complete\nmultiline transcript.\n" else { throw Failure("Transcript persistence test failed.") }
    let jsonPath = directory.appendingPathComponent("result.json")
    let jsonOptions = try Options(["--result", jsonPath.path, "audio.ogg"])
    try writeResult("Quoted \"text\" and\na new line", options: jsonOptions)
    let json = try JSONSerialization.jsonObject(with: Data(contentsOf: jsonPath)) as? [String: String]
    guard json?["text"] == "Quoted \"text\" and\na new line", !jsonOptions.interactive else { throw Failure("JSON result test failed.") }
    // A one-second PCM fixture also exercises the real conversion path without Speech access.
    var wave = Data()
    func word(_ value: UInt16) { var little = value.littleEndian; withUnsafeBytes(of: &little) { wave.append(contentsOf: $0) } }
    func long(_ value: UInt32) { var little = value.littleEndian; withUnsafeBytes(of: &little) { wave.append(contentsOf: $0) } }
    wave.append(Data("RIFF".utf8)); long(32036); wave.append(Data("WAVEfmt ".utf8))
    long(16); word(1); word(1); long(16000); long(32000); word(2); word(16)
    wave.append(Data("data".utf8)); long(32000); wave.append(Data(repeating: 0, count: 32000))
    let source = directory.appendingPathComponent("silence.wav")
    let converted = directory.appendingPathComponent("converted.wav")
    try wave.write(to: source)
    try convert(source, to: converted)
    guard (try Data(contentsOf: converted)).count >= 32044 else { throw Failure("Audio conversion test failed.") }
    print("Transcription helper self-test passed (synthetic audio; no permissions accessed).")
}

var interactive = false
var status: Int32 = 0
var currentOptions: Options?
signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
let signalSources = [SIGINT, SIGTERM].map { value -> DispatchSourceSignal in
    let source = DispatchSource.makeSignalSource(signal: value, queue: .main)
    source.setEventHandler { interrupted = true }
    source.resume()
    return source
}
do {
    let options = try Options(Array(CommandLine.arguments.dropFirst()))
    currentOptions = options
    interactive = options.interactive
    if options.selfTest { try selfTest() } else { try writeResult(transcribe(options), options: options) }
} catch {
    let message = (error as? Failure)?.description ?? error.localizedDescription
    FileHandle.standardError.write(Data((message + "\n").utf8))
    status = interrupted ? 130 : 1
    if let path = currentOptions?.result,
       let data = try? JSONSerialization.data(withJSONObject: ["error": message, "cancelled": interrupted], options: [.sortedKeys]) {
        try? data.write(to: URL(fileURLWithPath: path), options: .atomic)
        try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: path)
    }
}
for source in signalSources { source.cancel() }
if interactive && !interrupted {
    signal(SIGINT, SIG_DFL)
    signal(SIGTERM, SIG_DFL)
    print("\nPress Return to return to nchat.")
    _ = readLine()
}
exit(status)
