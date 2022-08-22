/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DOMSVGAnimatedString.h"

#include "mozilla/dom/SVGAnimatedStringBinding.h"

namespace mozilla::dom {

NS_SVG_VAL_IMPL_CYCLE_COLLECTION_WRAPPERCACHED(DOMSVGAnimatedString,
                                               mSVGElement)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(DOMSVGAnimatedString, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(DOMSVGAnimatedString, Release)

JSObject* DOMSVGAnimatedString::WrapObject(JSContext* aCx,
                                           JS::Handle<JSObject*> aGivenProto) {
  return SVGAnimatedString_Binding::Wrap(aCx, this, aGivenProto);
}

}  // namespace mozilla::dom
