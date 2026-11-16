// NDK input backend for the tuned MediaTek HEVC SDR configuration.
// Input submission is native; output callbacks post to the Java pacing thread.
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <errno.h>

typedef media_status_t (*SetAsyncFn)(AMediaCodec*, AMediaCodecOnAsyncNotifyCallback, void*);
static struct {
    AMediaCodec* codec;
    ANativeWindow* window;
    jobject renderer;
    jmethodID metadata, output, error;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    int inputs[64], head, count, held;
    uint8_t* inputData; size_t inputCapacity;
    _Atomic bool stopping;
    bool started, initialized, fused, submittedCsd;
    int64_t lastPts;
    SetAsyncFn setAsync;
} nc;
static bool nativeCodecActive(void) { return nc.codec != NULL; }
static void nativeError(const char* message) {
    atomic_store(&nc.stopping,true);
    pthread_mutex_lock(&nc.lock); pthread_cond_broadcast(&nc.ready); pthread_mutex_unlock(&nc.lock);
    JNIEnv* env=GetThreadEnv();
    jstring s=(*env)->NewStringUTF(env,message);
    if(s) { (*env)->CallVoidMethod(env,nc.renderer,nc.error,s); (*env)->DeleteLocalRef(env,s); }
    if((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
}
static void inputAvailable(AMediaCodec* codec,void* u,int32_t index) {
    (void)u;
    if(atomic_load(&nc.stopping)) return;
    pthread_mutex_lock(&nc.lock);
    bool overflow=nc.count==64;
    if(!overflow) { nc.inputs[(nc.head+nc.count)%64]=index; nc.count++; pthread_cond_signal(&nc.ready); }
    pthread_mutex_unlock(&nc.lock);
    if(overflow) nativeError("NDK input queue overflow");
}
static void outputAvailable(AMediaCodec* codec,void* u,int32_t index,AMediaCodecBufferInfo* info) {
    (void)codec;(void)u;
    if(atomic_load(&nc.stopping)) return;
    JNIEnv* env=GetThreadEnv();
    (*env)->CallVoidMethod(env,nc.renderer,nc.output,index,(jlong)info->presentationTimeUs);
    if((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); nativeError("NDK output callback exception"); }
}
static void formatChanged(AMediaCodec* codec,void* u,AMediaFormat* f) {
    (void)codec;(void)u;
    __android_log_print(ANDROID_LOG_INFO,"LimeLog","NDK output format: %s",AMediaFormat_toString(f));
}
static void codecError(AMediaCodec* codec,void* u,media_status_t error,int32_t action,const char* detail) {
    (void)codec;(void)u;(void)error;(void)action;
    nativeError(detail?detail:"NDK codec error");
}
JNIEXPORT void JNICALL Java_com_limelight_nvstream_jni_MoonBridge_nativeCodecPrepareStop(JNIEnv* env,jclass c) {
    (void)env;(void)c;
    if(!nc.initialized) return;
    atomic_store(&nc.stopping,true);
    pthread_mutex_lock(&nc.lock); pthread_cond_broadcast(&nc.ready); pthread_mutex_unlock(&nc.lock);
}
JNIEXPORT void JNICALL Java_com_limelight_nvstream_jni_MoonBridge_nativeCodecClose(JNIEnv* env,jclass c) {
    Java_com_limelight_nvstream_jni_MoonBridge_nativeCodecPrepareStop(env,c);
    if(nc.codec) {
        if(nc.started) AMediaCodec_stop(nc.codec);
        AMediaCodecOnAsyncNotifyCallback empty={0};
        if(nc.setAsync) nc.setAsync(nc.codec,empty,NULL);
        AMediaCodec_delete(nc.codec); nc.codec=NULL;
    }
    if(nc.renderer) (*env)->DeleteGlobalRef(env,nc.renderer);
    if(nc.window) ANativeWindow_release(nc.window);
    if(nc.initialized) { pthread_mutex_destroy(&nc.lock); pthread_cond_destroy(&nc.ready); }
    memset(&nc,0,sizeof(nc));
}
JNIEXPORT jboolean JNICALL Java_com_limelight_nvstream_jni_MoonBridge_nativeCodecCreate(JNIEnv* env,jclass c,jstring name,jobject surface,jobjectArray keys,jintArray types,jobjectArray values,jobject renderer,jboolean fused) {
    if(nc.initialized) return JNI_FALSE;
    memset(&nc,0,sizeof(nc)); nc.held=-1; nc.fused=fused;
    pthread_mutex_init(&nc.lock,NULL);
    pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca,CLOCK_MONOTONIC);
    pthread_cond_init(&nc.ready,&ca); pthread_condattr_destroy(&ca); nc.initialized=true;
    nc.renderer=(*env)->NewGlobalRef(env,renderer);
    jclass cls=(*env)->GetObjectClass(env,renderer);
    nc.metadata=(*env)->GetMethodID(env,cls,"nativeFrameMetadata","(IICJJJ)V");
    nc.output=(*env)->GetMethodID(env,cls,"nativeOutput","(IJ)V");
    nc.error=(*env)->GetMethodID(env,cls,"nativeCodecError","(Ljava/lang/String;)V");
    (*env)->DeleteLocalRef(env,cls);
    if(!nc.renderer||!nc.metadata||!nc.output||!nc.error) goto fail;
    nc.window=ANativeWindow_fromSurface(env,surface);
    nc.setAsync=(SetAsyncFn)dlsym(RTLD_DEFAULT,"AMediaCodec_setAsyncNotifyCallback");
    const char* n=(*env)->GetStringUTFChars(env,name,NULL);
    if(!n) goto fail;
    nc.codec=AMediaCodec_createCodecByName(n); (*env)->ReleaseStringUTFChars(env,name,n);
    if(!nc.codec||!nc.window||!nc.setAsync) goto fail;
    AMediaFormat* f=AMediaFormat_new();
    if(!f) goto fail;
    int count=(*env)->GetArrayLength(env,keys);
    if((*env)->GetArrayLength(env,values)!=count||(*env)->GetArrayLength(env,types)!=count) { AMediaFormat_delete(f); goto fail; }
    for(int i=0;i<count;i++) {
        jstring key=(*env)->GetObjectArrayElement(env,keys,i),value=(*env)->GetObjectArrayElement(env,values,i); jint type;
        (*env)->GetIntArrayRegion(env,types,i,1,&type);
        const char* k=(*env)->GetStringUTFChars(env,key,NULL); const char* v=(*env)->GetStringUTFChars(env,value,NULL);
        if(k&&v) switch(type) {
            case 1: AMediaFormat_setInt32(f,k,strtol(v,NULL,10));break;
            case 2: AMediaFormat_setInt64(f,k,strtoll(v,NULL,10));break;
            case 3: AMediaFormat_setFloat(f,k,strtof(v,NULL));break;
            case 4: AMediaFormat_setString(f,k,v);break;
        }
        if(k) (*env)->ReleaseStringUTFChars(env,key,k);
        if(v) (*env)->ReleaseStringUTFChars(env,value,v);
        (*env)->DeleteLocalRef(env,key);(*env)->DeleteLocalRef(env,value);
    }
    __android_log_print(ANDROID_LOG_INFO,"LimeLog","NDK configure: %s fused=%d",AMediaFormat_toString(f),nc.fused);
    AMediaCodecOnAsyncNotifyCallback cb={inputAvailable,outputAvailable,formatChanged,codecError};
    media_status_t status=nc.setAsync(nc.codec,cb,&nc);
    if(status==AMEDIA_OK) status=AMediaCodec_configure(nc.codec,f,nc.window,NULL,0);
    AMediaFormat_delete(f);
    if(status!=AMEDIA_OK) goto fail;
    status=AMediaCodec_start(nc.codec);
    if(status!=AMEDIA_OK) goto fail;
    nc.started=true;
    typedef AMediaFormat* (*GetInputFormatFn)(AMediaCodec*);
    GetInputFormatFn getInput=(GetInputFormatFn)dlsym(RTLD_DEFAULT,"AMediaCodec_getInputFormat");
    if(getInput) { AMediaFormat* accepted=getInput(nc.codec); if(accepted) { __android_log_print(ANDROID_LOG_INFO,"LimeLog","NDK accepted input: %s",AMediaFormat_toString(accepted));AMediaFormat_delete(accepted); } }
    return JNI_TRUE;
fail:
    Java_com_limelight_nvstream_jni_MoonBridge_nativeCodecClose(env,c);
    return JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_limelight_nvstream_jni_MoonBridge_nativeCodecRelease(JNIEnv* env,jclass c,jint index,jlong target) {
    (void)c;
    if(!nc.codec||atomic_load(&nc.stopping)) return;
    media_status_t status=target<0 ? AMediaCodec_releaseOutputBuffer(nc.codec,index,false) : AMediaCodec_releaseOutputBufferAtTime(nc.codec,index,target);
    if(status!=AMEDIA_OK) {
        jclass ex=(*env)->FindClass(env,"java/lang/IllegalStateException");
        if(ex) (*env)->ThrowNew(env,ex,"NDK release output failed");
    }
}
static bool obtainInput(void) {
    if(nc.held>=0) return true;
    struct timespec deadline; clock_gettime(CLOCK_MONOTONIC,&deadline); deadline.tv_sec+=5;
    pthread_mutex_lock(&nc.lock);
    while(!nc.count&&!atomic_load(&nc.stopping)) {
        int err=pthread_cond_timedwait(&nc.ready,&nc.lock,&deadline);
        if(err==ETIMEDOUT) break;
    }
    if(nc.count&&!atomic_load(&nc.stopping)) { nc.held=nc.inputs[nc.head]; nc.head=(nc.head+1)%64;nc.count--; }
    pthread_mutex_unlock(&nc.lock);
    if(nc.held>=0 && !atomic_load(&nc.stopping)) nc.inputData=AMediaCodec_getInputBuffer(nc.codec,nc.held,&nc.inputCapacity);
    if(nc.held<0&&!atomic_load(&nc.stopping)) nativeError("NDK input wait exceeded 5s");
    return nc.held>=0&&!atomic_load(&nc.stopping);
}
static int queueNative(size_t bytes,int64_t pts,uint32_t flags) {
    media_status_t status=AMediaCodec_queueInputBuffer(nc.codec,nc.held,0,bytes,pts,flags);
    nc.held=-1;
    if(status!=AMEDIA_OK) { nativeError("NDK queue input failed"); return DR_NEED_IDR; }
    // Match the installed path: prefetch after queueing, not a new optimization.
    return obtainInput()?DR_OK:DR_NEED_IDR;
}
static int nativeSubmit(void* opaque) {
    PDECODE_UNIT du=opaque;
    if(atomic_load(&nc.stopping)) return DR_OK;
    bool idr=du->frameType==FRAME_TYPE_IDR;
    size_t csd=0; int pic=0;
    for(PLENTRY e=du->bufferList;e;e=e->next) {
        if(e->length<0||(e->length&&!e->data)) { nativeError("Invalid DU fragment");return DR_NEED_IDR; }
        if(e->bufferType==BUFFER_TYPE_PICDATA) {
            if(e->length>INT32_MAX-pic) return DR_NEED_IDR;
            pic+=e->length;
        } else { if((size_t)e->length>SIZE_MAX-csd) return DR_NEED_IDR; csd+=e->length; }
    }
    if(!obtainInput()) return DR_NEED_IDR;
    size_t capacity=nc.inputCapacity; uint8_t* dst=nc.inputData;
    if(!dst||csd>capacity) { nativeError("NDK CSD buffer bounds");return DR_NEED_IDR; }
    size_t prefix=0;
    if(idr&&csd) {
        // Same grouping as Java: VPS, SPS, PPS; initial CSD separately, later fused if supported.
        const int types[]={BUFFER_TYPE_VPS,BUFFER_TYPE_SPS,BUFFER_TYPE_PPS};
        for(int t=0;t<3;t++)
            for(PLENTRY e=du->bufferList;e;e=e->next) if(e->bufferType==types[t]) {memcpy(dst+prefix,e->data,e->length);prefix+=e->length;}
        if(!nc.submittedCsd||!nc.fused) {
            if(queueNative(prefix,0,AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)!=DR_OK) return DR_NEED_IDR;
            nc.submittedCsd=true;prefix=0;
            dst=nc.inputData; capacity=nc.inputCapacity;
        }
    }
    int copied=prefix<=INT32_MAX ? copyPicture(du,dst,capacity,(int)prefix,pic) : -2;
    if(copied!=pic) {nativeError("NDK picture bounds");return DR_NEED_IDR;}
    int64_t pts=du->enqueueTimeMs*1000;
    if(pts<=nc.lastPts) pts=nc.lastPts+1;
    nc.lastPts=pts;
    JNIEnv* env=GetThreadEnv();
    (*env)->CallVoidMethod(env,nc.renderer,nc.metadata,du->frameNumber,du->frameType,(jchar)du->frameHostProcessingLatency,(jlong)du->receiveTimeMs,(jlong)du->enqueueTimeMs,(jlong)pts);
    if((*env)->ExceptionCheck(env)) {(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);nativeError("NDK metadata exception");return DR_NEED_IDR;}
    if (atomic_load(&nc.stopping)) return DR_OK;
    return queueNative(prefix+pic,pts,idr?AMEDIACODEC_BUFFER_FLAG_KEY_FRAME:0);
}
