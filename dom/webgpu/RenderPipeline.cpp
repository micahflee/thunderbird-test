/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderPipeline.h"

#include "Device.h"
#include "ipc/WebGPUChild.h"
#include "mozilla/dom/WebGPUBinding.h"

namespace mozilla::webgpu {

GPU_IMPL_CYCLE_COLLECTION(RenderPipeline, mParent)
GPU_IMPL_JS_WRAP(RenderPipeline)

RenderPipeline::RenderPipeline(Device* const aParent, RawId aId,
                               RawId aImplicitPipelineLayoutId,
                               nsTArray<RawId>&& aImplicitBindGroupLayoutIds)
    : ChildOf(aParent),
      mImplicitPipelineLayoutId(aImplicitPipelineLayoutId),
      mImplicitBindGroupLayoutIds(std::move(aImplicitBindGroupLayoutIds)),
      mId(aId) {}

RenderPipeline::~RenderPipeline() { Cleanup(); }

void RenderPipeline::Cleanup() {
  if (mValid && mParent) {
    mValid = false;
    auto bridge = mParent->GetBridge();
    if (bridge && bridge->IsOpen()) {
      bridge->SendRenderPipelineDestroy(mId);
      if (mImplicitPipelineLayoutId) {
        bridge->SendImplicitLayoutDestroy(mImplicitPipelineLayoutId,
                                          mImplicitBindGroupLayoutIds);
      }
    }
  }
}

already_AddRefed<BindGroupLayout> RenderPipeline::GetBindGroupLayout(
    uint32_t index) const {
  const RawId id = index < mImplicitBindGroupLayoutIds.Length()
                       ? mImplicitBindGroupLayoutIds[index]
                       : 0;
  RefPtr<BindGroupLayout> object = new BindGroupLayout(mParent, id, false);
  return object.forget();
}

}  // namespace mozilla::webgpu
