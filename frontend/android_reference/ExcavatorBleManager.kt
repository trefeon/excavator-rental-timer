package com.excavator.rental.ble

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.Context
import android.util.Log
import java.util.UUID

/**
 * Excavator Rental Timer - Android BLE SDK Reference
 * 
 * This file serves as a reference implementation for Android developers
 * who need to integrate the ESP32 Excavator Timer into a native Kotlin app.
 * It uses the standard Android BluetoothGatt API.
 * 
 * Prerequisites:
 * - BLUETOOTH_SCAN, BLUETOOTH_CONNECT, ACCESS_FINE_LOCATION permissions in AndroidManifest.xml
 */

// UUIDs mapped directly from docs/BLE_PROTOCOL_SPEC.md
object ExcavatorUUIDs {
    val SERVICE_UUID = UUID.fromString("7b7d0001-8f2a-4f6b-9b2e-2f3ad5a10001")
    val STATE_CHAR_UUID = UUID.fromString("7b7d0002-8f2a-4f6b-9b2e-2f3ad5a10001")
    val COMMAND_CHAR_UUID = UUID.fromString("7b7d0003-8f2a-4f6b-9b2e-2f3ad5a10001")
    val ACK_CHAR_UUID = UUID.fromString("7b7d0004-8f2a-4f6b-9b2e-2f3ad5a10001")
    
    // Standard BLE Descriptor for enabling notifications
    val CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
}

data class ExcavatorState(
    val state: String,
    val remainingSeconds: Int,
    val display: String,
    val battery: String
)

@SuppressLint("MissingPermission") // Production apps should handle permissions via Activity/Fragment
class ExcavatorBleManager(private val context: Context) {

    private val bluetoothManager: BluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter
    private var bluetoothGatt: BluetoothGatt? = null
    
    private var commandIdCounter = 100
    
    // Callbacks to update the UI
    var onStateUpdated: ((ExcavatorState) -> Unit)? = null
    var onConnectionStateChanged: ((Boolean) -> Unit)? = null

    /**
     * 1. Connect to the ESP32 Device by MAC Address
     */
    fun connect(macAddress: String) {
        val device = bluetoothAdapter?.getRemoteDevice(macAddress)
        if (device == null) {
            Log.e("ExcavatorBLE", "Device not found for MAC: $macAddress")
            return
        }
        
        Log.d("ExcavatorBLE", "Connecting to ${device.name} ($macAddress)...")
        // autoConnect = false ensures faster immediate connection
        bluetoothGatt = device.connectGatt(context, false, gattCallback)
    }

    /**
     * 2. Disconnect and release resources
     */
    fun disconnect() {
        bluetoothGatt?.disconnect()
        bluetoothGatt?.close()
        bluetoothGatt = null
    }

    /**
     * 3. Public API Methods for UI Buttons
     */
    fun addTime(minutes: Int) {
        val seconds = minutes * 60
        sendCommand("ADD_TIME", seconds.toString())
    }

    fun pause() {
        sendCommand("PAUSE", "0")
    }

    fun resume() {
        sendCommand("RESUME", "0")
    }

    fun stop() {
        sendCommand("STOP", "0")
    }

    /**
     * Internal helper to write to the Command Characteristic
     */
    private fun sendCommand(command: String, value: String) {
        commandIdCounter++
        
        // Protocol Format: v1|command_id|command|value|session_id|nonce|signature
        // Note: We use "debug" for signature as per ALLOW_UNSIGNED_DEBUG_COMMANDS in firmware
        val payload = "v1|$commandIdCounter|$command|$value|APP-SESSION|debug|debug"
        
        val service = bluetoothGatt?.getService(ExcavatorUUIDs.SERVICE_UUID)
        val char = service?.getCharacteristic(ExcavatorUUIDs.COMMAND_CHAR_UUID)
        
        if (char != null) {
            char.value = payload.toByteArray(Charsets.UTF_8)
            bluetoothGatt?.writeCharacteristic(char)
            Log.d("ExcavatorBLE", "Sent BLE Command: $payload")
        } else {
            Log.e("ExcavatorBLE", "Error: Command characteristic not found. Are you connected?")
        }
    }

    /**
     * 4. GATT Callback Handler
     * Listens for connection changes and incoming notifications from ESP32
     */
    private val gattCallback = object : BluetoothGattCallback() {
        
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.d("ExcavatorBLE", "Connected to GATT server. Discovering services...")
                onConnectionStateChanged?.invoke(true)
                
                // We MUST discover services before we can read/write characteristics
                gatt.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.d("ExcavatorBLE", "Disconnected from GATT server.")
                onConnectionStateChanged?.invoke(false)
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d("ExcavatorBLE", "Services discovered. Subscribing to State characteristic...")
                
                // Subscribe to state notifications
                val service = gatt.getService(ExcavatorUUIDs.SERVICE_UUID)
                val stateChar = service?.getCharacteristic(ExcavatorUUIDs.STATE_CHAR_UUID)
                
                if (stateChar != null) {
                    gatt.setCharacteristicNotification(stateChar, true)
                    
                    // Write to the CCCD descriptor to tell the ESP32 to start pushing notifications
                    val descriptor = stateChar.getDescriptor(ExcavatorUUIDs.CCCD_UUID)
                    if (descriptor != null) {
                        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        gatt.writeDescriptor(descriptor)
                    }
                }
            }
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            // Fired automatically every second by the ESP32 pushing State notifications
            if (characteristic.uuid == ExcavatorUUIDs.STATE_CHAR_UUID) {
                val rawData = String(characteristic.value, Charsets.UTF_8)
                parseStatePayload(rawData)
            }
        }
        
        private fun parseStatePayload(payload: String) {
            // Example payload: v1;toy=EXC-01;state=RUNNING;rem=299;disp=04:59;paid=300;bat=OK;fault=0;seq=1;sid=S1
            val parts = payload.split(";")
            var state = "UNKNOWN"
            var rem = 0
            var disp = ""
            var bat = ""
            
            parts.forEach {
                when {
                    it.startsWith("state=") -> state = it.substringAfter("=")
                    it.startsWith("rem=") -> rem = it.substringAfter("=").toIntOrNull() ?: 0
                    it.startsWith("disp=") -> disp = it.substringAfter("=")
                    it.startsWith("bat=") -> bat = it.substringAfter("=")
                }
            }
            
            val parsedState = ExcavatorState(state, rem, disp, bat)
            
            // In a real app, you should dispatch this to the Main UI Thread 
            // e.g. using runOnUiThread { } or Kotlin Coroutine's Dispatchers.Main
            onStateUpdated?.invoke(parsedState)
        }
    }
}
