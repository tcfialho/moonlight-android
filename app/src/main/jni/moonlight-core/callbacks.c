#include <jni.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdatomic.h>
static int nativeSubmit(void* du);
static bool nativeCodecActive(void);


#include <pthread.h>
#include <string.h>

#include <Limelight.h>

#include <opus_multistream.h>
#include <android/log.h>

#include <cpu-features.h>

static OpusMSDecoder* Decoder;
static OPUS_MULTISTREAM_CONFIGURATION OpusConfig;

static JavaVM *JVM;
static pthread_key_t JniEnvKey;
static pthread_once_t JniEnvKeyInitOnce = PTHREAD_ONCE_INIT;
static jclass GlobalBridgeClass;
static jmethodID BridgeDrSetupMethod;
static jmethodID BridgeDrStartMethod;
static jmethodID BridgeDrStopMethod;
static jmethodID BridgeDrCleanupMethod;
static jmethodID BridgeDrSubmitDecodeUnitMethod;
static jmethodID BridgeDrSubmitNativeMethod, BridgeDrSupportsNativeMethod;
static bool UseNativeInput;
static jmethodID BridgeArInitMethod;
static jmethodID BridgeArStartMethod;
static jmethodID BridgeArStopMethod;
static jmethodID BridgeArCleanupMethod;
static jmethodID BridgeArPlaySampleMethod;
static jmethodID BridgeClStageStartingMethod;
static jmethodID BridgeClStageCompleteMethod;
static jmethodID BridgeClStageFailedMethod;
static jmethodID BridgeClConnectionStartedMethod;
static jmethodID BridgeClConnectionTerminatedMethod;
static jmethodID BridgeClRumbleMethod;
static jmethodID BridgeClConnectionStatusUpdateMethod;
static jmethodID BridgeClSetHdrModeMethod;
static jmethodID BridgeClRumbleTriggersMethod;
static jmethodID BridgeClSetMotionEventStateMethod;
static jmethodID BridgeClSetControllerLEDMethod;
static jbyteArray DecodedFrameBuffer;
static jshortArray DecodedAudioBuffer;

void DetachThread(void* context) {
    (*JVM)->DetachCurrentThread(JVM);
}

void JniEnvKeyInit(void) {
    // Create a TLS slot for the JNIEnv. We aren't in
    // a pthread during init, so we must wait until we
    // are to initialize this.
    pthread_key_create(&JniEnvKey, DetachThread);
}

JNIEnv* GetThreadEnv(void) {
    JNIEnv* env;

    // First check if this is already attached to the JVM
    if ((*JVM)->GetEnv(JVM, (void**)&env, JNI_VERSION_1_4) == JNI_OK) {
        return env;
    }

    // Create the TLS slot now that we're safely in a pthread
    pthread_once(&JniEnvKeyInitOnce, JniEnvKeyInit);

    // Try the TLS to see if we already have a JNIEnv
    env = pthread_getspecific(JniEnvKey);
    if (env)
        return env;

    // This is the thread's first JNI call, so attach now
    (*JVM)->AttachCurrentThread(JVM, &env, NULL);

    // Write our JNIEnv to TLS, so we detach before dying
    pthread_setspecific(JniEnvKey, env);

    return env;
}

JNIEXPORT void JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_init(JNIEnv *env, jclass clazz) {
    (*env)->GetJavaVM(env, &JVM);
    GlobalBridgeClass = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "com/limelight/nvstream/jni/MoonBridge"));
    BridgeDrSupportsNativeMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeDrSupportsNativeInput", "()Z");
    BridgeDrSubmitNativeMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeDrSubmitNativeDecodeUnit", "(JIIICJJ)I");
    BridgeDrSetupMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeDrSetup", "(IIII)I");
    BridgeDrStartMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeDrStart", "()V");
    BridgeDrStopMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeDrStop", "()V");
    BridgeDrCleanupMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeDrCleanup", "()V");
    BridgeDrSubmitDecodeUnitMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeDrSubmitDecodeUnit", "([BIIIICJJ)I");
    BridgeArInitMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeArInit", "(III)I");
    BridgeArStartMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeArStart", "()V");
    BridgeArStopMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeArStop", "()V");
    BridgeArCleanupMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeArCleanup", "()V");
    BridgeArPlaySampleMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeArPlaySample", "([S)V");
    BridgeClStageStartingMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClStageStarting", "(I)V");
    BridgeClStageCompleteMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClStageComplete", "(I)V");
    BridgeClStageFailedMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClStageFailed", "(II)V");
    BridgeClConnectionStartedMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClConnectionStarted", "()V");
    BridgeClConnectionTerminatedMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClConnectionTerminated", "(I)V");
    BridgeClRumbleMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClRumble", "(SSS)V");
    BridgeClConnectionStatusUpdateMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClConnectionStatusUpdate", "(I)V");
    BridgeClSetHdrModeMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClSetHdrMode", "(Z[B)V");
    BridgeClRumbleTriggersMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClRumbleTriggers", "(SSS)V");
    BridgeClSetMotionEventStateMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClSetMotionEventState", "(SBS)V");
    BridgeClSetControllerLEDMethod = (*env)->GetStaticMethodID(env, clazz, "bridgeClSetControllerLED", "(SBBB)V");
}

static bool ensureScratchArray(JNIEnv* env, int size) {
    if (size < 0) return false;
    if (DecodedFrameBuffer && (*env)->GetArrayLength(env, DecodedFrameBuffer) >= size) return true;
    jbyteArray local = (*env)->NewByteArray(env, size);
    if (!local) return false;
    jbyteArray global = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!global) return false;
    if (DecodedFrameBuffer) (*env)->DeleteGlobalRef(env, DecodedFrameBuffer);
    DecodedFrameBuffer = global;
    return true;
}

int BridgeDrSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    JNIEnv* env = GetThreadEnv();
    int err;

    err = (*env)->CallStaticIntMethod(env, GlobalBridgeClass, BridgeDrSetupMethod, videoFormat, width, height, redrawRate);
    if ((*env)->ExceptionCheck(env)) {
        // This is called on a Java thread, so it's safe to return
        return -1;
    }
    else if (err != 0) {
        return err;
    }

    UseNativeInput = (*env)->CallStaticBooleanMethod(env, GlobalBridgeClass, BridgeDrSupportsNativeMethod);
    if ((*env)->ExceptionCheck(env)) return -1;
    // Picture bytes bypass this array; parameters retain the established Java path.
    if (!ensureScratchArray(env, UseNativeInput ? 0 : 32768)) return -1;
    return 0;
}

void BridgeDrStart(void) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeDrStartMethod);
}

void BridgeDrStop(void) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeDrStopMethod);
}

void BridgeDrCleanup(void) {
    JNIEnv* env = GetThreadEnv();

    if (DecodedFrameBuffer) (*env)->DeleteGlobalRef(env, DecodedFrameBuffer);
    DecodedFrameBuffer = NULL;
    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeDrCleanupMethod);
}

int BridgeDrSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
    if (nativeCodecActive()) return nativeSubmit(decodeUnit);
    JNIEnv* env = GetThreadEnv();
    int ret;

    if (!UseNativeInput && !ensureScratchArray(env, decodeUnit->fullLength)) return DR_NEED_IDR;
    PLENTRY currentEntry;
    int offset;
    currentEntry = decodeUnit->bufferList;
    offset = 0;
    while (currentEntry != NULL) {
        // Submit parameter set NALUs separately from picture data
        if (currentEntry->bufferType != BUFFER_TYPE_PICDATA) {
            if (UseNativeInput && !ensureScratchArray(env, currentEntry->length)) return DR_NEED_IDR;
            // Use the beginning of the buffer each time since this is a separate
            // invocation of the decoder each time.
            (*env)->SetByteArrayRegion(env, DecodedFrameBuffer, 0, currentEntry->length, (jbyte*)currentEntry->data);

            ret = (*env)->CallStaticIntMethod(env, GlobalBridgeClass, BridgeDrSubmitDecodeUnitMethod,
                                              DecodedFrameBuffer, currentEntry->length, currentEntry->bufferType,
                                              decodeUnit->frameNumber, decodeUnit->frameType, (jchar)decodeUnit->frameHostProcessingLatency,
                                              (jlong)decodeUnit->receiveTimeMs, (jlong)decodeUnit->enqueueTimeMs);
            if ((*env)->ExceptionCheck(env)) {
                // We will crash here
                (*JVM)->DetachCurrentThread(JVM);
                return DR_OK;
            }
            else if (ret != DR_OK) {
                return ret;
            }
        }
        else {
            if (!UseNativeInput) {
                (*env)->SetByteArrayRegion(env, DecodedFrameBuffer, offset, currentEntry->length, (jbyte*)currentEntry->data);
            }
            offset += currentEntry->length;
        }

        currentEntry = currentEntry->next;
    }

    if (UseNativeInput) {
        ret = (*env)->CallStaticIntMethod(env, GlobalBridgeClass, BridgeDrSubmitNativeMethod,
                (jlong)(uintptr_t)decodeUnit, offset, decodeUnit->frameNumber, decodeUnit->frameType,
                (jchar)decodeUnit->frameHostProcessingLatency, (jlong)decodeUnit->receiveTimeMs,
                (jlong)decodeUnit->enqueueTimeMs);
    } else {
    ret = (*env)->CallStaticIntMethod(env, GlobalBridgeClass, BridgeDrSubmitDecodeUnitMethod,
                                       DecodedFrameBuffer, offset, BUFFER_TYPE_PICDATA,
                                       decodeUnit->frameNumber, decodeUnit->frameType, (jchar)decodeUnit->frameHostProcessingLatency,
                                       (jlong)decodeUnit->receiveTimeMs, (jlong)decodeUnit->enqueueTimeMs);
    }
    if ((*env)->ExceptionCheck(env)) {
        // We will crash here
        (*JVM)->DetachCurrentThread(JVM);
        return DR_OK;
    }
    else {
        return ret;
    }
}

// A borrowed DU handle never escapes its synchronous native -> Java -> native call.
static bool pictureFits(PDECODE_UNIT du, int length) {
    if (!du || length < 0) return false;
    int sum = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) {
        if (e->bufferType != BUFFER_TYPE_PICDATA) continue;
        if (e->length < 0 || e->length > length - sum || (e->length && !e->data)) return false;
        sum += e->length;
    }
    return sum == length;
}
static int copyPicture(PDECODE_UNIT du, unsigned char* dst, jlong capacity, int offset, int length) {
    if (!dst || offset < 0 || capacity < 0 || offset > capacity || length < 0 || length > capacity-offset || !pictureFits(du,length)) return -2;
    int wrote = 0;
    for (PLENTRY e=du->bufferList;e;e=e->next) {
        if (e->bufferType == BUFFER_TYPE_PICDATA && e->length) {
            memcpy(dst+offset+wrote,e->data,e->length); wrote += e->length;
        }
    }
    return wrote;
}
#include "native_codec.inc"

JNIEXPORT jint JNICALL Java_com_limelight_nvstream_jni_MoonBridge_copyNativePicture(JNIEnv* env,jclass clazz,jlong handle,jobject target,jint offset,jint length) {
    unsigned char* dst=(*env)->GetDirectBufferAddress(env,target);
    if (!dst) return -1;
    return copyPicture((PDECODE_UNIT)(uintptr_t)handle,dst,(*env)->GetDirectBufferCapacity(env,target),offset,length);
}
JNIEXPORT jboolean JNICALL Java_com_limelight_nvstream_jni_MoonBridge_copyNativePictureToArray(JNIEnv* env,jclass clazz,jlong handle,jbyteArray target,jint length) {
    PDECODE_UNIT du=(PDECODE_UNIT)(uintptr_t)handle;
    if ((*env)->GetArrayLength(env,target)<length || !pictureFits(du,length)) return JNI_FALSE;
    int offset=0;
    for(PLENTRY e=du->bufferList;e;e=e->next) if(e->bufferType==BUFFER_TYPE_PICDATA && e->length) {
        (*env)->SetByteArrayRegion(env,target,offset,e->length,(jbyte*)e->data);
        if ((*env)->ExceptionCheck(env)) return JNI_FALSE;
        offset+=e->length;
    }
    return JNI_TRUE;
}
int BridgeArInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int flags) {
    JNIEnv* env = GetThreadEnv();
    int err;

    err = (*env)->CallStaticIntMethod(env, GlobalBridgeClass, BridgeArInitMethod, audioConfiguration, opusConfig->sampleRate, opusConfig->samplesPerFrame);
    if ((*env)->ExceptionCheck(env)) {
        // This is called on a Java thread, so it's safe to return
        err = -1;
    }
    if (err == 0) {
        memcpy(&OpusConfig, opusConfig, sizeof(*opusConfig));
        Decoder = opus_multistream_decoder_create(opusConfig->sampleRate,
                                                  opusConfig->channelCount,
                                                  opusConfig->streams,
                                                  opusConfig->coupledStreams,
                                                  opusConfig->mapping,
                                                  &err);
        if (Decoder == NULL) {
            (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeArCleanupMethod);
            return -1;
        }

        // We know ahead of time what the buffer size will be for decoded audio, so pre-allocate it
        DecodedAudioBuffer = (*env)->NewGlobalRef(env, (*env)->NewShortArray(env, opusConfig->channelCount * opusConfig->samplesPerFrame));
    }

    return err;
}

void BridgeArStart(void) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeArStartMethod);
}

void BridgeArStop(void) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeArStopMethod);
}

void BridgeArCleanup() {
    JNIEnv* env = GetThreadEnv();

    opus_multistream_decoder_destroy(Decoder);

    (*env)->DeleteGlobalRef(env, DecodedAudioBuffer);

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeArCleanupMethod);
}

void BridgeArDecodeAndPlaySample(char* sampleData, int sampleLength) {
    JNIEnv* env = GetThreadEnv();

    jshort* decodedData = (*env)->GetPrimitiveArrayCritical(env, DecodedAudioBuffer, NULL);

    int decodeLen = opus_multistream_decode(Decoder,
                                            (const unsigned char*)sampleData,
                                            sampleLength,
                                            decodedData,
                                            OpusConfig.samplesPerFrame,
                                            0);
    if (decodeLen > 0) {
        // We must release the array elements before making further JNI calls
        (*env)->ReleasePrimitiveArrayCritical(env, DecodedAudioBuffer, decodedData, 0);

        (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeArPlaySampleMethod, DecodedAudioBuffer);
        if ((*env)->ExceptionCheck(env)) {
            // We will crash here
            (*JVM)->DetachCurrentThread(JVM);
        }
    }
    else {
        // We can abort here to avoid the copy back since no data was modified
        (*env)->ReleasePrimitiveArrayCritical(env, DecodedAudioBuffer, decodedData, JNI_ABORT);
    }
}

void BridgeClStageStarting(int stage) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClStageStartingMethod, stage);
}

void BridgeClStageComplete(int stage) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClStageCompleteMethod, stage);
}

void BridgeClStageFailed(int stage, int errorCode) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClStageFailedMethod, stage, errorCode);
}

void BridgeClConnectionStarted(void) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClConnectionStartedMethod);
}

void BridgeClConnectionTerminated(int errorCode) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClConnectionTerminatedMethod, errorCode);
    if ((*env)->ExceptionCheck(env)) {
        // We will crash here
        (*JVM)->DetachCurrentThread(JVM);
    }
}

void BridgeClRumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor) {
    JNIEnv* env = GetThreadEnv();

    // The seemingly redundant short casts are required in order to convert the unsigned short to a signed short.
    // If we leave it as an unsigned short, CheckJNI will fail when the value exceeds 32767. The cast itself is
    // fine because the Java code treats the value as unsigned even though it's stored in a signed type.
    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClRumbleMethod, controllerNumber, (short)lowFreqMotor, (short)highFreqMotor);
    if ((*env)->ExceptionCheck(env)) {
        // We will crash here
        (*JVM)->DetachCurrentThread(JVM);
    }
}

void BridgeClConnectionStatusUpdate(int connectionStatus) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClConnectionStatusUpdateMethod, connectionStatus);
    if ((*env)->ExceptionCheck(env)) {
        // We will crash here
        (*JVM)->DetachCurrentThread(JVM);
        return;
    }
}

void BridgeClSetHdrMode(bool enabled) {
    JNIEnv* env = GetThreadEnv();

    jbyteArray hdrMetadataByteArray = NULL;
    SS_HDR_METADATA hdrMetadata;

    // Check if HDR metadata was provided
    if (enabled && LiGetHdrMetadata(&hdrMetadata)) {
        hdrMetadataByteArray = (*env)->NewByteArray(env, sizeof(SS_HDR_METADATA));
        (*env)->SetByteArrayRegion(env, hdrMetadataByteArray, 0, sizeof(SS_HDR_METADATA), (jbyte*)&hdrMetadata);
    }

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClSetHdrModeMethod, enabled, hdrMetadataByteArray);
    if ((*env)->ExceptionCheck(env)) {
        // We will crash here
        (*JVM)->DetachCurrentThread(JVM);
    }
}

void BridgeClRumbleTriggers(unsigned short controllerNumber, unsigned short leftTrigger, unsigned short rightTrigger) {
    JNIEnv* env = GetThreadEnv();

    // The seemingly redundant short casts are required in order to convert the unsigned short to a signed short.
    // If we leave it as an unsigned short, CheckJNI will fail when the value exceeds 32767. The cast itself is
    // fine because the Java code treats the value as unsigned even though it's stored in a signed type.
    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClRumbleTriggersMethod, controllerNumber, (short)leftTrigger, (short)rightTrigger);
    if ((*env)->ExceptionCheck(env)) {
        // We will crash here
        (*JVM)->DetachCurrentThread(JVM);
    }
}

void BridgeClSetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz) {
    JNIEnv* env = GetThreadEnv();

    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClSetMotionEventStateMethod, controllerNumber, motionType, reportRateHz);
    if ((*env)->ExceptionCheck(env)) {
        // We will crash here
        (*JVM)->DetachCurrentThread(JVM);
    }
}

void BridgeClSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) {
    JNIEnv* env = GetThreadEnv();

    // These jbyte casts are necessary to satisfy CheckJNI
    (*env)->CallStaticVoidMethod(env, GlobalBridgeClass, BridgeClSetControllerLEDMethod, controllerNumber, (jbyte)r, (jbyte)g, (jbyte)b);
    if ((*env)->ExceptionCheck(env)) {
        // We will crash here
        (*JVM)->DetachCurrentThread(JVM);
    }
}

void BridgeClLogMessage(const char* format, ...) {
    va_list va;
    va_start(va, format);
    __android_log_vprint(ANDROID_LOG_INFO, "moonlight-common-c", format, va);
    va_end(va);
}

static DECODER_RENDERER_CALLBACKS BridgeVideoRendererCallbacks = {
        .setup = BridgeDrSetup,
        .start = BridgeDrStart,
        .stop = BridgeDrStop,
        .cleanup = BridgeDrCleanup,
        .submitDecodeUnit = BridgeDrSubmitDecodeUnit,
};

static AUDIO_RENDERER_CALLBACKS BridgeAudioRendererCallbacks = {
        .init = BridgeArInit,
        .start = BridgeArStart,
        .stop = BridgeArStop,
        .cleanup = BridgeArCleanup,
        .decodeAndPlaySample = BridgeArDecodeAndPlaySample,
        .capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION
};

static CONNECTION_LISTENER_CALLBACKS BridgeConnListenerCallbacks = {
        .stageStarting = BridgeClStageStarting,
        .stageComplete = BridgeClStageComplete,
        .stageFailed = BridgeClStageFailed,
        .connectionStarted = BridgeClConnectionStarted,
        .connectionTerminated = BridgeClConnectionTerminated,
        .logMessage = BridgeClLogMessage,
        .rumble = BridgeClRumble,
        .connectionStatusUpdate = BridgeClConnectionStatusUpdate,
        .setHdrMode = BridgeClSetHdrMode,
        .rumbleTriggers = BridgeClRumbleTriggers,
        .setMotionEventState = BridgeClSetMotionEventState,
        .setControllerLED = BridgeClSetControllerLED,
};

static bool
hasFastAes() {
    if (android_getCpuCount() <= 2) {
        return false;
    }

    switch (android_getCpuFamily()) {
        case ANDROID_CPU_FAMILY_ARM:
            return !!(android_getCpuFeatures() & ANDROID_CPU_ARM_FEATURE_AES);
        case ANDROID_CPU_FAMILY_ARM64:
            return !!(android_getCpuFeatures() & ANDROID_CPU_ARM64_FEATURE_AES);
        case ANDROID_CPU_FAMILY_X86:
        case ANDROID_CPU_FAMILY_X86_64:
            return !!(android_getCpuFeatures() & ANDROID_CPU_X86_FEATURE_AES_NI);
        case ANDROID_CPU_FAMILY_MIPS:
        case ANDROID_CPU_FAMILY_MIPS64:
            return false;
        default:
            // Assume new architectures will all have crypto acceleration (RISC-V will)
            return true;
    }
}

JNIEXPORT jint JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_startConnection(JNIEnv *env, jclass clazz,
                                                           jstring address, jstring appVersion, jstring gfeVersion,
                                                           jstring rtspSessionUrl, jint serverCodecModeSupport,
                                                           jint width, jint height, jint fps,
                                                           jint bitrate, jint packetSize, jint streamingRemotely,
                                                           jint audioConfiguration, jint supportedVideoFormats,
                                                           jint clientRefreshRateX100,
                                                           jbyteArray riAesKey, jbyteArray riAesIv,
                                                           jint videoCapabilities,
                                                           jint colorSpace, jint colorRange) {
    SERVER_INFORMATION serverInfo = {
            .address = (*env)->GetStringUTFChars(env, address, 0),
            .serverInfoAppVersion = (*env)->GetStringUTFChars(env, appVersion, 0),
            .serverInfoGfeVersion = gfeVersion ? (*env)->GetStringUTFChars(env, gfeVersion, 0) : NULL,
            .rtspSessionUrl = rtspSessionUrl ? (*env)->GetStringUTFChars(env, rtspSessionUrl, 0) : NULL,
            .serverCodecModeSupport = serverCodecModeSupport,
    };
    STREAM_CONFIGURATION streamConfig = {
            .width = width,
            .height = height,
            .fps = fps,
            .bitrate = bitrate,
            .packetSize = packetSize,
            .streamingRemotely = streamingRemotely,
            .audioConfiguration = audioConfiguration,
            .supportedVideoFormats = supportedVideoFormats,
            .clientRefreshRateX100 = clientRefreshRateX100,
            .encryptionFlags = ENCFLG_AUDIO,
            .colorSpace = colorSpace,
            .colorRange = colorRange
    };

    jbyte* riAesKeyBuf = (*env)->GetByteArrayElements(env, riAesKey, NULL);
    memcpy(streamConfig.remoteInputAesKey, riAesKeyBuf, sizeof(streamConfig.remoteInputAesKey));
    (*env)->ReleaseByteArrayElements(env, riAesKey, riAesKeyBuf, JNI_ABORT);

    jbyte* riAesIvBuf = (*env)->GetByteArrayElements(env, riAesIv, NULL);
    memcpy(streamConfig.remoteInputAesIv, riAesIvBuf, sizeof(streamConfig.remoteInputAesIv));
    (*env)->ReleaseByteArrayElements(env, riAesIv, riAesIvBuf, JNI_ABORT);

    BridgeVideoRendererCallbacks.capabilities = videoCapabilities;

    // Enable all encryption features if the platform has fast AES support
    if (hasFastAes()) {
        streamConfig.encryptionFlags = ENCFLG_ALL;
    }

    int ret = LiStartConnection(&serverInfo,
                                &streamConfig,
                                &BridgeConnListenerCallbacks,
                                &BridgeVideoRendererCallbacks,
                                &BridgeAudioRendererCallbacks,
                                NULL, 0,
                                NULL, 0);

    (*env)->ReleaseStringUTFChars(env, address, serverInfo.address);
    (*env)->ReleaseStringUTFChars(env, appVersion, serverInfo.serverInfoAppVersion);
    if (gfeVersion != NULL) {
        (*env)->ReleaseStringUTFChars(env, gfeVersion, serverInfo.serverInfoGfeVersion);
    }
    if (rtspSessionUrl != NULL) {
        (*env)->ReleaseStringUTFChars(env, rtspSessionUrl, serverInfo.rtspSessionUrl);
    }

    return ret;
}