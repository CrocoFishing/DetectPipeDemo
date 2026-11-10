import XCTest
import CryptoKit
@testable import EventPipeSDExport

final class SDExportContractTests: XCTestCase {
    func testReceiverNoFaceHasDistinctWireStatusAndOwnership() throws {
        let packet = try Packet.decode(RecognitionResult.noFace(requestID: 21, imageID: 22,
                                                                processingTimeMs: 23).encodedPacket())
        XCTAssertEqual(packet.header.messageType, .recognitionResult)
        XCTAssertEqual(packet.header.requestID, 21)
        XCTAssertEqual(packet.header.imageID, 22)
        XCTAssertEqual(packet.payload[0], 2)
        XCTAssertEqual(packet.payload[1], 0)
        XCTAssertTrue(packet.payload[4..<8].allSatisfy { $0 == 0 })
        XCTAssertEqual(Array(packet.payload[8..<12]), [23, 0, 0, 0])
    }

    func testPriorBatchErrorWithSuccessfulLatestBatchDecodes() throws {
        var data = Data([1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1])
        data.append(contentsOf: [0, 0, 0, 0, 0])
        let packet = Packet(header: PacketHeader(messageType: .eventComplete, requestID: 21), payload: data)
        _ = try EventPayload.decode(packet)
    }
    private let session = "0000000000000001"
    private let emptySHA = SDExportManifestParser.hex(SHA256.hash(data: Data()))

    private func line(_ value: [String: Any]) throws -> Data {
        var data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
        data.append(10)
        return data
    }

    private func parser() throws -> SDExportManifestParser {
        var result = SDExportManifestParser(sessionID: session)
        _ = try result.consume(line(["type": "header", "format_version": 1, "session_id": session]))
        return result
    }

    private func fileLine(path: String = "event_0000002500000068.wav", size: UInt64 = 0) throws -> Data {
        try line(["type": "file", "path": path, "size": size, "sha256": emptySHA, "etag": "\"session-file\""])
    }

    func testExactFileBytesIncludingLFAreHashedAndProgressIsExcluded() throws {
        var parser = try parser()
        let file = try fileLine()
        _ = try parser.consume(line(["type": "progress", "path": "event_0000002500000068.wav", "bytes_hashed": 0]))
        _ = try parser.consume(file)
        let digest = SDExportManifestParser.hex(SHA256.hash(data: file))
        _ = try parser.consume(line(["type": "end", "file_count": 1, "total_bytes": 0, "manifest_sha256": digest]))
        let manifest = try parser.finish()
        XCTAssertEqual(manifest.files.count, 1)
        XCTAssertEqual(manifest.end.manifestSHA256, digest)
    }

    func testEmptyCardHasEmptyFileRecordDigest() throws {
        var parser = try parser()
        _ = try parser.consume(line(["type": "end", "file_count": 0, "total_bytes": 0, "manifest_sha256": emptySHA]))
        XCTAssertTrue(try parser.finish().files.isEmpty)
    }

    func testTruncatedManifestNeverPassesWithoutEnd() throws {
        var parser = try parser()
        _ = try parser.consume(fileLine())
        XCTAssertThrowsError(try parser.finish())
    }

    func testMissingLFIsRejected() throws {
        var parser = try parser()
        XCTAssertThrowsError(try parser.consume(Data(try fileLine().dropLast())))
    }

    func testWrongCountAndDigestAreRejected() throws {
        var parser = try parser()
        _ = try parser.consume(fileLine())
        XCTAssertThrowsError(try parser.consume(line(["type": "end", "file_count": 1, "total_bytes": 0, "manifest_sha256": emptySHA])))
    }

    func testCaseInsensitiveDestinationCollisionIsRejected() throws {
        var parser = try parser()
        _ = try parser.consume(fileLine(path: "A/file.wav"))
        XCTAssertThrowsError(try parser.consume(fileLine(path: "a/FILE.wav")))
    }

    func testWrongSessionHeaderIsRejected() throws {
        var parser = SDExportManifestParser(sessionID: session)
        XCTAssertThrowsError(try parser.consume(line(["type": "header", "format_version": 1, "session_id": "0000000000000002"])))
    }

    func testUnsafeRelativePathsAreRejected() {
        for path in ["../secret", "/absolute", "a/../secret", "a//b", "a\\b", "a/./b", "a\nb", "C:secret"] {
            XCTAssertThrowsError(try SDExportPaths.validate(path), path)
        }
        XCTAssertNoThrow(try SDExportPaths.validate("nested/測試 100%.wav"))
    }

    func testEventIDPreservesLeadingZerosAndRejectsFloatingPointJSON() throws {
        let id = try XCTUnwrap(SDStoredEventID(rawValue: "0000002500000068"))
        XCTAssertEqual(id.bootSession, 37)
        XCTAssertEqual(id.sequence, 104)
        XCTAssertEqual(id.audioFilename, "event_0000002500000068.wav")
        XCTAssertEqual(try JSONDecoder().decode(SDStoredEventID.self, from: JSONEncoder().encode(id)), id)
        XCTAssertNil(SDStoredEventID(rawValue: "0000000000000000"))
        XCTAssertNil(SDStoredEventID(rawValue: "000000250000006a"))
        XCTAssertThrowsError(try JSONDecoder().decode(SDStoredEventID.self, from: Data("159000000000.0".utf8)))
    }

    func testOwnedGenericControlErrorDecodesLittleEndianPrefix() throws {
        let packet = Packet(header: PacketHeader(messageType: .error, requestID: 9), payload: Data([0x34, 0x12, 0xff]))
        let failure = try XCTUnwrap(SDExportControlError.decode(packet, issuedRequestIDs: [9]))
        XCTAssertEqual(failure.requestID, 9)
        XCTAssertEqual(failure.code, 0x1234)
    }

    func testCaptureAndExpiredRequestErrorsAreIgnoredByExportLink() {
        let packet = Packet(header: PacketHeader(messageType: .error, requestID: 8), payload: Data([4, 0]))
        XCTAssertNil(SDExportControlError.decode(packet, issuedRequestIDs: [9]))
        XCTAssertNil(SDExportControlError.decode(packet, issuedRequestIDs: []))
    }

    func testTruncatedOrNonErrorReplyIsNotAControlError() {
        let short = Packet(header: PacketHeader(messageType: .error, requestID: 9), payload: Data([4]))
        let status = Packet(header: PacketHeader(messageType: .status, requestID: 9), payload: Data([4, 0]))
        XCTAssertNil(SDExportControlError.decode(short, issuedRequestIDs: [9]))
        XCTAssertNil(SDExportControlError.decode(status, issuedRequestIDs: [9]))
    }
}
