import Foundation

/// SD/JSON identifiers use a string, preserving all 64 bits and leading zeros.
public struct SDStoredEventID: RawRepresentable, Hashable, Codable, Sendable {
    public let rawValue: String
    public init?(rawValue: String) {
        guard rawValue.utf8.count == 16,
              rawValue.utf8.allSatisfy({ (48...57).contains($0) || (65...70).contains($0) }),
              let number = UInt64(rawValue, radix: 16), number != 0 else { return nil }
        self.rawValue = rawValue
    }
    public var bootSession: UInt32 { UInt32(UInt64(rawValue, radix: 16)! >> 32) }
    public var sequence: UInt32 { UInt32(UInt64(rawValue, radix: 16)! & 0xffffffff) }
    public var audioFilename: String { "event_" + rawValue + ".wav" }
    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        let value = try container.decode(String.self)
        guard let id = Self(rawValue: value) else {
            throw DecodingError.dataCorruptedError(in: container, debugDescription: "Expected a nonzero 16-digit uppercase Event ID")
        }
        self = id
    }
    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(rawValue)
    }
}

// Reference import types for the raw CSV columns documented in EVENT_DATA_MODEL.
// Parse CSV quoting first, then convert decimal strings to these integer types.
// These are App-side values, not a new on-device JSON/wire representation.
public struct SDStoredAudioEvent: Sendable {
    public let eventID: SDStoredEventID
    public let audioFile: String
    public let audioSource: String
    public let sampleRate: UInt32
    public let channels: UInt16
    public let bitsPerSample: UInt16
    public let audioFirstSample: UInt64
    public let vadStartSample: UInt64
    public let vadEndSample: UInt64
    public let audioLastSample: UInt64
    public let vadStartTimestampUs: Int64
    public let vadEndTimestampUs: Int64
    public let audioStatus: SDStoredAudioStatus

    public init(eventID: SDStoredEventID, audioFile: String, audioSource: String = "RAW",
                sampleRate: UInt32 = 16_000, channels: UInt16 = 1, bitsPerSample: UInt16 = 16,
                audioFirstSample: UInt64, vadStartSample: UInt64, vadEndSample: UInt64,
                audioLastSample: UInt64, vadStartTimestampUs: Int64, vadEndTimestampUs: Int64,
                audioStatus: SDStoredAudioStatus) {
        self.eventID = eventID; self.audioFile = audioFile; self.audioSource = audioSource
        self.sampleRate = sampleRate; self.channels = channels; self.bitsPerSample = bitsPerSample
        self.audioFirstSample = audioFirstSample; self.vadStartSample = vadStartSample
        self.vadEndSample = vadEndSample; self.audioLastSample = audioLastSample
        self.vadStartTimestampUs = vadStartTimestampUs; self.vadEndTimestampUs = vadEndTimestampUs
        self.audioStatus = audioStatus
    }

    public var savedFrameCount: UInt64? {
        audioLastSample >= audioFirstSample ? audioLastSample - audioFirstSample : nil
    }
}

public enum SDStoredAudioStatus: String, Codable, Sendable { case ready = "READY", failed = "FAILED" }
public enum SDStoredFaceResultStatus: String, Codable, Sendable {
    case recognized = "RECOGNIZED", unknown = "UNKNOWN", failed = "FAILED"
}
public enum SDStoredFaceBatchStatus: String, Codable, Sendable {
    case completed = "COMPLETED", noFace = "NO_FACE", partial = "PARTIAL", busy = "BUSY"
    case timeout = "TIMEOUT", disconnected = "DISCONNECTED", failed = "FAILED"
}
public enum SDStoredEventCompletion: String, Codable, Sendable {
    case complete = "COMPLETE", completeWithError = "COMPLETE_WITH_ERROR", incomplete = "INCOMPLETE"
}

public struct SDStoredFaceAssociation: Sendable {
    public let eventID: SDStoredEventID
    public let requestID: UInt32
    public let imageID: UInt32
    public let faceIndex: UInt16
    public let faceCount: UInt16
    public let status: SDStoredFaceResultStatus
    public let personID: String
    public let similarity: Float
    public let processingTimeMs: UInt32

    public init(eventID: SDStoredEventID, requestID: UInt32, imageID: UInt32,
                faceIndex: UInt16, faceCount: UInt16, status: SDStoredFaceResultStatus,
                personID: String, similarity: Float, processingTimeMs: UInt32) {
        self.eventID = eventID; self.requestID = requestID; self.imageID = imageID
        self.faceIndex = faceIndex; self.faceCount = faceCount; self.status = status
        self.personID = personID; self.similarity = similarity; self.processingTimeMs = processingTimeMs
    }
}

/// image_id=0/face_index=-1 rows map here, rather than to a face object.
public struct SDStoredFaceBatchTerminal: Sendable {
    public let eventID: SDStoredEventID
    public let requestID: UInt32
    public let expectedFaceCount: UInt16
    public let status: SDStoredFaceBatchStatus

    public init(eventID: SDStoredEventID, requestID: UInt32, expectedFaceCount: UInt16,
                status: SDStoredFaceBatchStatus) {
        self.eventID = eventID; self.requestID = requestID
        self.expectedFaceCount = expectedFaceCount; self.status = status
    }
}

public struct SDStoredLifecycleTransition: Sendable {
    public let eventID: SDStoredEventID
    public let state: String
    public let timestampUs: Int64
    public let status: String

    public init(eventID: SDStoredEventID, state: String, timestampUs: Int64, status: String) {
        self.eventID = eventID; self.state = state; self.timestampUs = timestampUs; self.status = status
    }
}
