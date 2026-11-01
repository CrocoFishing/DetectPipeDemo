import Foundation
import CoreBluetooth

/// A connected-peripheral adapter. The App retains its central/peripheral delegates
/// and forwards the callbacks listed in IOS_SD_EXPORT.md, on the main actor.
@MainActor
public final class SDExportBLELink {
    public let peripheral: CBPeripheral
    public private(set) var latestStatus: SDExportStatus?
    public private(set) var ownedSessionID: UInt64 = 0
    private var startRequestID: UInt32?
    private var control: CBCharacteristic?
    private var event: CBCharacteristic?
    private var information: CBCharacteristic?
    private var exportInformation: CBCharacteristic?
    private var prepared = false
    private var nextID: UInt32 = 1
    private var servicesWaiter: ExportBLEWaiter<CBService>?
    private var characteristicsWaiter: ExportBLEWaiter<Void>?
    private var notifyWaiter: ExportBLEWaiter<Void>?
    private var readWaiter: (CBUUID, ExportBLEWaiter<Data>)?
    private var writeWaiter: ExportBLEWaiter<Void>?
    private var statusWaiter: (UInt32, ExportBLEWaiter<SDExportStatus>)?
    private var statuses: [UInt32: SDExportStatus] = [:]
    private var issuedRequestIDs = Set<UInt32>()
    private var requestErrors: [UInt32: UInt16] = [:]

    public init(peripheral: CBPeripheral) { self.peripheral = peripheral }

    /// Discover the optional export characteristic, verify the feature bit, and
    /// subscribe Event. CoreBluetooth performs a long characteristic read itself.
    public func prepare() async throws {
        try Task.checkCancellation()
        guard peripheral.state == .connected else { throw SDExportClientError.bleDisconnected }
        prepared = false
        control = nil; event = nil; information = nil; exportInformation = nil
        let service: CBService = try await wait(timeout: 10, install: { servicesWaiter = $0 }) {
            peripheral.discoverServices([BLEProtocol.serviceUUID])
        }
        let _: Void = try await wait(timeout: 10, install: { characteristicsWaiter = $0 }) {
            peripheral.discoverCharacteristics([
                BLEProtocol.controlUUID, BLEProtocol.eventUUID,
                BLEProtocol.deviceInformationUUID, BLEProtocol.sdExportInfoUUID
            ], for: service)
        }
        guard let event, let information, control != nil, exportInformation != nil else {
            throw SDExportClientError.unsupportedDevice
        }
        let info = try await read(information)
        guard let text = String(data: info, encoding: .utf8),
              let field = text.split(separator: ";").first(where: { $0.hasPrefix("features=") }),
              let features = UInt32(String(field.dropFirst("features=".count)).replacingOccurrences(of: "0x", with: ""), radix: 16),
              features & BLEProtocol.wifiSDExportFeature != 0 else {
            throw SDExportClientError.unsupportedDevice
        }
        if !event.isNotifying {
            let _: Void = try await wait(timeout: 10, install: { notifyWaiter = $0 }) {
                peripheral.setNotifyValue(true, for: event)
            }
        }
        prepared = true
    }

    /// Wait for a snapshot while retaining existing work. Missing notifications
    /// are recovered by GET_STATUS; this does not resend START.
    public func beginExport() async throws -> SDExportStatus {
        if !prepared { try await prepare() }
        statuses.removeAll(); latestStatus = nil; ownedSessionID = 0
        let startID = requestID()
        startRequestID = startID
        issuedRequestIDs.insert(startID)
        defer { finishRequest(startID) }
        try await write(try SDExportCommand.start(requestID: startID), requestID: startID)
        let deadline = Date().addingTimeInterval(50)
        var sessionID: UInt64 = 0
        while Date() < deadline {
            try Task.checkCancellation()
            try throwRequestError(startID)
            if let startResponse = statuses.removeValue(forKey: startID), startResponse.error != .none {
                throw SDExportClientError.device(startResponse.error.rawValue)
            }
            let status: SDExportStatus
            do {
                status = try await queryStatus(sessionID: sessionID)
            } catch SDExportClientError.servicesChanged {
                try await prepare()
                continue
            } catch SDExportClientError.bleTimeout {
                continue
            }
            try throwRequestError(startID)
            guard status.error == .none else { throw SDExportClientError.device(status.error.rawValue) }
            if status.sessionID != 0 { sessionID = status.sessionID }
            switch status.state {
            case .ready, .transferring:
                ownedSessionID = status.sessionID
                return status
            case .failed: throw SDExportClientError.device(status.error.rawValue)
            case .cancelled: throw CancellationError()
            default: break
            }
            try await Task.sleep(nanoseconds: 1_000_000_000)
        }
        throw SDExportClientError.bleTimeout
    }

    public func queryStatus(sessionID: UInt64 = 0) async throws -> SDExportStatus {
        if !prepared { try await prepare() }
        let id = requestID()
        issuedRequestIDs.insert(id)
        defer { finishRequest(id) }
        try await write(try SDExportCommand.getStatus(requestID: id, sessionID: sessionID), requestID: id)
        if let startRequestID, issuedRequestIDs.contains(startRequestID) { try throwRequestError(startRequestID) }
        if let status = statuses.removeValue(forKey: id) { return status }
        return try await wait(timeout: 3, install: { statusWaiter = (id, $0) }) {}
    }

    public func readConnectionInfo(sessionID: UInt64) async throws -> SDExportConnectionInfo {
        if !prepared { try await prepare() }
        guard let exportInformation else { throw SDExportClientError.unsupportedDevice }
        let data = try await read(exportInformation)
        guard data.count <= 512 else { throw SDExportClientError.invalidConnectionInfo }
        let info = try JSONDecoder().decode(SDExportConnectionInfo.self, from: data)
        try info.validate(expectedSessionID: sessionID)
        return info
    }

    public func cancelExport(sessionID: UInt64) async throws {
        guard sessionID != 0 else { return }
        if !prepared { try await prepare() }
        let id = requestID()
        issuedRequestIDs.insert(id)
        defer { finishRequest(id) }
        try await write(try SDExportCommand.cancel(requestID: id, sessionID: sessionID), requestID: id)
    }

    // Forward these from CBPeripheralDelegate and CBCentralManagerDelegate.
    public func didDiscoverServices(error: Error?) {
        if let error { servicesWaiter?.finish(.failure(error)) }
        else if let service = peripheral.services?.first(where: { $0.uuid == BLEProtocol.serviceUUID }) {
            servicesWaiter?.finish(.success(service))
        } else { servicesWaiter?.finish(.failure(SDExportClientError.unsupportedDevice)) }
        servicesWaiter = nil
    }

    public func didDiscoverCharacteristics(for service: CBService, error: Error?) {
        guard service.uuid == BLEProtocol.serviceUUID else { return }
        if let error { characteristicsWaiter?.finish(.failure(error)) }
        else {
            for characteristic in service.characteristics ?? [] {
                switch characteristic.uuid {
                case BLEProtocol.controlUUID: control = characteristic
                case BLEProtocol.eventUUID: event = characteristic
                case BLEProtocol.deviceInformationUUID: information = characteristic
                case BLEProtocol.sdExportInfoUUID: exportInformation = characteristic
                default: break
                }
            }
            characteristicsWaiter?.finish(.success(()))
        }
        characteristicsWaiter = nil
    }

    public func didUpdateNotificationState(for characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == BLEProtocol.eventUUID else { return }
        if let error { notifyWaiter?.finish(.failure(error)) }
        else if characteristic.isNotifying { notifyWaiter?.finish(.success(())) }
        else { notifyWaiter?.finish(.failure(SDExportClientError.unsupportedDevice)) }
        notifyWaiter = nil
    }

    public func didUpdateValue(for characteristic: CBCharacteristic, error: Error?) {
        if let (uuid, waiter) = readWaiter, characteristic.uuid == uuid {
            if let error { waiter.finish(.failure(error)) }
            else if let value = characteristic.value { waiter.finish(.success(value)) }
            else { waiter.finish(.failure(SDExportClientError.invalidConnectionInfo)) }
            readWaiter = nil
        }
        guard characteristic.uuid == BLEProtocol.eventUUID, error == nil,
              let data = characteristic.value, let packet = try? Packet.decode(data) else { return }
        if let failure = SDExportControlError.decode(packet, issuedRequestIDs: issuedRequestIDs) {
            if requestErrors.count >= 16 { requestErrors.removeAll() }
            requestErrors[failure.requestID] = failure.code
            if let (id, waiter) = statusWaiter,
               id == failure.requestID || failure.requestID == startRequestID {
                waiter.finish(.failure(SDExportClientError.device(failure.code)))
                statusWaiter = nil
            }
            return
        }
        guard packet.header.messageType == .sdExportStatus,
              let status = try? SDExportStatus.decode(packet) else { return }
        latestStatus = status
        if let startRequestID, packet.header.requestID == startRequestID, status.error == .none,
           status.state == .preparing || status.state == .ready || status.state == .transferring {
            ownedSessionID = status.sessionID
        }
        if statuses.count >= 16 { statuses.removeAll() }
        statuses[packet.header.requestID] = status
        if let (id, waiter) = statusWaiter, id == packet.header.requestID {
            waiter.finish(.success(status)); statusWaiter = nil
            statuses.removeValue(forKey: id)
        }
    }

    public func didWriteValue(for characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == BLEProtocol.controlUUID else { return }
        if let error { writeWaiter?.finish(.failure(error)) }
        else { writeWaiter?.finish(.success(())) }
        writeWaiter = nil
    }

    public func didModifyServices(_ services: [CBService]) {
        guard services.contains(where: { $0.uuid == BLEProtocol.serviceUUID }) else { return }
        prepared = false
        failPending(SDExportClientError.servicesChanged)
    }

    public func disconnected() {
        prepared = false
        failPending(SDExportClientError.bleDisconnected)
        // The HTTP snapshot remains valid until completion/cancel/lease expiry.
    }

    private func requestID() -> UInt32 {
        let result = nextID
        nextID = nextID == 0x7fffffff ? 1 : nextID + 1
        return result
    }

    private func read(_ characteristic: CBCharacteristic) async throws -> Data {
        try await wait(timeout: 10, install: { readWaiter = (characteristic.uuid, $0) }) {
            peripheral.readValue(for: characteristic)
        }
    }

    private func write(_ data: Data, requestID: UInt32) async throws {
        guard let control, peripheral.state == .connected else { throw SDExportClientError.bleDisconnected }
        guard data.count <= peripheral.maximumWriteValueLength(for: .withResponse) else {
            throw SDExportClientError.unsupportedDevice
        }
        let _: Void = try await wait(timeout: 10, install: { writeWaiter = $0 }) {
            peripheral.writeValue(data, for: control, type: .withResponse)
        }
        // ERROR can arrive before the ATT write acknowledgement; retain it
        // until that acknowledgement, then fail without a status timeout.
        try throwRequestError(requestID)
    }

    private func throwRequestError(_ id: UInt32) throws {
        if let code = requestErrors.removeValue(forKey: id) { throw SDExportClientError.device(code) }
    }

    private func finishRequest(_ id: UInt32) {
        issuedRequestIDs.remove(id)
        requestErrors.removeValue(forKey: id)
        statuses.removeValue(forKey: id)
    }

    private func wait<T>(timeout: TimeInterval, install: (ExportBLEWaiter<T>) -> Void,
                         issue: () -> Void) async throws -> T {
        try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                let waiter = ExportBLEWaiter(continuation)
                install(waiter)
                waiter.timer = Task { @MainActor [weak waiter] in
                    do { try await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000)) }
                    catch { return }
                    waiter?.finish(.failure(SDExportClientError.bleTimeout))
                }
                issue()
            }
        } onCancel: {
            Task { @MainActor in self.failPending(CancellationError()) }
        }
    }

    private func failPending(_ error: Error) {
        servicesWaiter?.finish(.failure(error)); servicesWaiter = nil
        characteristicsWaiter?.finish(.failure(error)); characteristicsWaiter = nil
        notifyWaiter?.finish(.failure(error)); notifyWaiter = nil
        readWaiter?.1.finish(.failure(error)); readWaiter = nil
        writeWaiter?.finish(.failure(error)); writeWaiter = nil
        statusWaiter?.1.finish(.failure(error)); statusWaiter = nil
    }
}

/// Generic firmware ERROR replies remain visible to the App's original delegate;
/// only currently issued export request IDs affect this link's async operation.
struct SDExportControlError {
    let requestID: UInt32
    let code: UInt16

    static func decode(_ packet: Packet, issuedRequestIDs: Set<UInt32>) -> Self? {
        guard packet.header.messageType == .error,
              issuedRequestIDs.contains(packet.header.requestID), packet.payload.count >= 2 else { return nil }
        return Self(requestID: packet.header.requestID,
                    code: UInt16(packet.payload[0]) | UInt16(packet.payload[1]) << 8)
    }
}

@MainActor
private final class ExportBLEWaiter<T> {
    private var continuation: CheckedContinuation<T, Error>?
    var timer: Task<Void, Never>?
    init(_ continuation: CheckedContinuation<T, Error>) { self.continuation = continuation }
    func finish(_ result: Result<T, Error>) {
        guard let continuation else { return }
        self.continuation = nil
        timer?.cancel(); timer = nil
        continuation.resume(with: result)
    }
}
