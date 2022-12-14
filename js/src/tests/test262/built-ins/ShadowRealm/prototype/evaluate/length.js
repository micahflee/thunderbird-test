// |reftest| shell-option(--enable-shadow-realms) skip-if(!xulRuntime.shell) -- requires shell-options
// Copyright (C) 2021 Rick Waldron. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: sec-shadowrealm.prototype.evaluate
description: >
  The value of ShadowRealm.prototype.evaluate.length is 1
info: |
  ShadowRealm.prototype.evaluate ( sourceText )

  Every built-in function object, including constructors, has a "length" property
  whose value is a non-negative integral Number. Unless otherwise specified, this value
  is equal to the number of required parameters shown in the subclause heading for the
  function description. Optional parameters and rest parameters are not included in
  the parameter count.

  Unless otherwise specified, the "length" property of a built-in function object has
  the attributes { [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }.

includes: [propertyHelper.js]
features: [ShadowRealm]
---*/

verifyProperty(ShadowRealm.prototype.evaluate, "length", {
  value: 1,
  enumerable: false,
  writable: false,
  configurable: true,
});

reportCompare(0, 0);
