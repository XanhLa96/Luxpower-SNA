#!/usr/bin/env node
'use strict';

const net = require('net');

// Change protocol selection to CLI arg or default
const defaultProtocol = parseInt(process.argv[2] || '1', 10);
if (![1,2].includes(defaultProtocol)) {
    console.error('Invalid protocol. Use 1 or 2.');
    process.exit(1);
}
console.log('Using TCP protocol:', defaultProtocol);

// Add default protocol and default datalog SN
const DEFAULT_DATALOG_SN = Buffer.alloc(10, 0xFF);
// Empty inverter SN (10 null bytes) for commands
const EMPTY_INVERTER_SN = Buffer.alloc(10, 0x00);
// Interval between reads (ms)
const READ_INTERVAL = 5000;

// Modbus CRC16 calculation
function crc16Modbus(buffer, length) {
    let crc = 0xFFFF;
    for (let pos = 0; pos < length; pos++) {
        crc ^= buffer[pos] & 0xFF;
        for (let i = 0; i < 8; i++) {
            if ((crc & 0x0001) !== 0) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc & 0xFFFF;
}

// Build Read Input Command (modbus) buffer
function buildReadInputCommandBuffer(serialBuffer, startAddress, pointNumber) {
    const cmd = Buffer.alloc(18);
    cmd.writeUInt8(0, 0); // address
    cmd.writeUInt8(4, 1); // function code R_INPUT (4)
    serialBuffer.copy(cmd, 2, 0, 10); // datalogSn
    // start address
    cmd.writeUInt8(startAddress & 0xFF, 12);
    cmd.writeUInt8((startAddress >> 8) & 0xFF, 13);
    // point number
    cmd.writeUInt8(pointNumber & 0xFF, 14);
    cmd.writeUInt8((pointNumber >> 8) & 0xFF, 15);
    // CRC16
    const crc = crc16Modbus(cmd, 16);
    cmd.writeUInt8(crc & 0xFF, 16);      // CRC low
    cmd.writeUInt8((crc >> 8) & 0xFF, 17);  // CRC high
    return cmd;
}

// Build Transfer Data buffer (datalogSn + length + command)
function buildTransferDataBuffer(datalogSnBuf, commandBuf) {
    const payloadLen = commandBuf.length;
    const buf = Buffer.alloc(10 + 2 + payloadLen);
    // datalogSn
    datalogSnBuf.copy(buf, 0, 0, 10);
    // length
    buf.writeUInt8(payloadLen & 0xFF, 10);
    buf.writeUInt8((payloadLen >> 8) & 0xFF, 11);
    // command
    commandBuf.copy(buf, 12);
    return buf;
}

// Build TCP Frame (AbstractTcpDataFrame)
function buildTcpFrame(protocol, functionCode, dataBuf) {
    const payloadLen = dataBuf.length + 2;
    const frame = Buffer.alloc(8 + dataBuf.length);
    frame.writeUInt8(0xA1, 0); // prefix0
    frame.writeUInt8(0x1A, 1); // prefix1
    // protocol
    frame.writeUInt8(protocol & 0xFF, 2);
    frame.writeUInt8((protocol >> 8) & 0xFF, 3);
    // length
    frame.writeUInt8(payloadLen & 0xFF, 4);
    frame.writeUInt8((payloadLen >> 8) & 0xFF, 5);
    frame.writeUInt8(1, 6);
    frame.writeUInt8(functionCode & 0xFF, 7);
    dataBuf.copy(frame, 8);
    return frame;
}

// Parser state
let parserBuffer = Buffer.alloc(0);
const datalogSnBuf = DEFAULT_DATALOG_SN;

// Function to send a read input registers request
function sendReadInput() {
    const cmdBuf = buildReadInputCommandBuffer(EMPTY_INVERTER_SN, 0, 40);
    const tdBuf = buildTransferDataBuffer(datalogSnBuf, cmdBuf);
    const tcpBuf = buildTcpFrame(defaultProtocol, 194, tdBuf);
    client.write(tcpBuf);
    console.log('Sent read input registers request');
}

// Connect to inverter
const client = net.createConnection({ host: '10.10.10.1', port: 8000 }, () => {
    console.log('Connected to inverter at 10.10.10.1:8000');
    // initial read and start periodic polling
    sendReadInput();
    setInterval(sendReadInput, READ_INTERVAL);
});

client.on('data', (chunk) => {
    parserBuffer = Buffer.concat([parserBuffer, chunk]);
    parseFrames();
});

client.on('close', () => {
    console.log('Connection closed');
    process.exit(0);
});

client.on('error', (err) => {
    console.error('Connection error:', err);
    process.exit(1);
});

// Parse incoming frames
function parseFrames() {
    while (true) {
        if (parserBuffer.length < 2) break;
        // sync prefix
        if (parserBuffer[0] !== 0xA1 || parserBuffer[1] !== 0x1A) {
            const idx = parserBuffer.indexOf(0xA1, 1);
            if (idx < 0) {
                parserBuffer = Buffer.alloc(0);
                break;
            }
            parserBuffer = parserBuffer.slice(idx);
            continue;
        }
        if (parserBuffer.length < 6) break;
        const len = parserBuffer[5] * 256 + parserBuffer[4];
        const fullLen = len + 6;
        if (parserBuffer.length < fullLen) break;
        const frame = parserBuffer.slice(0, fullLen);
        parserBuffer = parserBuffer.slice(fullLen);
        handleFrame(frame);
    }
}

// Handle a complete TCP frame
function handleFrame(frame) {
    // Debug: print raw frame data as hex
    console.log('Raw frame:', frame.toString('hex').match(/.{1,2}/g).join(' '));
    const fn = frame[7];
    if (fn === 194) { // TRANSLATE - read input response
        // Read registers as Java LocalOverviewFragment
        const r7 = getRegister2(frame, 7);
        const r8 = getRegister2(frame, 8);
        const r9 = getRegister2(frame, 9);
        const outInv = getRegister2(frame, 16);
        const inInv = getRegister2(frame, 17);
        const outGrid = getRegister2(frame, 26);
        const inGrid = getRegister2(frame, 27);

        // PV flow calculation: sum of three PV registers for non-AC charger
        const totalPv = r7 + r8 + r9;
        // Consumption: (outInv - inInv) + (inGrid - outGrid), floor at 0
        let consumption = (outInv - inInv) + (inGrid - outGrid);
        if (consumption < 0) consumption = 0;

        console.log('PV Flow:', totalPv + ' W');
        console.log('Consumption:', consumption + ' W');
        //client.end();
    }
}

// Mirror of FrameTool.getRegister2
function getRegister2(buf, index) {
    const p = (index * 2) + 35;
    if (p + 1 >= buf.length) return 0;
    const low = buf[p];
    const high = buf[p + 1];
    return (high << 8) + low;
} 
