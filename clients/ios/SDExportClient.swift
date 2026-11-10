import Foundation
import CryptoKit
import NetworkExtension
import UIKit

public enum SDExportClientError: Error {
    case unsupportedDevice, bleDisconnected, bleTimeout, servicesChanged
    case invalidConnectionInfo, invalidManifest(String), unsafePath(String)
    case http(Int), invalidResponse, checksum(String), device(UInt16)
    case alreadyRunning, foregroundRequired
    case partialDeletion(deletedPaths: [String], reason: String)
}

/// This value contains session credentials. Keep it in memory; never log it.
public struct SDExportConnectionInfo: Decodable, Sendable {
    public let formatVersion: Int
    public let sessionID: String
    public let ssid: String
    public let passphrase: String
    public let baseURL: URL
    public let token: String
    enum CodingKeys: String, CodingKey {
        case formatVersion = "format_version", sessionID = "session_id"
        case ssid, passphrase, baseURL = "base_url", token
    }

    func validate(expectedSessionID: UInt64) throws {
        guard formatVersion == 1, sessionID == String(format: "%016llX", expectedSessionID),
              expectedSessionID != 0, (1...32).contains(ssid.utf8.count),
              (8...63).contains(passphrase.utf8.count),
              passphrase.utf8.allSatisfy({ (32...126).contains($0) }),
              baseURL.scheme == "http", baseURL.host == "192.168.4.1",
              baseURL.port == nil || baseURL.port == 80,
              baseURL.user == nil, baseURL.password == nil,
              baseURL.query == nil, baseURL.fragment == nil,
              baseURL.path.isEmpty || baseURL.path == "/",
              token.count >= 32, token.count <= 128,
              token.utf8.allSatisfy({ (33...126).contains($0) }) else {
            throw SDExportClientError.invalidConnectionInfo
        }
    }
}

public struct SDExportProgress: Sendable {
    public enum Phase: String, Sendable { case scanning, checking, downloading, completed }
    public let phase: Phase
    public let path: String
    public let bytes: UInt64
    public let total: UInt64
}

public struct SDExportResult: Sendable {
    /// Raw SD files are mirrored below this directory. The manifest is authoritative.
    public let filesDirectory: URL
    public let manifestURL: URL
    public let fileCount: UInt64
    public let totalBytes: UInt64
    /// False means the data was acknowledged, but final audio restoration could
    /// not be confirmed over BLE. Do not display "recording resumed" in that case.
    public let deviceRestored: Bool
}

public struct SDFileDeletionResult: Sendable {
    public let deletedPaths: [String]
    public let deviceRestored: Bool
}

public struct SDExportManifestFile: Codable, Equatable, Sendable {
    public let path: String
    public let size: UInt64
    public let sha256: String
    public let etag: String
}

struct SDExportManifestEnd: Decodable, Sendable {
    let fileCount: UInt64
    let totalBytes: UInt64
    let manifestSHA256: String
    enum CodingKeys: String, CodingKey {
        case fileCount = "file_count", totalBytes = "total_bytes", manifestSHA256 = "manifest_sha256"
    }
}

struct SDExportManifest: Sendable {
    let files: [SDExportManifestFile]
    let end: SDExportManifestEnd
}

/// Parses exact UTF-8 NDJSON bytes. Only file lines, including their LF, enter
/// the digest; header, progress and end lines do not. No whole-file bytes live here.
struct SDExportManifestParser {
    private var headerSeen = false
    private var ended: SDExportManifestEnd?
    private var hasher = SHA256()
    private var files: [SDExportManifestFile] = []
    private var paths = Set<String>()
    private var total: UInt64 = 0
    let sessionID: String

    mutating func consume(_ line: Data) throws -> SDExportProgress? {
        struct Kind: Decodable { let type: String }
        guard line.last == 10, line.count <= 16_384, ended == nil else {
            throw SDExportClientError.invalidManifest("unterminated/oversized line or data after end")
        }
        let kind = try JSONDecoder().decode(Kind.self, from: line)
        switch kind.type {
        case "header":
            struct Header: Decodable { let format_version: Int; let session_id: String }
            let header = try JSONDecoder().decode(Header.self, from: line)
            guard !headerSeen, files.isEmpty, header.format_version == 1, header.session_id == sessionID else {
                throw SDExportClientError.invalidManifest("wrong/duplicate header")
            }
            headerSeen = true
        case "progress":
            struct Progress: Decodable { let path: String; let bytes_hashed: UInt64 }
            guard headerSeen else { throw SDExportClientError.invalidManifest("missing header") }
            let progress = try JSONDecoder().decode(Progress.self, from: line)
            return SDExportProgress(phase: .scanning, path: progress.path, bytes: progress.bytes_hashed, total: 0)
        case "file":
            guard headerSeen else { throw SDExportClientError.invalidManifest("missing header") }
            let file = try JSONDecoder().decode(SDExportManifestFile.self, from: line)
            try SDExportPaths.validate(file.path)
            let collisionKey = file.path.precomposedStringWithCanonicalMapping.lowercased()
            guard paths.insert(collisionKey).inserted, Self.isSHA256(file.sha256),
                  file.etag.count >= 2, file.etag.count <= 128,
                  file.etag.first == "\"", file.etag.last == "\"",
                  file.etag.utf8.allSatisfy({ (33...126).contains($0) }) else {
                throw SDExportClientError.invalidManifest("duplicate path, invalid SHA-256 or ETag")
            }
            let (sum, overflow) = total.addingReportingOverflow(file.size)
            guard !overflow else { throw SDExportClientError.invalidManifest("size overflow") }
            total = sum
            hasher.update(data: line)
            files.append(file)
        case "end":
            guard headerSeen else { throw SDExportClientError.invalidManifest("missing header") }
            let end = try JSONDecoder().decode(SDExportManifestEnd.self, from: line)
            guard end.fileCount == UInt64(files.count), end.totalBytes == total,
                  Self.isSHA256(end.manifestSHA256),
                  Self.hex(hasher.finalize()) == end.manifestSHA256 else {
                throw SDExportClientError.invalidManifest("terminal count/size/digest mismatch")
            }
            ended = end
        default: throw SDExportClientError.invalidManifest("unknown record type")
        }
        return nil
    }

    func finish() throws -> SDExportManifest {
        guard let ended else { throw SDExportClientError.invalidManifest("missing end record") }
        return SDExportManifest(files: files, end: ended)
    }

    static func hex<S: Sequence>(_ bytes: S) -> String where S.Element == UInt8 {
        bytes.map { String(format: "%02x", $0) }.joined()
    }
    static func isSHA256(_ value: String) -> Bool {
        value.utf8.count == 64 && value.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) })
    }
}

enum SDExportPaths {
    static func validate(_ path: String) throws {
        let components = path.split(separator: "/", omittingEmptySubsequences: false)
        guard !path.isEmpty, path.utf8.count <= 1024,
              !path.contains("\\"), !path.contains(":"),
              !path.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }),
              components.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }) else {
            throw SDExportClientError.unsafePath(path)
        }
    }

    static func file(_ path: String, below root: URL) throws -> URL {
        try validate(path)
        let base = root.resolvingSymlinksInPath().standardizedFileURL
        let target = base.appendingPathComponent(path).standardizedFileURL
        guard target.resolvingSymlinksInPath().path.hasPrefix(base.path + "/") else {
            throw SDExportClientError.unsafePath(path)
        }
        return target
    }
}

/// HTTP/file work runs outside the main actor. Partial data and content identity
/// are persisted separately from raw SD names, so an App restart can resume.
actor SDExportHTTPClient {
    private let info: SDExportConnectionInfo
    private let session: URLSession
    private let controlSession: URLSession
    private let fm = FileManager.default

    init(info: SDExportConnectionInfo) {
        self.info = info
        let config = URLSessionConfiguration.ephemeral
        config.waitsForConnectivity = true
        config.allowsCellularAccess = false
        config.httpMaximumConnectionsPerHost = 2
        config.timeoutIntervalForRequest = 30
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        config.httpShouldSetCookies = false
        config.timeoutIntervalForResource = 15
        controlSession = URLSession(configuration: config, delegate: SDExportRedirectPolicy(), delegateQueue: nil)
        // A long file may take hours, but loss of connectivity should return to
        // the journal/retry path rather than wait a day for another network.
        config.waitsForConnectivity = false
        config.timeoutIntervalForResource = 86_400
        session = URLSession(configuration: config, delegate: SDExportRedirectPolicy(), delegateQueue: nil)
    }

    func invalidate() { session.invalidateAndCancel(); controlSession.invalidateAndCancel() }

    func deleteFiles(paths: [String]) async throws -> [String] {
        for path in paths { try SDExportPaths.validate(path) }
        guard Set(paths).count == paths.count else { throw SDExportClientError.unsafePath("duplicate paths") }
        let temporary = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".ndjson")
        defer { try? fm.removeItem(at: temporary) }
        let manifest = try await fetchManifest(to: temporary, progress: { _ in })
        let byPath = Dictionary(uniqueKeysWithValues: manifest.files.map { ($0.path, $0) })
        guard paths.allSatisfy({ byPath[$0] != nil }) else { throw SDExportClientError.unsafePath("file not in current manifest") }
        var deleted: [String] = []
        do {
            for path in paths {
                try Task.checkCancellation()
                guard let file = byPath[path] else { throw SDExportClientError.invalidResponse }
                var deletion = request("file", query: [URLQueryItem(name: "path", value: path)])
                deletion.httpMethod = "DELETE"
                deletion.setValue(file.etag, forHTTPHeaderField: "If-Match")
                let (data, response) = try await controlSession.data(for: deletion)
                try Self.requireHTTP(response, code: 200)
                struct Receipt: Decodable { let deleted: Bool; let path: String; let manifest_invalidated: Bool }
                let receipt = try JSONDecoder().decode(Receipt.self, from: data)
                guard receipt.deleted, receipt.path == path, receipt.manifest_invalidated else {
                    throw SDExportClientError.invalidResponse
                }
                deleted.append(path)
            }
        } catch {
            // A lost HTTP response leaves that last file's outcome uncertain.
            // Never claim atomic deletion or silently delete additional files.
            throw SDExportClientError.partialDeletion(deletedPaths: deleted, reason: String(describing: error))
        }
        return deleted
    }

    func keepAlive() async throws {
        let (data, response) = try await controlSession.data(for: request("status"))
        try Self.requireHTTP(response, code: 200)
        struct Status: Decodable { let format_version: Int; let session_id: String; let state: UInt8; let error: UInt16 }
        let status = try JSONDecoder().decode(Status.self, from: data)
        guard status.format_version == 1, status.session_id == info.sessionID else {
            throw SDExportClientError.invalidResponse
        }
        if status.state == SDExportState.failed.rawValue { throw SDExportClientError.device(status.error) }
    }

    func download(to directory: URL, progress: @escaping @Sendable (SDExportProgress) -> Void) async throws
        -> (manifest: SDExportManifest, manifestURL: URL, filesDirectory: URL) {
        let destination = directory.resolvingSymlinksInPath().standardizedFileURL
        let filesDirectory = destination.appendingPathComponent("files", isDirectory: true)
        let stateDirectory = destination.appendingPathComponent(".sd-export", isDirectory: true)
        try fm.createDirectory(at: filesDirectory, withIntermediateDirectories: true)
        try fm.createDirectory(at: stateDirectory, withIntermediateDirectories: true)
        guard filesDirectory.resolvingSymlinksInPath().path == filesDirectory.path,
              stateDirectory.resolvingSymlinksInPath().path == stateDirectory.path else {
            throw SDExportClientError.unsafePath("export directory contains a symlink")
        }
        let manifestURL = stateDirectory.appendingPathComponent("manifest.ndjson")
        let temporaryManifest = stateDirectory.appendingPathComponent("manifest.downloading")
        let manifest = try await fetchManifest(to: temporaryManifest, progress: progress)
        try replace(temporaryManifest, with: manifestURL)
        for file in manifest.files {
            try Task.checkCancellation()
            let target = try SDExportPaths.file(file.path, below: filesDirectory)
            progress(SDExportProgress(phase: .checking, path: file.path, bytes: 0, total: file.size))
            if fm.fileExists(atPath: target.path), try size(target) == file.size,
               try await sha256(target) == file.sha256 {
                progress(SDExportProgress(phase: .completed, path: file.path, bytes: file.size, total: file.size))
                continue
            }
            try await fetchFile(file, target: target, stateDirectory: stateDirectory, progress: progress)
        }
        return (manifest, manifestURL, filesDirectory)
    }

    func complete(_ manifest: SDExportManifest) async throws {
        struct Completion: Encodable {
            let session_id: String; let manifest_sha256: String; let file_count: UInt64; let total_bytes: UInt64
        }
        var completion = request("complete")
        completion.httpMethod = "POST"
        completion.setValue("application/json", forHTTPHeaderField: "Content-Type")
        completion.httpBody = try JSONEncoder().encode(Completion(
            session_id: info.sessionID, manifest_sha256: manifest.end.manifestSHA256,
            file_count: manifest.end.fileCount, total_bytes: manifest.end.totalBytes))
        let (data, response) = try await controlSession.data(for: completion)
        try Self.requireHTTP(response, code: 200)
        struct Acknowledgement: Decodable { let accepted: Bool; let session_id: String }
        let ack = try JSONDecoder().decode(Acknowledgement.self, from: data)
        guard ack.accepted, ack.session_id == info.sessionID else { throw SDExportClientError.invalidResponse }
    }

    private func fetchManifest(to target: URL, progress: @escaping @Sendable (SDExportProgress) -> Void) async throws -> SDExportManifest {
        _ = fm.createFile(atPath: target.path, contents: nil)
        let output = try FileHandle(forWritingTo: target)
        try output.truncate(atOffset: 0)
        defer { try? output.close() }
        let (bytes, response) = try await session.bytes(for: request("manifest"))
        try Self.requireHTTP(response, code: 200)
        var parser = SDExportManifestParser(sessionID: info.sessionID)
        var line = Data()
        for try await byte in bytes {
            try Task.checkCancellation()
            line.append(byte)
            guard line.count <= 16_384 else { throw SDExportClientError.invalidManifest("line too long") }
            if byte == 10 {
                if let update = try parser.consume(line) { progress(update) }
                try output.write(contentsOf: line)
                line.removeAll(keepingCapacity: true)
            }
        }
        guard line.isEmpty else { throw SDExportClientError.invalidManifest("truncated final line") }
        let manifest = try parser.finish()
        try output.synchronize()
        return manifest
    }

    private func fetchFile(_ file: SDExportManifestFile, target: URL, stateDirectory: URL,
                           progress: @escaping @Sendable (SDExportProgress) -> Void) async throws {
        struct Journal: Codable { let path: String; let size: UInt64; let sha256: String }
        let key = SDExportManifestParser.hex(SHA256.hash(data: Data(file.path.utf8)))
        let partial = stateDirectory.appendingPathComponent(key + ".part")
        let journalURL = stateDirectory.appendingPathComponent(key + ".json")
        let previous = try? JSONDecoder().decode(Journal.self, from: Data(contentsOf: journalURL))
        if previous?.path != file.path || previous?.size != file.size || previous?.sha256 != file.sha256 {
            if fm.fileExists(atPath: partial.path) { try fm.removeItem(at: partial) }
        }
        try JSONEncoder().encode(Journal(path: file.path, size: file.size, sha256: file.sha256))
            .write(to: journalURL, options: .atomic)
        if !fm.fileExists(atPath: partial.path) { _ = fm.createFile(atPath: partial.path, contents: nil) }

        for attempt in 0..<3 {
            try Task.checkCancellation()
            do {
                let output = try FileHandle(forWritingTo: partial)
                defer { try? output.close() }
                var offset = try size(partial)
                var resetPartial = offset > file.size
                if offset == file.size { resetPartial = try await sha256(partial) != file.sha256 }
                if resetPartial {
                    try output.truncate(atOffset: 0); offset = 0
                }
                if offset < file.size {
                    var download = request("file", query: [URLQueryItem(name: "path", value: file.path)])
                    if offset != 0 {
                        download.setValue("bytes=\(offset)-", forHTTPHeaderField: "Range")
                        download.setValue(file.etag, forHTTPHeaderField: "If-Range")
                    }
                    let (bytes, response) = try await session.bytes(for: download)
                    guard let response = response as? HTTPURLResponse else { throw SDExportClientError.invalidResponse }
                    guard response.statusCode == 200 || response.statusCode == 206 else {
                        throw SDExportClientError.http(response.statusCode)
                    }
                    guard response.value(forHTTPHeaderField: "ETag") == file.etag,
                          response.value(forHTTPHeaderField: "Content-Encoding") == nil ||
                            response.value(forHTTPHeaderField: "Content-Encoding") == "identity" else {
                        throw SDExportClientError.invalidResponse
                    }
                    if response.statusCode == 200 {
                        try output.truncate(atOffset: 0); offset = 0
                    } else {
                        let expected = "bytes \(offset)-\(file.size - 1)/\(file.size)"
                        guard response.value(forHTTPHeaderField: "Content-Range") == expected else {
                            throw SDExportClientError.invalidResponse
                        }
                    }
                    let remaining = file.size - offset
                    if response.expectedContentLength >= 0, UInt64(response.expectedContentLength) != remaining {
                        throw SDExportClientError.invalidResponse
                    }
                    try output.seek(toOffset: offset)
                    var buffer = Data(); buffer.reserveCapacity(65_536)
                    var received: UInt64 = 0
                    for try await byte in bytes {
                        buffer.append(byte); received += 1
                        guard received <= remaining else { throw SDExportClientError.invalidResponse }
                        if buffer.count == 65_536 {
                            try Task.checkCancellation()
                            try output.write(contentsOf: buffer)
                            buffer.removeAll(keepingCapacity: true)
                            progress(SDExportProgress(phase: .downloading, path: file.path, bytes: offset + received, total: file.size))
                        }
                    }
                    try Task.checkCancellation()
                    if !buffer.isEmpty { try output.write(contentsOf: buffer) }
                    guard received == remaining else { throw SDExportClientError.invalidResponse }
                    try output.synchronize()
                }
                guard try await sha256(partial) == file.sha256 else {
                    // A full but corrupt partial cannot be a useful resume source.
                    try output.truncate(atOffset: 0)
                    throw SDExportClientError.checksum(file.path)
                }
                try output.close()
                try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                try replace(partial, with: target)
                try fm.removeItem(at: journalURL)
                progress(SDExportProgress(phase: .completed, path: file.path, bytes: file.size, total: file.size))
                return
            } catch let error as URLError where error.code != .cancelled && attempt < 2 {
                try await Task.sleep(nanoseconds: UInt64(attempt + 1) * 1_000_000_000)
            }
        }
    }

    private func request(_ endpoint: String, query: [URLQueryItem] = []) -> URLRequest {
        var components = URLComponents(url: info.baseURL.appendingPathComponent("v1/export/" + endpoint), resolvingAgainstBaseURL: false)!
        if !query.isEmpty {
            components.queryItems = query
            // URLComponents permits '+' in queries; encode it explicitly so
            // form-style servers preserve a literal plus in an SD filename.
            components.percentEncodedQuery = components.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")
        }
        var result = URLRequest(url: components.url!)
        result.setValue("Bearer " + info.token, forHTTPHeaderField: "Authorization")
        result.setValue(info.sessionID, forHTTPHeaderField: "X-Export-Session")
        result.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
        return result
    }

    private static func requireHTTP(_ response: URLResponse, code: Int) throws {
        guard let response = response as? HTTPURLResponse else { throw SDExportClientError.invalidResponse }
        guard response.statusCode == code else { throw SDExportClientError.http(response.statusCode) }
    }

    private func size(_ url: URL) throws -> UInt64 {
        guard let value = try fm.attributesOfItem(atPath: url.path)[.size] as? NSNumber else {
            throw SDExportClientError.invalidResponse
        }
        return value.uint64Value
    }

    private func sha256(_ url: URL) async throws -> String {
        let input = try FileHandle(forReadingFrom: url)
        defer { try? input.close() }
        var hasher = SHA256()
        while let data = try input.read(upToCount: 524_288), !data.isEmpty {
            try Task.checkCancellation()
            hasher.update(data: data)
            await Task.yield() // Let the lease heartbeat run during large local hashes.
        }
        return SDExportManifestParser.hex(hasher.finalize())
    }

    private func replace(_ source: URL, with target: URL) throws {
        if fm.fileExists(atPath: target.path) {
            _ = try fm.replaceItemAt(target, withItemAt: source, backupItemName: nil, options: .usingNewMetadataOnly)
        } else { try fm.moveItem(at: source, to: target) }
    }
}

private final class SDExportRedirectPolicy: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        // Every request is bound to the fixed device endpoint and session.
        completionHandler(nil)
    }
}

/// Foreground coordinator. The caller cancels its Task when leaving foreground;
/// verified files and partial journals remain available for the next invocation.
@MainActor
public final class SDExportCoordinator {
    private let link: SDExportBLELink
    private var running = false
    public init(link: SDExportBLELink) { self.link = link }

    /// Explicit deletion only. exportAll never deletes source files.
    public func deleteFiles(paths: [String]) async throws -> SDFileDeletionResult {
        guard !running else { throw SDExportClientError.alreadyRunning }
        guard UIApplication.shared.applicationState == .active else { throw SDExportClientError.foregroundRequired }
        for path in paths { try SDExportPaths.validate(path) }
        guard !paths.isEmpty, Set(paths).count == paths.count else {
            throw SDExportClientError.unsafePath("empty or duplicate paths")
        }
        running = true
        defer { running = false }
        var sessionID: UInt64 = 0
        var ssid: String?
        var client: SDExportHTTPClient?
        var confirmedDeleted: [String] = []
        do {
            try await link.requireFileDeletion()
            let ready = try await link.beginExport()
            sessionID = ready.sessionID
            let info = try await link.readConnectionInfo(sessionID: sessionID)
            ssid = info.ssid
            try await join(info)
            let http = SDExportHTTPClient(info: info)
            client = http
            for attempt in 0..<3 {
                do { try await http.keepAlive(); break }
                catch let error as URLError where error.code != .cancelled && attempt < 2 {
                    try await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
            let deleted = try await http.deleteFiles(paths: paths)
            confirmedDeleted = deleted
            // Closing a management-only session uses CANCEL, not an export receipt.
            try await link.cancelExport(sessionID: sessionID)
            var restored = false
            let deadline = Date().addingTimeInterval(20)
            while Date() < deadline {
                try Task.checkCancellation()
                do {
                    let status = try await link.queryStatus(sessionID: sessionID)
                    guard status.sessionID == sessionID else { break }
                    if status.state == .cancelled { restored = true; break }
                    if status.state == .failed { throw SDExportClientError.device(status.error.rawValue) }
                } catch SDExportClientError.bleDisconnected { break }
                catch SDExportClientError.bleTimeout { }
                try await Task.sleep(nanoseconds: 1_000_000_000)
            }
            await http.invalidate()
            NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: info.ssid)
            return SDFileDeletionResult(deletedPaths: deleted, deviceRestored: restored)
        } catch {
            if let client { await client.invalidate() }
            let active = sessionID != 0 ? sessionID : link.ownedSessionID
            if active != 0 {
                await Task { @MainActor in try? await self.link.cancelExport(sessionID: active) }.value
            }
            if let ssid { NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid) }
            if !confirmedDeleted.isEmpty {
                throw SDExportClientError.partialDeletion(deletedPaths: confirmedDeleted, reason: String(describing: error))
            }
            throw error
        }
    }

    public func exportAll(to directory: URL,
                          progress: @escaping @Sendable (SDExportProgress) -> Void = { _ in }) async throws -> SDExportResult {
        guard !running else { throw SDExportClientError.alreadyRunning }
        guard UIApplication.shared.applicationState == .active else { throw SDExportClientError.foregroundRequired }
        running = true
        defer { running = false }
        var sessionID: UInt64 = 0
        var ssid: String?
        var client: SDExportHTTPClient?
        var heartbeat: Task<Void, Never>?
        do {
            let ready = try await link.beginExport()
            sessionID = ready.sessionID
            let info = try await link.readConnectionInfo(sessionID: sessionID)
            ssid = info.ssid
            try await join(info)
            let http = SDExportHTTPClient(info: info)
            client = http
            // apply() can complete before DHCP/routing is ready. Validate the
            // authenticated endpoint, allowing bounded network settling time.
            for attempt in 0..<3 {
                do { try await http.keepAlive(); break }
                catch let error as URLError where error.code != .cancelled && attempt < 2 {
                    try await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
            heartbeat = Task {
                while !Task.isCancelled {
                    do { try await Task.sleep(nanoseconds: 20_000_000_000) }
                    catch { return }
                    guard !Task.isCancelled else { return }
                    try? await http.keepAlive()
                }
            }
            let downloaded = try await http.download(to: directory, progress: progress)
            heartbeat?.cancel(); heartbeat = nil
            do { try await http.complete(downloaded.manifest) }
            catch {
                // The response can be lost as the AP shuts down. A final BLE
                // COMPLETED for this exact session also proves acknowledgement.
                guard let terminal = try? await link.queryStatus(sessionID: sessionID),
                      terminal.sessionID == sessionID, terminal.state == .completed else { throw error }
            }
            var restored = false
            let deadline = Date().addingTimeInterval(20)
            while Date() < deadline {
                try Task.checkCancellation()
                do {
                    let status = try await link.queryStatus(sessionID: sessionID)
                    guard status.sessionID == sessionID else { break }
                    if status.state == .completed { restored = true; break }
                    if status.state == .failed { throw SDExportClientError.device(status.error.rawValue) }
                } catch SDExportClientError.bleDisconnected { break }
                catch SDExportClientError.bleTimeout { /* Poll again. */ }
                try await Task.sleep(nanoseconds: 1_000_000_000)
            }
            await http.invalidate()
            NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: info.ssid)
            return SDExportResult(filesDirectory: downloaded.filesDirectory, manifestURL: downloaded.manifestURL,
                                  fileCount: downloaded.manifest.end.fileCount, totalBytes: downloaded.manifest.end.totalBytes,
                                  deviceRestored: restored)
        } catch {
            heartbeat?.cancel()
            if let client { await client.invalidate() }
            let activeSession = sessionID != 0 ? sessionID : link.ownedSessionID
            if activeSession != 0 {
                // An unstructured cleanup task is not born cancelled when the
                // caller's foreground task was cancelled.
                await Task { @MainActor in try? await self.link.cancelExport(sessionID: activeSession) }.value
            }
            if let ssid { NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid) }
            throw error
        }
    }

    private func join(_ info: SDExportConnectionInfo) async throws {
        guard UIApplication.shared.applicationState == .active else { throw SDExportClientError.foregroundRequired }
        let config = NEHotspotConfiguration(ssid: info.ssid, passphrase: info.passphrase, isWEP: false)
        config.joinOnce = true
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            NEHotspotConfigurationManager.shared.apply(config) { error in
                if let error = error as NSError?,
                   !(error.domain == NEHotspotConfigurationErrorDomain &&
                     error.code == NEHotspotConfigurationError.alreadyAssociated.rawValue) {
                    continuation.resume(throwing: error)
                } else { continuation.resume() }
            }
        }
        try Task.checkCancellation()
    }
}
