package com.excavator.rental.wifi

import android.util.Log
import okhttp3.*
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException

/**
 * Excavator Rental Timer - Android Wi-Fi SDK Reference (Centralized Master API)
 * 
 * All requests are sent to the Master IP (192.168.4.1).
 * The Master handles querying and commanding individual slave toys automatically.
 */

data class ExcavatorState(
    val toyId: Int,
    val ip: String,
    val mac: String,
    val isOnline: Boolean,
    val state: String,
    val remainingSeconds: Int,
    val display: String,
    val paidSeconds: Int,
    val battery: String
)

class ExcavatorWifiManager {

    private val masterIp = "192.168.4.1"
    private val client = OkHttpClient.Builder()
        .connectTimeout(2, java.util.concurrent.TimeUnit.SECONDS)
        .readTimeout(2, java.util.concurrent.TimeUnit.SECONDS)
        .build()

    private val JSON_MEDIA_TYPE = "application/json; charset=utf-8".toMediaType()

    /**
     * 1. Get the current status of ALL toys from the Master
     * Call this every 1 second to update your RecyclerView/UI
     */
    fun getAllToysState(onResult: (List<ExcavatorState>?) -> Unit) {
        val request = Request.Builder()
            .url("http://$masterIp/api/slaves")
            .get()
            .build()

        client.newCall(request).enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                Log.e("ExcavatorWIFI", "Failed to fetch toys: ${e.message}")
                onResult(null)
            }

            override fun onResponse(call: Call, response: Response) {
                if (response.isSuccessful) {
                    val responseBody = response.body?.string()
                    if (responseBody != null) {
                        try {
                            val jsonArray = JSONArray(responseBody)
                            val toysList = mutableListOf<ExcavatorState>()
                            
                            for (i in 0 until jsonArray.length()) {
                                val obj = jsonArray.getJSONObject(i)
                                toysList.add(
                                    ExcavatorState(
                                        toyId = obj.optInt("id", 0),
                                        ip = obj.optString("ip", ""),
                                        mac = obj.optString("mac", ""),
                                        isOnline = obj.optBoolean("online", false),
                                        state = obj.optString("state", "UNKNOWN"),
                                        remainingSeconds = obj.optInt("rem", 0),
                                        display = obj.optString("disp", "--:--"),
                                        paidSeconds = obj.optInt("paid", 0),
                                        battery = obj.optString("bat", "UNKNOWN")
                                    )
                                )
                            }
                            onResult(toysList)
                        } catch (e: Exception) {
                            Log.e("ExcavatorWIFI", "JSON Parsing error", e)
                            onResult(null)
                        }
                    } else {
                        onResult(null)
                    }
                } else {
                    onResult(null)
                }
                response.close()
            }
        })
    }

    /**
     * 2. Send Control Command to a specific toy via the Master
     */
    fun sendCommand(toyNumericId: Int, command: String, value: Int, onComplete: (Boolean) -> Unit) {
        // JSON Payload: {"id":1, "cmd":"ADD_TIME", "val":300}
        val jsonPayload = JSONObject().apply {
            put("id", toyNumericId)
            put("cmd", command)
            put("val", value)
        }.toString()

        val body = jsonPayload.toRequestBody(JSON_MEDIA_TYPE)
        val request = Request.Builder()
            .url("http://$masterIp/api/command")
            .post(body)
            .build()

        client.newCall(request).enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                Log.e("ExcavatorWIFI", "Command failed: ${e.message}")
                onComplete(false)
            }

            override fun onResponse(call: Call, response: Response) {
                onComplete(response.isSuccessful)
                response.close()
            }
        })
    }

    // --- Public API Wrappers ---

    fun addTime(toyNumericId: Int, minutes: Int, onComplete: (Boolean) -> Unit) {
        val seconds = minutes * 60
        sendCommand(toyNumericId, "ADD_TIME", seconds, onComplete)
    }

    fun pause(toyNumericId: Int, onComplete: (Boolean) -> Unit) {
        sendCommand(toyNumericId, "PAUSE", 0, onComplete)
    }

    fun resume(toyNumericId: Int, onComplete: (Boolean) -> Unit) {
        sendCommand(toyNumericId, "RESUME", 0, onComplete)
    }

    fun stop(toyNumericId: Int, onComplete: (Boolean) -> Unit) {
        sendCommand(toyNumericId, "STOP", 0, onComplete)
    }
}
