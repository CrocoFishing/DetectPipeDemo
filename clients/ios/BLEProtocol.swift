import Foundation
import CoreBluetooth

public enum BLEProtocol {
    public static let version: UInt8 = 1
    public static let magic: UInt32 = 0x45434146
    public static let headerLength = 32
    public static let serviceUUID = CBUUID(string: "7f510000-b7b2-4f6a-9f3a-6c8a2d7e1000")
    public static let controlUUID = CBUUID(string: "7f510001-b7b2-4f6a-9f3a-6c8a2d7e1000")
    public static let eventUUID = CBUUID(string: "7f510002-b7b2-4f6a-9f3a-6c8a2d7e1000")
    public static let imageDataUUID = CBUUID(string: "7f510003-b7b2-4f6a-9f3a-6c8a2d7e1000")
    public static let recognitionResultUUID = CBUUID(string: "7f510004-b7b2-4f6a-9f3a-6c8a2d7e1000")
    public static let deviceInformationUUID = CBUUID(string: "7f510005-b7b2-4f6a-9f3a-6c8a2d7e1000")
}

public enum MessageType: UInt8 {
    case hello = 1, helloAck = 2, startCapture = 3, captureAccepted = 4
    case busy = 5, status = 6, imageBegin = 7, imageChunk = 8, imageEnd = 9
    case recognitionResult = 10, noFace = 11, error = 12, ping = 13, pong = 14
}

public struct MessageFlags: OptionSet {
    public let rawValue: UInt16
    public init(rawValue: UInt16) { self.rawValue = rawValue }
    public static let faceSequence = MessageFlags(rawValue: 0x0001)
}

public enum Command: UInt16 { case startCapture = 1 }
public enum StatusCode: UInt8 { case ok = 0, unknown = 1, noFace = 2, failed = 3 }
public enum ErrorCode: UInt16 {
    case none = 0, invalidPacket = 1, unsupportedVersion = 2, crcMismatch = 3
    case busy = 4, duplicateRequest = 5, cameraInit = 6, psramUnavailable = 7
    case captureFailed = 8, detectorLoad = 9, invalidBoundingBox = 10
    case cropAllocation = 11, jpegEncode = 12, bleNotConnected = 13
    case notifyNotSubscribed = 14, bleCongestion = 15, transferDisconnected = 16
    case imageTimeout = 17, chunkMissing = 18, recognitionTimeout = 19
    case staleResult = 20, incorrectImageId = 21, internalError = 22
}

public enum ProtocolValidationError: Error {
    case partialPacket, invalidMagic, unsupportedVersion(UInt8), unknownMessage(UInt8)
    case invalidLength, crcMismatch, wrongIdentifiers, invalidSequence, missingChunks([UInt16])
}

public struct PacketHeader {
    public var messageType: MessageType
    public var flags: UInt16 = 0
    public var requestID: UInt32 = 0
    public var imageID: UInt32 = 0
    public var payloadLength: UInt32 = 0
    public var chunkIndex: UInt16 = 0
    public var totalChunks: UInt16 = 0
    public var crc32: UInt32 = 0
    public var reserved: UInt32 = 0

    public func encoded() -> Data {
        var data = Data()
        data.appendLE(BLEProtocol.magic); data.append(BLEProtocol.version); data.append(messageType.rawValue)
        data.appendLE(flags); data.appendLE(requestID); data.appendLE(imageID); data.appendLE(payloadLength)
        data.appendLE(chunkIndex); data.appendLE(totalChunks); data.appendLE(crc32); data.appendLE(reserved)
        return data
    }

    public static func decode(_ data: Data) throws -> PacketHeader {
        guard data.count >= BLEProtocol.headerLength else { throw ProtocolValidationError.partialPacket }
        guard data.uint32LE(at: 0) == BLEProtocol.magic else { throw ProtocolValidationError.invalidMagic }
        let version = data[4]
        guard version == BLEProtocol.version else { throw ProtocolValidationError.unsupportedVersion(version) }
        guard let type = MessageType(rawValue: data[5]) else { throw ProtocolValidationError.unknownMessage(data[5]) }
        return PacketHeader(messageType: type, flags: data.uint16LE(at: 6), requestID: data.uint32LE(at: 8),
                            imageID: data.uint32LE(at: 12), payloadLength: data.uint32LE(at: 16),
                            chunkIndex: data.uint16LE(at: 20), totalChunks: data.uint16LE(at: 22),
                            crc32: data.uint32LE(at: 24), reserved: data.uint32LE(at: 28))
    }
}

public struct Packet {
    public let header: PacketHeader
    public let payload: Data

    public func encoded() -> Data {
        var value = header
        value.payloadLength = UInt32(payload.count)
        value.crc32 = CRC32.checksum(payload)
        return value.encoded() + payload
    }

    public static func decode(_ data: Data) throws -> Packet {
        let header = try PacketHeader.decode(data)
        guard data.count == BLEProtocol.headerLength + Int(header.payloadLength) else {
            throw ProtocolValidationError.invalidLength
        }
        let payload = data.subdata(in: BLEProtocol.headerLength..<data.count)
        guard CRC32.checksum(payload) == header.crc32 else { throw ProtocolValidationError.crcMismatch }
        return Packet(header: header, payload: payload)
    }
}

public struct RecognitionResult {
    public let requestID: UInt32
    public let imageID: UInt32
    public let status: StatusCode
    public let personID: String
    public let personName: String
    public let similarity: Float
    public let processingTimeMs: UInt32

    public func encodedPacket() -> Data {
        let id = Data(personID.utf8.prefix(32)); let name = Data(personName.utf8.prefix(64))
        var payload = Data([status.rawValue, UInt8(id.count), UInt8(name.count), 0])
        payload.appendLE(similarity.bitPattern); payload.appendLE(processingTimeMs); payload.append(id); payload.append(name)
        return Packet(header: PacketHeader(messageType: .recognitionResult, requestID: requestID, imageID: imageID),
                      payload: payload).encoded()
    }
}

public enum CRC32 {
    public static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xffffffff
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = (crc >> 1) ^ ((crc & 1) == 1 ? 0xedb88320 : 0) }
        }
        return crc ^ 0xffffffff
    }
}

public final class ImageChunkAssembler {
    private var requestID: UInt32 = 0, imageID: UInt32 = 0
    private var expectedBytes: UInt32 = 0, expectedCRC: UInt32 = 0
    private var totalChunks: UInt16 = 0
    private var chunks: [UInt16: Data] = [:]
    public private(set) var duplicatedSequenceCount = 0
    public private(set) var faceIndex: UInt16 = 0
    public private(set) var faceCount: UInt16 = 1
    private var faceSequenceFlagged = false

    public init() {}

    public func begin(_ packet: Packet) throws {
        guard packet.header.messageType == .imageBegin, packet.payload.count == 8 else {
            throw ProtocolValidationError.invalidLength
        }
        requestID = packet.header.requestID; imageID = packet.header.imageID
        let sequence = try Self.decodeFaceSequence(packet.header)
        faceSequenceFlagged = sequence.flagged
        faceIndex = sequence.index; faceCount = sequence.count
        expectedBytes = packet.payload.uint32LE(at: 0); expectedCRC = packet.payload.uint32LE(at: 4)
        totalChunks = 0; chunks.removeAll(keepingCapacity: true); duplicatedSequenceCount = 0
    }

    public func add(_ packet: Packet) throws {
        guard packet.header.messageType == .imageChunk, packet.header.requestID == requestID,
              packet.header.imageID == imageID else { throw ProtocolValidationError.wrongIdentifiers }
        guard packet.header.totalChunks > 0, packet.header.chunkIndex < packet.header.totalChunks else {
            throw ProtocolValidationError.invalidSequence
        }
        totalChunks = packet.header.totalChunks
        if let old = chunks[packet.header.chunkIndex] {
            duplicatedSequenceCount += 1
            guard old == packet.payload else { throw ProtocolValidationError.invalidSequence }
        } else { chunks[packet.header.chunkIndex] = packet.payload }
    }

    public func finish(_ packet: Packet) throws -> Data {
        guard packet.header.messageType == .imageEnd, packet.header.requestID == requestID,
              packet.header.imageID == imageID else { throw ProtocolValidationError.wrongIdentifiers }
        guard packet.payload.count == 8 else { throw ProtocolValidationError.invalidLength }
        let sequence = try Self.decodeFaceSequence(packet.header)
        guard sequence.flagged == faceSequenceFlagged,
              sequence.index == faceIndex, sequence.count == faceCount else {
            throw ProtocolValidationError.invalidSequence
        }
        guard packet.payload.uint32LE(at: 0) == expectedBytes,
              packet.payload.uint32LE(at: 4) == expectedCRC else {
            throw ProtocolValidationError.invalidSequence
        }
        let missing = (0..<totalChunks).filter { chunks[$0] == nil }
        guard missing.isEmpty else { throw ProtocolValidationError.missingChunks(missing) }
        var image = Data(); for index in 0..<totalChunks { image.append(chunks[index]!) }
        guard image.count == Int(expectedBytes) else { throw ProtocolValidationError.invalidLength }
        guard CRC32.checksum(image) == expectedCRC else { throw ProtocolValidationError.crcMismatch }
        return image
    }

    private static func decodeFaceSequence(
        _ header: PacketHeader
    ) throws -> (flagged: Bool, index: UInt16, count: UInt16) {
        let flagged = MessageFlags(rawValue: header.flags).contains(.faceSequence)
        guard flagged else { return (false, 0, 1) }
        let index = UInt16(header.reserved & 0xffff)
        let count = UInt16((header.reserved >> 16) & 0xffff)
        guard count > 0, index < count else { throw ProtocolValidationError.invalidSequence }
        return (true, index, count)
    }
}

private extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var little = value.littleEndian; Swift.withUnsafeBytes(of: &little) { append(contentsOf: $0) }
    }
    func uint16LE(at offset: Int) -> UInt16 { UInt16(self[offset]) | UInt16(self[offset + 1]) << 8 }
    func uint32LE(at offset: Int) -> UInt32 {
        UInt32(self[offset]) | UInt32(self[offset + 1]) << 8 | UInt32(self[offset + 2]) << 16 | UInt32(self[offset + 3]) << 24
    }
}
