import Foundation
import CoreBluetooth

final class BLEManager: NSObject, ObservableObject {
    static let serviceUUID = CBUUID(string: "6E0A0001-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    static let navCharUUID = CBUUID(string: "6E0A0002-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    static let mapCharUUID = CBUUID(string: "6E0A0003-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    static let infoCharUUID = CBUUID(string: "6E0A0004-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    static let otaCharUUID = CBUUID(string: "6E0A0005-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    private static let savedPeripheralKey = "motonav.peripheral"

    @Published private(set) var isConnected = false
    @Published private(set) var statusText = "Procurando a placa..."
    @Published private(set) var uploadProgress: Double?
    @Published private(set) var boardFirmware: Int?
    @Published private(set) var otaRunning = false
    @Published private(set) var otaProgress: Double = 0
    @Published private(set) var otaMessage: String?

    var onReady: (() -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var navChar: CBCharacteristic?
    private var mapChar: CBCharacteristic?
    private var infoChar: CBCharacteristic?
    private var otaChar: CBCharacteristic?
    private var bulkTarget: CBCharacteristic?
    private var otaTimeout: DispatchWorkItem?
    private var lastNavPacket: Data?
    private var bulkQueue: [Data] = []
    private var bulkTotal = 0
    private var bulkDone: (() -> Void)?
    private var pendingBlobs: [(data: Data, ota: Bool, done: (() -> Void)?)] = []
    private var blobId: UInt16 = 1

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    var maxPayload: Int {
        guard let p = peripheral else { return 20 }
        return min(244, max(20, p.maximumWriteValueLength(for: .withoutResponse)))
    }

    func send(_ data: Data) {
        lastNavPacket = data
        write(data, to: navChar)
    }

    func sendPosition(_ data: Data) {
        guard bulkQueue.isEmpty else { return }
        write(data, to: navChar)
    }

    private func write(_ data: Data, to c: CBCharacteristic?) {
        guard let p = peripheral, let c = c, p.state == .connected else { return }
        let payload = data.prefix(maxPayload)
        if c.properties.contains(.writeWithoutResponse) && p.canSendWriteWithoutResponse {
            p.writeValue(payload, for: c, type: .withoutResponse)
        } else if c.properties.contains(.write) {
            p.writeValue(payload, for: c, type: .withResponse)
        }
    }

    func sendMap(blob: Data, priority: Bool, completion: (() -> Void)? = nil) {
        guard isConnected, mapChar != nil else { return }
        if priority {
            pendingBlobs.insert((data: blob, ota: false, done: completion), at: 0)
        } else {
            pendingBlobs.append((data: blob, ota: false, done: completion))
        }
        startNextBlob()
    }

    var supportsOTA: Bool { otaChar != nil }

    func sendFirmware(_ image: Data) {
        guard isConnected, otaChar != nil, !otaRunning else { return }
        otaRunning = true
        otaProgress = 0
        otaMessage = "Enviando..."
        pendingBlobs.insert((data: image, ota: true, done: { [weak self] in
            self?.otaMessage = "Gravando na placa..."
            self?.armOtaTimeout(20)
        }), at: 0)
        startNextBlob()
    }

    private func armOtaTimeout(_ seconds: Double) {
        otaTimeout?.cancel()
        let w = DispatchWorkItem { [weak self] in
            guard let self = self, self.otaRunning else { return }
            self.finishOta(ok: false, message: "A placa não respondeu")
        }
        otaTimeout = w
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds, execute: w)
    }

    private func finishOta(ok: Bool, message: String) {
        otaTimeout?.cancel()
        otaRunning = false
        otaMessage = message
        if ok { otaProgress = 1 }
    }

    private func handleInfo(_ text: String) {
        if text.hasPrefix("fw=") {
            boardFirmware = Int(text.dropFirst(3))
        } else if text == "ota=ok" {
            finishOta(ok: true, message: "Placa atualizada. Reiniciando...")
        } else if text.hasPrefix("ota=err") {
            bulkQueue.removeAll()
            bulkTotal = 0
            uploadProgress = nil
            finishOta(ok: false, message: "Falha na atualização (\(text.dropFirst(8)))")
            startNextBlob()
        }
    }

    private func startNextBlob() {
        guard bulkQueue.isEmpty, bulkTotal == 0, !pendingBlobs.isEmpty else { return }
        let next = pendingBlobs.removeFirst()
        let blob = next.data
        if next.ota {
            var packets: [Data] = []
            var begin = Data([0x20])
            append(&begin, UInt32(blob.count))
            packets.append(begin)
            let chunk = max(1, maxPayload - 5)
            var off = 0
            while off < blob.count {
                let end = min(blob.count, off + chunk)
                var p = Data([0x21])
                append(&p, UInt32(off))
                p.append(blob.subdata(in: off..<end))
                packets.append(p)
                off = end
            }
            packets.append(Data([0x22]))
            bulkQueue = packets
            bulkTotal = packets.count
            bulkDone = next.done
            bulkTarget = otaChar
            pump()
            return
        }
        bulkTarget = mapChar
        blobId = blobId == UInt16.max ? 1 : blobId + 1
        let id = blobId
        var packets: [Data] = []
        var begin = Data([0x10])
        append(&begin, id)
        append(&begin, UInt32(blob.count))
        packets.append(begin)
        let chunk = max(1, maxPayload - 7)
        var off = 0
        while off < blob.count {
            let end = min(blob.count, off + chunk)
            var p = Data([0x11])
            append(&p, id)
            append(&p, UInt32(off))
            p.append(blob.subdata(in: off..<end))
            packets.append(p)
            off = end
        }
        var endPkt = Data([0x12])
        append(&endPkt, id)
        packets.append(endPkt)
        bulkQueue = packets
        bulkTotal = packets.count
        bulkDone = next.done
        uploadProgress = 0
        pump()
    }

    private func pump() {
        guard let p = peripheral, let c = bulkTarget, p.state == .connected else { return }
        while !bulkQueue.isEmpty && p.canSendWriteWithoutResponse {
            p.writeValue(bulkQueue.removeFirst(), for: c, type: .withoutResponse)
        }
        if bulkTotal > 0 {
            let progress = Double(bulkTotal - bulkQueue.count) / Double(bulkTotal)
            if c === otaChar {
                otaProgress = progress
            } else {
                uploadProgress = progress
            }
        }
        if bulkQueue.isEmpty && bulkTotal > 0 {
            bulkTotal = 0
            uploadProgress = nil
            let done = bulkDone
            bulkDone = nil
            done?()
            startNextBlob()
        }
    }

    private func append<T: FixedWidthInteger>(_ d: inout Data, _ v: T) {
        withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) }
    }

    private func startScan() {
        guard central.state == .poweredOn else { return }
        if let idString = UserDefaults.standard.string(forKey: Self.savedPeripheralKey),
           let id = UUID(uuidString: idString),
           let known = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(known)
            return
        }
        let connected = central.retrieveConnectedPeripherals(withServices: [Self.serviceUUID])
        if let p = connected.first {
            connect(p)
            return
        }
        statusText = "Procurando a placa..."
        central.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
    }

    private func connect(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        statusText = "Conectando..."
        central.connect(p, options: nil)
    }

    private func resetLink() {
        isConnected = false
        navChar = nil
        mapChar = nil
        infoChar = nil
        otaChar = nil
        bulkTarget = nil
        if otaRunning && otaMessage != "Placa atualizada. Reiniciando..." {
            otaRunning = false
            otaMessage = "Conexão perdida durante a atualização"
        }
        bulkQueue = []
        bulkTotal = 0
        bulkDone = nil
        pendingBlobs = []
        uploadProgress = nil
    }

    func forgetDevice() {
        UserDefaults.standard.removeObject(forKey: Self.savedPeripheralKey)
        if let p = peripheral { central.cancelPeripheralConnection(p) }
        peripheral = nil
        resetLink()
        startScan()
    }
}

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            startScan()
        case .unauthorized:
            statusText = "Permita o Bluetooth nos Ajustes"
        case .poweredOff:
            statusText = "Bluetooth desligado"
            resetLink()
        default:
            statusText = "Bluetooth indisponível"
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        central.stopScan()
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.savedPeripheralKey)
        statusText = "Conectando..."
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        resetLink()
        statusText = "Falha ao conectar"
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.startScan() }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        resetLink()
        statusText = "Placa desconectada"
        central.connect(peripheral, options: nil)
    }
}

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            statusText = "Placa sem o serviço MotoNav"
            return
        }
        peripheral.discoverCharacteristics([Self.navCharUUID, Self.mapCharUUID, Self.infoCharUUID, Self.otaCharUUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let c = service.characteristics?.first(where: { $0.uuid == Self.navCharUUID }) else { return }
        navChar = c
        mapChar = service.characteristics?.first(where: { $0.uuid == Self.mapCharUUID })
        infoChar = service.characteristics?.first(where: { $0.uuid == Self.infoCharUUID })
        otaChar = service.characteristics?.first(where: { $0.uuid == Self.otaCharUUID })
        boardFirmware = nil
        if let ic = infoChar {
            peripheral.setNotifyValue(true, for: ic)
            peripheral.readValue(for: ic)
        }
        isConnected = true
        statusText = mapChar == nil ? "Placa conectada (atualize o firmware)" : "Placa conectada"
        if let last = lastNavPacket { send(last) }
        onReady?()
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        pump()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.infoCharUUID, let v = characteristic.value,
              let text = String(data: v, encoding: .utf8) else { return }
        handleInfo(text)
    }
}
