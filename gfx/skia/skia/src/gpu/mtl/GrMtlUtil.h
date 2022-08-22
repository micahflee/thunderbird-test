/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrMtlUtil_DEFINED
#define GrMtlUtil_DEFINED

#import <Metal/Metal.h>

#include "include/gpu/GrBackendSurface.h"
#include "include/private/GrTypesPriv.h"
#include "src/sksl/ir/SkSLProgram.h"

#if !__has_feature(objc_arc)
#error This file must be compiled with Arc. Use -fobjc-arc flag
#endif

#if defined(SK_BUILD_FOR_MAC)
#if __MAC_OS_X_VERSION_MAX_ALLOWED < 101400
#error Must use at least 10.14 SDK to build Metal backend for MacOS
#endif
#else
#if __IPHONE_OS_VERSION_MAX_ALLOWED < 120000 && __TV_OS_VERSION_MAX_ALLOWED < 120000
#error Must use at least 12.00 SDK to build Metal backend for iOS
#endif
#endif

class GrMtlGpu;
class GrSurface;

/**
 * Returns the Metal texture format for the given GrPixelConfig
 */
bool GrPixelConfigToMTLFormat(GrPixelConfig config, MTLPixelFormat* format);

/**
 * Returns a id<MTLTexture> to the MTLTexture pointed at by the const void*.
 */
SK_ALWAYS_INLINE id<MTLTexture> GrGetMTLTexture(const void* mtlTexture)  {
    return (__bridge id<MTLTexture>)mtlTexture;
}

/**
 * Returns a const void* to whatever the id object is pointing to.
 */
SK_ALWAYS_INLINE const void* GrGetPtrFromId(id idObject) {
    return (__bridge const void*)idObject;
}

/**
 * Returns a const void* to whatever the id object is pointing to.
 * Will call CFRetain on the object.
 */
SK_ALWAYS_INLINE const void* GrRetainPtrFromId(id idObject) {
    return (__bridge_retained const void*)idObject;
}


/**
 * Returns a MTLTextureDescriptor which describes the MTLTexture. Useful when creating a duplicate
 * MTLTexture without the same storage allocation.
 */
MTLTextureDescriptor* GrGetMTLTextureDescriptor(id<MTLTexture> mtlTexture);

/**
 * Returns a compiled MTLLibrary created from MSL code generated by SkSLC
 */
id<MTLLibrary> GrCompileMtlShaderLibrary(const GrMtlGpu* gpu,
                                         const char* shaderString,
                                         SkSL::Program::Kind kind,
                                         const SkSL::Program::Settings& settings,
                                         SkSL::Program::Inputs* outInputs);

/**
 * Replacement for newLibraryWithSource:options:error that has a timeout.
 */
id<MTLLibrary> GrMtlNewLibraryWithSource(id<MTLDevice>, NSString* mslCode,
                                         MTLCompileOptions*, bool* timedout);

/**
 * Replacement for newRenderPipelineStateWithDescriptor:error that has a timeout.
 */
id<MTLRenderPipelineState> GrMtlNewRenderPipelineStateWithDescriptor(
        id<MTLDevice>, MTLRenderPipelineDescriptor*, bool* timedout);

/**
 * Returns a MTLTexture corresponding to the GrSurface.
 */
id<MTLTexture> GrGetMTLTextureFromSurface(GrSurface* surface);

static inline MTLPixelFormat GrBackendFormatAsMTLPixelFormat(const GrBackendFormat& format) {
    return static_cast<MTLPixelFormat>(format.asMtlFormat());
}

/**
 * Returns true if the format is compressed.
 */
bool GrMtlFormatIsCompressed(MTLPixelFormat mtlFormat);

/**
 * Maps a MTLPixelFormat into the CompressionType enum if applicable.
 */
bool GrMtlFormatToCompressionType(MTLPixelFormat mtlFormat,
                                  SkImage::CompressionType* compressionType);
#endif
