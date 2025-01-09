package dev.thermion.android

import android.app.Activity
import android.content.res.AssetManager
import android.graphics.*
import android.os.Build
import android.util.Log
import android.view.Surface
import androidx.annotation.NonNull
import androidx.annotation.Keep
import androidx.annotation.RequiresApi
import androidx.lifecycle.Lifecycle
import io.flutter.FlutterInjector
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding
import io.flutter.embedding.engine.plugins.lifecycle.HiddenLifecycleReference
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry.SurfaceTextureEntry
import java.io.File
import java.nio.ByteBuffer

@Keep
class ThermionFlutterPlugin : FlutterPlugin, MethodChannel.MethodCallHandler, ActivityAware {
    companion object {
        const val CHANNEL_NAME = "dev.thermion.flutter/event"
        const val TAG = "FilamentPlugin"
    }

    init {
        System.loadLibrary("thermion_flutter_android")
    }

    external fun getNativeWindowFromSurface(surface: Any): Long
    external fun makeResourceLoaderWrapper() : Long

    private lateinit var channel: MethodChannel
    private lateinit var flutterPluginBinding: FlutterPlugin.FlutterPluginBinding
    private var lifecycle: Lifecycle? = null
    private lateinit var activity: Activity

    private data class TextureEntry(
        val surfaceTextureEntry: SurfaceTextureEntry,
        val surfaceTexture: SurfaceTexture,
        val surface: Surface
    )

    var _surfaceTexture: SurfaceTexture? = null
    private var _surfaceTextureEntry: SurfaceTextureEntry? = null
    var _surface: Surface? = null
    private val textures: MutableMap<Long, TextureEntry> = mutableMapOf()

    private var nativePtr: Long = 0

    @JvmName("loadResourceFromOwner")
    public final fun loadResourceFromOwner(path: String, owner: Long) : ByteBuffer? {
        Log.i("thermion_flutter", "Loading resource from path $path")
        var fileData: ByteArray? = null
        
        when {
            path?.startsWith("file://") == true -> {
                fileData = File(path.substring(6)).readBytes()
            }
            else -> {
                var assetPath = path
                if (assetPath?.startsWith("asset://") == true) {
                    assetPath = assetPath.substring(8)
                }
                val loader = FlutterInjector.instance().flutterLoader()
                val key = loader.getLookupKeyForAsset(assetPath!!)
                
                fileData = try {
                            activity.assets.open(key).readBytes().also {
                                Log.i("thermion_flutter", "Loaded ${it.size} bytes")
                            }
                        } catch (e: Exception) {
                            Log.e("thermion_flutter", "Failed to open asset at $assetPath", e)
                            null
                        }
  
            }
        }

        return if (fileData != null) {
            val buffer = ByteBuffer.allocateDirect(fileData.size)
            buffer.put(fileData)
            buffer.flip()
            buffer
        } else {
            null
        }
    }


    @RequiresApi(Build.VERSION_CODES.M)
    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "createTexture" -> {
                val args = call.arguments as List<*>
                val width = args[0] as Int
                val height = args[1] as Int
                
                if (width < 1 || height < 1) {
                    result.error("DIMENSION_MISMATCH", 
                        "Both dimensions must be greater than zero (you provided $width x $height)", null)
                    return
                }
                
                Log.i("thermion_flutter", "Creating SurfaceTexture ${width}x${height}")
                
                val surfaceTextureEntry = flutterPluginBinding.textureRegistry.createSurfaceTexture()
                val surfaceTexture = surfaceTextureEntry.surfaceTexture()
                surfaceTexture.setDefaultBufferSize(width, height)
                
                val surface = Surface(surfaceTexture)
                
                if (!surface.isValid) {
                    result.error("SURFACE_INVALID", "Failed to create valid surface", null)
                } else {
                    val flutterTextureId = surfaceTextureEntry.id()
                    textures[flutterTextureId] = TextureEntry(surfaceTextureEntry, surfaceTexture, surface)
                    val nativeWindow = getNativeWindowFromSurface(surface)
                    result.success(listOf(flutterTextureId, flutterTextureId, nativeWindow))
                }
            }
            "destroyTexture" -> {
                val textureId = (call.arguments as Int).toLong()
                textures[textureId]?.let { entry ->
                    entry.surface.release()
                    entry.surfaceTextureEntry.release()
                    textures.remove(textureId)
                    result.success(true)
                } ?: result.error("TEXTURE_NOT_FOUND", "Texture with id $textureId not found", null)
            }
            "markTextureFrameAvailable" -> {
                val textureId = (call.arguments as Int).toLong()
                if (textures.containsKey(textureId)) {
                    result.success(null)
                } else {
                    result.error("TEXTURE_NOT_FOUND", "Texture with id $textureId not found", null)
                }
            }
            "getResourceLoaderWrapper" -> {
                val resourceLoader = makeResourceLoaderWrapper()
                result.success(resourceLoader)
            }
            "getDriverPlatform" -> {
                result.success(null)
            }
            "getSharedContext" -> {
                result.success(null)
            }
            "getRenderCallback" -> {
                result.success(listOf(0, 0))
            }
            else -> {
                result.notImplemented()
            }
        }
    }

    override fun onAttachedToEngine(@NonNull flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        this.flutterPluginBinding = flutterPluginBinding
        channel = MethodChannel(flutterPluginBinding.binaryMessenger, CHANNEL_NAME)
        channel.setMethodCallHandler(this)
    }

    override fun onAttachedToActivity(binding: ActivityPluginBinding) {
        lifecycle = (binding.lifecycle as? HiddenLifecycleReference)?.lifecycle
        activity = binding.activity
        activity.window.setFormat(PixelFormat.RGBA_8888)
    }

    override fun onDetachedFromEngine(@NonNull binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
        // Release all textures
        textures.values.forEach { entry ->
            entry.surface.release()
            entry.surfaceTextureEntry.release()
        }
        textures.clear()
    }

    override fun onReattachedToActivityForConfigChanges(binding: ActivityPluginBinding) {
        onAttachedToActivity(binding)
    }

    override fun onDetachedFromActivityForConfigChanges() {
        onDetachedFromActivity()
    }

    override fun onDetachedFromActivity() {
        lifecycle = null
    }
}