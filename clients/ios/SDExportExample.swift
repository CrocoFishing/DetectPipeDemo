import Foundation
import CoreBluetooth
import UIKit
// Add `import EventPipeSDExport` when this file is in an App using the package.

/// Copy into the App's existing BLE owner; it deliberately does not create a
/// second CBCentralManager or replace the App's recognition/result handling.
/// The existing central manager must deliver callbacks on the main queue.
@MainActor
final class SDExportExample: NSObject, CBPeripheralDelegate {
    private let link: SDExportBLELink
    private let coordinator: SDExportCoordinator
    private var exportTask: Task<Void, Never>?
    private var backgroundObserver: NSObjectProtocol?
    var showProgress: ((SDExportProgress) -> Void)?
    var showResult: ((Result<SDExportResult, Error>) -> Void)?

    init(connectedPeripheral: CBPeripheral) {
        link = SDExportBLELink(peripheral: connectedPeripheral)
        coordinator = SDExportCoordinator(link: link)
        super.init()
        // Forward these methods from your existing delegate. If this example is
        // the sole peripheral owner, assign `connectedPeripheral.delegate = self`.
        backgroundObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.cancel() }
        }
    }

    deinit {
        exportTask?.cancel()
        if let backgroundObserver { NotificationCenter.default.removeObserver(backgroundObserver) }
    }

    func start() {
        guard exportTask == nil else { return }
        // Keep a stable, per-device directory across invocations. Real Apps
        // should key it by their paired device identity, not the current session.
        let root = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SDExports/" + link.peripheral.identifier.uuidString, isDirectory: true)
        exportTask = Task { [weak self] in
            guard let self else { return }
            defer { self.exportTask = nil }
            do {
                let result = try await coordinator.exportAll(to: root) { [weak self] update in
                    Task { @MainActor in self?.showProgress?(update) }
                }
                showResult?(.success(result))
            } catch { showResult?(.failure(error)) }
        }
    }

    func cancel() { exportTask?.cancel() }

    // Call from centralManager(_:didDisconnectPeripheral:error:).
    func disconnected() { link.disconnected() }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        link.didDiscoverServices(error: error)
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        link.didDiscoverCharacteristics(for: service, error: error)
    }
    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        link.didUpdateNotificationState(for: characteristic, error: error)
    }
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        link.didUpdateValue(for: characteristic, error: error)
        // Continue forwarding ordinary Event/image values to existing App logic.
    }
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        link.didWriteValue(for: characteristic, error: error)
    }
    func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        link.didModifyServices(invalidatedServices)
    }
}
